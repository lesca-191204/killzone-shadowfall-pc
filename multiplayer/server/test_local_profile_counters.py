from copy import deepcopy
import json
from pathlib import Path
import tempfile
import unittest
from uuid import uuid4
import xml.etree.ElementTree as ET

import local_profile
from local_profile import LocalProfile, TYPE

FIXTURES = Path(local_profile.__file__).with_name('fixtures')


def update():
    request = ET.parse(FIXTURES / 'native-loadout-update.xml').getroot()
    owner = request.find('profileOwner')
    owner.find('name').text = 'profile/Marakesch'
    owner.find('UUID').text = str(uuid4())
    request.find('{context}OnlineID').text = 'Marakesch'
    owner.find('ProfileData/cl_9/name').text = 'Betty'
    # Both counter names and values occur in the native initial profile.
    initial = ET.parse(FIXTURES / 'initial-profile-update.xml').getroot()
    for name in ('{c}Scout', '{c}TotalCoop'):
        owner.find('ProfileData').append(deepcopy(initial.find('profileOwner/ProfileData/' + name)))
    return request


def response(request):
    result = ET.Element('jaxbCollection')
    item = ET.SubElement(result, 'Item', {TYPE: 'profileUpdateResult'})
    ET.SubElement(item, 'profileOwner').text = 'profile/Marakesch'
    ET.SubElement(item, 'UUID').text = request.findtext('profileOwner/UUID')
    ET.SubElement(item, 'statusCode').text = 'INVALID_UPDATE'
    return ET.tostring(result)


class ChallengeCounterLoadoutTest(unittest.TestCase):
    def test_native_counters_preserve_local_rename_without_acknowledging_server_fields(self):
        request = update()
        with tempfile.TemporaryDirectory() as directory:
            store = LocalProfile(directory, 'Marakesch')
            reply = response(request)
            body = ET.tostring(request)
            self.assertEqual(store.apply_update(body, reply), reply)
            self.assertTrue(store.filename.exists(), 'Native challenge counters must not discard local loadouts')
            state = json.loads(store.filename.read_text())
            self.assertEqual(state['loadouts']['9']['name'], 'Betty')
            self.assertIn(request.findtext('profileOwner/UUID'), state['pending'])
            self.assertNotIn('{c}Scout', state)
            before = store.filename.read_bytes()
            self.assertEqual(store.apply_update(body, reply), reply)
            self.assertEqual(store.filename.read_bytes(), before)

    def test_malformed_counters_and_unknown_fields_preserve_saved_loadouts(self):
        mutations = [
            lambda c: setattr(c, 'text', '-1'),
            lambda c: setattr(c, 'text', '2147483648'),
            lambda c: setattr(c, 'text', '1.5'),
            lambda c: setattr(c, 'text', None),
            lambda c: c.set('operation', 'reset'),
            lambda c: ET.SubElement(c, 'nested'),
            lambda c: setattr(c, 'tag', '{unknown}Scout'),
        ]
        with tempfile.TemporaryDirectory() as directory:
            store = LocalProfile(directory, 'Marakesch')
            initial = update()
            store.apply_update(ET.tostring(initial), response(initial))
            self.assertTrue(store.filename.exists())
            before = store.filename.read_bytes()
            for index, mutate in enumerate(mutations):
                with self.subTest(mutation=index):
                    request = update()
                    request.find('profileOwner/ProfileData/cl_9/name').text = 'Do not save'
                    mutate(request.find('profileOwner/ProfileData/{c}Scout'))
                    reply = response(request)
                    self.assertEqual(store.apply_update(ET.tostring(request), reply), reply)
                    self.assertEqual(store.filename.read_bytes(), before)

    def test_counter_fields_do_not_bypass_local_validation_or_acknowledge_counter_only_updates(self):
        for case in ('partial_slot', 'unknown_field', 'counter_only'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory:
                request = update()
                data = request.find('profileOwner/ProfileData')
                if case == 'partial_slot':
                    data.find('cl_9').remove(data.find('cl_9/primaryAbility'))
                elif case == 'unknown_field':
                    ET.SubElement(data, 'unknown').text = '0'
                else:
                    for child in list(data):
                        if not child.tag.startswith('{c}'):
                            data.remove(child)
                store = LocalProfile(directory, 'Marakesch')
                reply = response(request)
                self.assertEqual(store.apply_update(ET.tostring(request), reply), reply)
                self.assertFalse(store.filename.exists())

    def test_replayed_counter_update_cannot_overwrite_newer_rename(self):
        first = update()
        with tempfile.TemporaryDirectory() as directory:
            store = LocalProfile(directory, 'Marakesch')
            first_body, first_reply = ET.tostring(first), response(first)
            store.apply_update(first_body, first_reply)
            newer = update()
            newer.find('profileOwner/ProfileData/cl_9/name').text = 'Newer kit'
            store.apply_update(ET.tostring(newer), response(newer))
            self.assertTrue(store.filename.exists())
            before = store.filename.read_bytes()
            self.assertEqual(store.apply_update(first_body, first_reply), first_reply)
            self.assertEqual(store.filename.read_bytes(), before)
            query = ET.Element('jaxbCollection')
            owner = ET.SubElement(query, 'Item', {TYPE: 'profileOwnerData'})
            ET.SubElement(owner, 'name').text = 'profile/Marakesch'
            owner.append(ET.Element('ProfileData', {'type': 'profile'}))
            restored = LocalProfile(directory, 'Marakesch').overlay_lookup(ET.tostring(query))
            self.assertEqual(ET.fromstring(restored).findtext('Item/ProfileData/cl_9/name'), 'Newer kit')


if __name__ == '__main__':
    unittest.main()
