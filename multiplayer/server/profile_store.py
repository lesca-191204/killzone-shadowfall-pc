"""Persistent private defaults plus validated per-player choice snapshots.

Unrecognized delta operations remain rejected. Validated loadout choices use
the same merge and replay protection as the client mirror.
The guest requires a ProfileData XML element even for absent data: its custom
field reader dereferences that element at 0x476f88. An empty element has no
`type` attribute, so 0x476fa8 skips it, leaving the profile pointer null.
"""
from copy import deepcopy
from datetime import datetime
from hashlib import sha256
import json
from pathlib import Path
import re
from threading import Lock
from uuid import UUID, uuid4
import xml.etree.ElementTree as ET
from xml_storage import XSI, atomic_write
from local_profile import LocalProfile, _delta
from boost_inventory import BOOST_NAMESPACE
from private_progression import apply_private_progression

XS = 'http://www.w3.org/2001/XMLSchema'
FIXTURES = Path(__file__).with_name('fixtures')
PROGRESSION = json.loads((FIXTURES / 'private-progression.json').read_text(encoding='utf-8'))
MAXIMUM_CHALLENGE_COUNT = PROGRESSION['maximum_challenge_count']
INITIAL_PROFILE = ET.parse(FIXTURES / 'initial-profile-update.xml').find('./profileOwner/ProfileData')
INITIAL_DAILY = next(owner.find('ProfileData') for owner in
                     ET.parse(FIXTURES / 'initial-profile-update.xml').findall('profileOwner')
                     if owner.findtext('name').startswith('daily_challenges/'))


def _current_local_tick():
    # Native daily generation uses local RTC microseconds since 0001-01-01.
    elapsed = datetime.now() - datetime(1, 1, 1)
    return (elapsed.days * 86400 + elapsed.seconds) * 1000000 + elapsed.microseconds


def _refresh_daily_generation(data):
    fields = data.findall('generationTimeStamp')
    if len(fields) > 1:
        raise ValueError('Duplicate daily generation time')
    current = _current_local_tick()
    if fields:
        field = fields[0]
        if field.attrib or len(field) or not re.fullmatch(r'[0-9]{1,19}', field.text or ''):
            raise ValueError('Invalid daily generation time')
        value = int(field.text)
        if value > 0x7fffffffffffffff:
            raise ValueError('Daily generation time out of range')
        if value // 86400000000 >= current // 86400000000:
            return
    else:
        field = ET.SubElement(data, 'generationTimeStamp')
    field.text = str(current)


class ProfileStore:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.lock = Lock()

    def _filename(self, name):
        return self.directory / (sha256(name.encode('utf-8')).hexdigest() + '.xml')

    def _choices(self, name):
        # Use the same validated merge and replay rules as the client mirror.
        return LocalProfile(self.directory / 'choices' / sha256(name.encode('utf-8')).hexdigest(),
                            name.removeprefix('profile/'))

    def _read(self, name):
        filename = self._filename(name)
        if not filename.exists():
            owner = ET.Element('profileOwnerData')
            ET.SubElement(owner, 'name').text = name
            return owner
        try:
            owner = ET.parse(filename).getroot()
        except ET.ParseError as error:
            raise OSError('Stored profile XML is invalid') from error
        if owner.tag != 'profileOwnerData' or owner.findtext('name') != name:
            raise OSError('Stored profile identity is invalid')
        return owner

    def _write(self, owner):
        atomic_write(self._filename(owner.findtext('name')), owner)

    def update(self, request):
        if request.tag != 'profileOwnerList':
            raise ValueError('Expected profileOwnerList')
        owners = request.findall('profileOwner')
        if not 1 <= len(owners) <= 64:
            raise ValueError('Expected bounded profile update list')
        if any(child.tag != 'profileOwner' and not child.tag.startswith('{context}') for child in request):
            raise ValueError('Unsupported profile-list field')
        parsed = []
        names = set()
        for item in owners:
            if sorted(child.tag for child in item) != ['ProfileData', 'UUID', 'name']:
                raise ValueError('Unsupported profile-owner fields')
            name = item.findtext('name')
            identifier = item.findtext('UUID')
            data = item.find('ProfileData')
            if not name or len(name) > 256 or name in names or identifier is None or data is None:
                raise ValueError('Incomplete or duplicate profile update')
            UUID(identifier)
            names.add(name)
            parsed.append((name, identifier, data))
        result = ET.Element('jaxbCollection')
        with self.lock:
            for name, identifier, data in parsed:
                data = deepcopy(data)
                if name.startswith(('profile/', 'challenges/', 'daily_challenges/')):
                    apply_private_progression(data)
                owner = self._read(name)
                stored = owner.find('ProfileData')
                status = 'INVALID_UPDATE'
                if stored is not None and owner.findtext('UUID') == identifier:
                    if ET.tostring(stored) == ET.tostring(data):
                        status = 'ALREADY_APPLIED'
                elif stored is None and data.attrib == {'type': data.get('type')}:
                    # The live initialization contains only scalar values and
                    # flat c: map entries. No unverified delta operations.
                    supported = data.get('type') in ('profile', 'challengesProfile', 'DailyChallengesProfile')
                    if supported and all(not len(child) and not child.attrib for child in data):
                        owner = ET.Element('profileOwnerData')
                        ET.SubElement(owner, 'name').text = name
                        ET.SubElement(owner, 'UUID').text = identifier
                        owner.append(deepcopy(data))
                        self._write(owner)
                        status = 'SUCCESS'
                item = ET.SubElement(result, 'Item', {f'{{{XSI}}}type': 'profileUpdateResult'})
                ET.SubElement(item, 'profileOwner').text = name
                ET.SubElement(item, 'UUID').text = identifier
                ET.SubElement(item, 'statusCode').text = status
        response = ET.tostring(result, encoding='utf-8')
        choices_request = deepcopy(request)
        validated_choices = set()
        for owner in choices_request.findall('profileOwner'):
            if (owner.findtext('name') or '').startswith('profile/'):
                profile = owner.find('ProfileData')
                try:
                    _delta(profile)
                except (TypeError, ValueError, AttributeError):
                    continue
                validated_choices.add(owner.findtext('name'))
                for child in list(profile):
                    if child.tag.startswith('{' + BOOST_NAMESPACE + '}'):
                        profile.remove(child)
        for name, _, _ in parsed:
            if name in validated_choices:
                response = self._choices(name).apply_update(ET.tostring(choices_request), response)
        return response

    def lookup(self, request):
        if request.tag != 'jaxbCollection' or not 1 <= len(request) <= 64:
            raise ValueError('Expected a bounded owner-name collection')
        names = []
        for item in request:
            if (item.tag != 'Item' or item.attrib.get(f'{{{XSI}}}type') != 'xs:string'
                    or len(item) or not item.text or len(item.text) > 256):
                raise ValueError('Expected an owner-name string')
            names.append(item.text)
        result = ET.Element('jaxbCollection', {'xmlns:xs': XS})
        with self.lock:
            for name in names:
                filename = self._filename(name)
                owner = self._read(name)
                before = ET.tostring(owner)
                if name.startswith('profile/'):
                    data = owner.find('ProfileData')
                    if data is None:
                        # This exact scalar template was captured from the game.
                        # Keep its fields, including lct; only identity and the
                        # proven multiplayer rank floor are new defaults.
                        data = deepcopy(INITIAL_PROFILE)
                        data.find('lct').text = str(_current_local_tick())
                        if owner.find('UUID') is None:
                            ET.SubElement(owner, 'UUID').text = str(uuid4())
                        owner.append(data)
                    apply_private_progression(data)
                elif name.startswith('daily_challenges/'):
                    data = owner.find('ProfileData')
                    if data is None:
                        data = deepcopy(INITIAL_DAILY)
                        owner.append(data)
                        if owner.find('UUID') is None:
                            ET.SubElement(owner, 'UUID').text = str(uuid4())
                    apply_private_progression(data)
                    _refresh_daily_generation(data)
                elif name.startswith('challenges/'):
                    data = owner.find('ProfileData')
                    if data is None:
                        data = ET.SubElement(owner, 'ProfileData', {'type': 'challengesProfile'})
                        if owner.find('UUID') is None:
                            ET.SubElement(owner, 'UUID').text = str(uuid4())
                    apply_private_progression(data)
                if not filename.exists() or ET.tostring(owner) != before:
                    self._write(owner)
                item = deepcopy(owner)
                item.tag = 'Item'
                item.set(f'{{{XSI}}}type', 'profileOwnerData')
                if item.find('ProfileData') is None:
                    ET.SubElement(item, 'ProfileData')
                result.append(item)
        response = ET.tostring(result, encoding='utf-8')
        for name in names:
            if name.startswith('profile/'):
                response = self._choices(name).overlay_lookup(response)
        return response
