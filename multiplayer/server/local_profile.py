"""Persist verified loadouts, personalization and launch Boost inventory on this PC.

The bridge calls these methods only for successful HTTP 200 gateway responses.
Server rank, statistics and other profile fields remain server-owned.
"""
from contextlib import contextmanager
from hashlib import sha256
import json
import os
from pathlib import Path
import re
import tempfile
from threading import Lock
from uuid import UUID, uuid4
import xml.etree.ElementTree as ET

from player_identity import validate_nickname
from boost_inventory import (BOOST_NAMESPACE, apply_deltas, launch_inventory,
                             parse_delta, validate_inventory)
from room_http import MAX_BODY, _NoDTDBuilder, validate_http_request
from xml_storage import XSI

TYPE = f'{{{XSI}}}type'
SLOT_FIELDS = ('iD', 'name', 'careerID', 'primaryWeapon', 'secondaryWeapon',
               'auxiliaryWeapon', 'primaryAbility', 'secondaryAbility', 'tertiaryAbility')
SLOT_TAGS = {f'cl_{index}': index for index in range(16)}
PERSONALIZATION = json.loads((Path(__file__).with_name('fixtures') /
                             'native-personalization-schema.json').read_text(encoding='utf-8'))['fields']
# Captured together with the complete local slots in native-profile-mixed-update.xml.
# These fields are recognized only for separating ownership, never applied here.
SERVER_SCALARS = {'{c}Total': 0x7fffffff, 'valor': 0x7fffffff,
                  'clc': 0x7fffffff, 'lct': 0x7fffffffffffffff}
MAX_SAVED_BYTES = 256 * 1024
MAX_REPLAYS = 256
# shortcut: Keep at most 1024 unacknowledged mixed UUIDs without eviction. If
# real-client traces reach this cap, implement verified server handling instead
# of discarding pending history and allowing an old snapshot to overwrite edits.
MAX_PENDING = 1024
_PROCESS_LOCK = Lock()


def _xml(body, tag):
    if not isinstance(body, bytes) or len(body) > MAX_BODY:
        raise ValueError('Invalid loadout XML size')
    root = ET.fromstring(body, parser=ET.XMLParser(target=_NoDTDBuilder()))
    if root.tag != tag:
        raise ValueError('Invalid loadout XML root')
    return root


def _integer(text, lower, upper, digits=10):
    if not isinstance(text, str) or not re.fullmatch(r'-?[0-9]{1,' + str(digits) + '}', text):
        raise ValueError('Invalid loadout integer')
    value = int(text)
    if not lower <= value <= upper:
        raise ValueError('Loadout integer out of range')
    return value


def _update_uuid(identifier):
    if not isinstance(identifier, str) or len(identifier) != 36 or str(UUID(identifier)) != identifier:
        raise ValueError('Invalid local profile update UUID')
    return identifier


def _slot(values, index):
    if not isinstance(values, dict) or set(values) != set(SLOT_FIELDS):
        raise ValueError('Unsupported loadout fields')
    if not isinstance(values['name'], str) or len(values['name'].encode('utf-8')) > 256:
        raise ValueError('Invalid loadout name')
    if any(ord(char) < 32 for char in values['name']):
        raise ValueError('Invalid loadout name character')
    for field in SLOT_FIELDS:
        if field == 'name':
            continue
        value = values[field]
        if type(value) is not int or not -1 <= value <= 0x7fffffff:
            raise ValueError('Invalid loadout field')
    if values['iD'] != index or not 0 <= values['careerID'] <= 3:
        raise ValueError('Loadout slot identity mismatch')
    return values


def _personalization(field, value):
    if field not in PERSONALIZATION:
        raise ValueError('Unknown personalization field')
    if PERSONALIZATION[field]['type'] == 'bool':
        if type(value) is not bool:
            raise ValueError('Invalid personalization boolean')
    elif type(value) is not int or not -1 <= value <= 0x7fffffff:
        raise ValueError('Invalid personalization ID')
    return value


def _delta(data):
    if data is None or data.attrib != {'type': 'profile'} or not len(data):
        raise ValueError('Unsupported loadout profile')
    result = {'loadouts': {}, 'personalization': {}}
    server_owned = False
    seen = set()
    for child in data:
        if child.tag in seen:
            raise ValueError('Duplicate loadout field')
        seen.add(child.tag)
        if child.tag == 'loadoutID' and not child.attrib and not len(child):
            result['loadoutID'] = _integer(child.text, 0, 15)
        elif child.tag in PERSONALIZATION and not child.attrib and not len(child):
            if PERSONALIZATION[child.tag]['type'] == 'bool':
                if child.text not in ('true', 'false', '1', '0'):
                    raise ValueError('Invalid personalization boolean')
                value = child.text in ('true', '1')
            else:
                value = _integer(child.text, -1, 0x7fffffff)
            result['personalization'][child.tag] = _personalization(child.tag, value)
        elif child.tag in SLOT_TAGS and child.attrib == {TYPE: 'loadout'}:
            values = {}
            for field in child:
                if field.tag not in SLOT_FIELDS or field.tag in values or field.attrib or len(field):
                    raise ValueError('Unsupported nested loadout delta')
                values[field.tag] = (field.text or '') if field.tag == 'name' else _integer(field.text, -1, 0x7fffffff)
            index = SLOT_TAGS[child.tag]
            result['loadouts'][str(index)] = _slot(values, index)
        elif (child.tag in SERVER_SCALARS or child.tag.startswith('{c}')) and not child.attrib and not len(child):
            # Native ChallengesCompletedMap keys share the c namespace.
            _integer(child.text, 0, SERVER_SCALARS.get(child.tag, 0x7fffffff), digits=19)
            server_owned = True
        elif child.tag.startswith('{' + BOOST_NAMESPACE + '}'):
            key, delta = parse_delta(child)
            # Preserve digests of earlier updates that contain no Boost fields.
            result.setdefault('boost_deltas', {})[key] = delta
        else:
            raise ValueError('Update contains fields not owned by local loadouts')
    if (not result['loadouts'] and not result['personalization'] and 'loadoutID' not in result
            and 'boost_deltas' not in result):
        raise ValueError('Update contains no local profile fields')
    return result, server_owned


@contextmanager
def _storage_lock(directory):
    # Two local game windows share this one PC profile. Serialize the short
    # read/merge/replace transaction across both threads and Windows processes.
    with _PROCESS_LOCK:
        directory.mkdir(parents=True, exist_ok=True)
        with (directory / '.profile.lock').open('a+b') as handle:
            if handle.tell() == 0:
                handle.write(b'\0')
                handle.flush()
            handle.seek(0)
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(handle.fileno(), msvcrt.LK_LOCK, 1)
            else:
                import fcntl
                fcntl.flock(handle, fcntl.LOCK_EX)
            try:
                yield
            finally:
                handle.seek(0)
                if os.name == 'nt':
                    msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    fcntl.flock(handle, fcntl.LOCK_UN)


class LocalProfile:
    def __init__(self, directory, nickname):
        self.directory = Path(directory)
        self.filename = self.directory / 'profile.json'
        self.owner = 'profile/' + validate_nickname(nickname)
        self.nickname = nickname

    @classmethod
    def reset_boosts(cls, directory):
        """Allocate five of each Boost once per game launch, preserving PC choices."""
        store = cls(directory, 'Player1')  # Reset is PC-wide; no wire owner is used.
        with _storage_lock(store.directory):
            state = store._read()
            state['boosts'] = launch_inventory()
            state['boost_generation'] = state.get('boost_generation', 0) + 1
            store._write(state)

    def capture_boost_generation(self):
        """Capture once before awaiting requests on a game's HTTP connection."""
        with _storage_lock(self.directory):
            return self._read().get('boost_generation', 0)

    def _read(self):
        if not self.filename.exists():
            return {'version': 1, 'loadouts': {}, 'personalization': {}, 'applied': {}, 'pending': {}}
        try:
            if self.filename.stat().st_size > MAX_SAVED_BYTES:
                raise ValueError('Local loadouts exceed limit')
            state = json.loads(self.filename.read_text(encoding='utf-8'))
            if (not isinstance(state, dict) or type(state.get('version')) is not int or state['version'] != 1
                    or set(state) - {'version', 'loadoutID', 'loadouts', 'personalization',
                                    'applied', 'pending', 'boosts', 'boost_generation'}):
                raise ValueError('Invalid local loadouts format')
            if 'loadoutID' in state:
                if type(state['loadoutID']) is not int or not 0 <= state['loadoutID'] <= 15:
                    raise ValueError('Invalid saved loadout selection')
            slots = state['loadouts']
            if not isinstance(slots, dict) or len(slots) > 16:
                raise ValueError('Invalid saved loadouts')
            for index, values in slots.items():
                if index not in {str(i) for i in range(16)}:
                    raise ValueError('Invalid saved loadout slot')
                _slot(values, int(index))
            personalization = state['personalization']
            if not isinstance(personalization, dict):
                raise ValueError('Invalid saved personalization')
            for field, value in personalization.items():
                _personalization(field, value)
            if 'boosts' in state:
                validate_inventory(state['boosts'])
            generation = state.get('boost_generation', 0)
            if type(generation) is not int or generation < 0:
                raise ValueError('Invalid saved Boost generation')
            # Existing version1 files predate mixed update support.
            state.setdefault('pending', {})
            for history, limit in ((state['applied'], MAX_REPLAYS), (state['pending'], MAX_PENDING)):
                if not isinstance(history, dict) or len(history) > limit:
                    raise ValueError('Invalid loadout replay history')
                for identifier, digest in history.items():
                    _update_uuid(identifier)
                    if not isinstance(digest, str) or not re.fullmatch('[0-9a-f]{64}', digest):
                        raise ValueError('Invalid loadout replay digest')
            if state['applied'].keys() & state['pending'].keys():
                raise ValueError('Duplicate local profile update history')
            return state
        except (ValueError, KeyError, TypeError, UnicodeError) as error:
            raise OSError('Saved local loadouts are invalid; file was not changed') from error

    def _write(self, state):
        encoded = (json.dumps(state, ensure_ascii=False, indent=2) + '\n').encode('utf-8')
        if len(encoded) > MAX_SAVED_BYTES:
            raise OSError('Local profile exceeds saved size limit; file was not changed')
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=self.directory, suffix='.tmp', delete=False) as output:
                temporary = Path(output.name)
                output.write(encoded)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, self.filename)
        finally:
            if temporary is not None and temporary.exists():
                temporary.unlink()

    @staticmethod
    def _choice_elements(state):
        elements = []
        if 'loadoutID' in state:
            child = ET.Element('loadoutID')
            child.text = str(state['loadoutID'])
            elements.append(child)
        for index, values in sorted(state['loadouts'].items(), key=lambda item: int(item[0])):
            child = ET.Element('cl_' + index, {TYPE: 'loadout'})
            for field in SLOT_FIELDS:
                ET.SubElement(child, field).text = str(values[field])
            elements.append(child)
        for field, value in state['personalization'].items():
            child = ET.Element(field)
            child.text = str(value).lower() if type(value) is bool else str(value)
            elements.append(child)
        return elements

    def snapshot_update(self):
        """Restore this PC's saved choices to a fresh host before the game reads it."""
        with _storage_lock(self.directory):
            choices = self._choice_elements(self._read())
        if not choices:
            return None
        root = ET.Element('profileOwnerList')
        owner = ET.SubElement(root, 'profileOwner', {TYPE: 'profileOwnerData'})
        ET.SubElement(owner, 'name').text = self.owner
        ET.SubElement(owner, 'UUID').text = str(uuid4())
        data = ET.SubElement(owner, 'ProfileData', {'type': 'profile'})
        data.extend(choices)
        ET.SubElement(root, '{context}OnlineID').text = self.nickname
        return ET.tostring(root, encoding='utf-8')

    def overlay_lookup(self, response_bytes):
        if not self.filename.exists():
            return response_bytes
        root = _xml(response_bytes, 'jaxbCollection')
        owners = [item for item in root if item.tag == 'Item' and item.findtext('name') == self.owner]
        if not owners:
            return response_bytes
        if len(owners) != 1 or owners[0].get(TYPE) != 'profileOwnerData':
            raise ValueError('Ambiguous profile lookup response')
        data = owners[0].find('ProfileData')
        if data is None or data.attrib != {'type': 'profile'}:
            return response_bytes
        with _storage_lock(self.directory):
            state = self._read()
        replacements = self._choice_elements(state)
        for key, value in state.get('boosts', {}).items():
            child = ET.Element('{' + BOOST_NAMESPACE + '}' + key)
            child.text = str(value)
            replacements.append(child)
        for child in replacements:
            for old in data.findall(child.tag):
                data.remove(old)
            data.append(child)
        return ET.tostring(root, encoding='utf-8')

    def apply_update(self, request_bytes, response_bytes, *, boost_generation=None):
        if boost_generation is not None and (type(boost_generation) is not int or boost_generation < 0):
            raise ValueError('Invalid captured Boost generation')
        validate_http_request('POST', '/profile/update', {}, request_bytes, self.nickname)
        request = _xml(request_bytes, 'profileOwnerList')
        owners = [owner for owner in request.findall('profileOwner') if owner.findtext('name') == self.owner]
        if not owners:
            return response_bytes
        owner = owners[0]
        if (sorted(child.tag for child in owner) != ['ProfileData', 'UUID', 'name']
                or owner.attrib not in ({}, {TYPE: 'profileOwnerData'})):
            return response_bytes
        try:
            identifier = _update_uuid(owner.findtext('UUID'))
            delta, server_owned = _delta(owner.find('ProfileData'))
        except (TypeError, ValueError, AttributeError):
            return response_bytes
        root = _xml(response_bytes, 'jaxbCollection')
        results = [item for item in root if item.tag == 'Item' and item.findtext('profileOwner') == self.owner]
        if (len(results) != 1 or results[0].get(TYPE) != 'profileUpdateResult'
                or results[0].findtext('UUID') != identifier
                or results[0].findtext('statusCode') not in ('SUCCESS', 'ALREADY_APPLIED', 'INVALID_UPDATE')):
            return response_bytes
        digest = sha256(json.dumps(delta, sort_keys=True, ensure_ascii=False).encode('utf-8')).hexdigest()
        unacknowledged = server_owned and results[0].findtext('statusCode') == 'INVALID_UPDATE'
        with _storage_lock(self.directory):
            state = self._read()
            previous = state['pending'].get(identifier) or state['applied'].get(identifier)
            if previous is not None and previous != digest:
                raise ValueError('Loadout update UUID was reused with different contents')
            history = state['pending'] if unacknowledged else state['applied']
            other = state['applied'] if unacknowledged else state['pending']
            if unacknowledged and identifier not in history and len(history) >= MAX_PENDING:
                raise OSError('Unacknowledged mixed profile updates reached the local limit; saved profile was not changed')
            if previous is None:
                # Reset supersedes old-launch spends, including first deliveries
                # arriving late. Still save other choices and the ordinary UUID
                # digest, so retrying that event cannot spend the new allocation.
                if ('boost_deltas' in delta and (boost_generation is None
                        or boost_generation == state.get('boost_generation', 0))):
                    state['boosts'] = apply_deltas(state.get('boosts'), delta['boost_deltas'])
                if 'loadoutID' in delta:
                    state['loadoutID'] = delta['loadoutID']
                state['loadouts'].update(delta['loadouts'])
                state['personalization'].update(delta['personalization'])
            if previous is None or identifier in other:
                other.pop(identifier, None)
                history[identifier] = digest
                while len(state['applied']) > MAX_REPLAYS:
                    del state['applied'][next(iter(state['applied']))]
                self._write(state)
        if server_owned:
            # The game has one status per owner, not per field. Preserve the
            # backend status verbatim for the unhandled server-owned subset.
            return response_bytes
        results[0].find('statusCode').text = 'ALREADY_APPLIED' if previous else 'SUCCESS'
        return ET.tostring(root, encoding='utf-8')
