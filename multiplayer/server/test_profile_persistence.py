"""Loadouts must live beyond a room, independently of the client-side mirror."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from uuid import uuid4
import xml.etree.ElementTree as ET

from profile_store import ProfileStore
from private_progression import CHALLENGE_FLOORS, DAILY_FLOORS

FIXTURES = Path(__file__).with_name('fixtures')
TYPE = '{http://www.w3.org/2001/XMLSchema-instance}type'


def lookup(name='Player1'):
    request = ET.Element('jaxbCollection')
    ET.SubElement(request, 'Item', {TYPE: 'xs:string'}).text = 'profile/' + name
    return request


class PersistentLoadouts(unittest.TestCase):
    def test_old_daily_generation_is_refreshed_without_resetting_claimed_rewards(self):
        query = ET.Element('jaxbCollection')
        ET.SubElement(query, 'Item', {TYPE: 'xs:string'}).text = 'daily_challenges/Player1'
        with tempfile.TemporaryDirectory() as directory:
            store = ProfileStore(directory)
            owner = ET.fromstring('<profileOwnerData><name>daily_challenges/Player1</name>'
                                 '<UUID>11111111-1111-4111-8111-111111111111</UUID>'
                                 '<ProfileData type="DailyChallengesProfile"><generationTimeStamp>1</generationTimeStamp>'
                                 '<discardCount>2</discardCount><serializedDailyChallengeMap>DC_08=1000|1,</serializedDailyChallengeMap>'
                                 '</ProfileData></profileOwnerData>')
            store._write(owner)
            with patch('profile_store._current_local_tick', return_value=86400000010):
                first = store.lookup(query)
                self.assertEqual(store.lookup(query), first)
            data = ET.fromstring(first)[0].find('ProfileData')
            self.assertEqual(data.findtext('generationTimeStamp'), '86400000010')
            self.assertEqual(data.findtext('discardCount'), '2')
            self.assertIn('DC_08=1000|1,', data.findtext('serializedDailyChallengeMap'))

    def test_invalid_boost_in_mixed_update_cannot_save_a_loadout_on_host_only(self):
        with tempfile.TemporaryDirectory() as directory:
            store = ProfileStore(directory)
            before = store.lookup(lookup())
            request = ET.parse(FIXTURES / 'native-loadout-update.xml').getroot()
            request.find('profileOwner/name').text = 'profile/Player1'
            request.find('profileOwner/UUID').text = str(uuid4())
            request.find('{context}OnlineID').text = 'Player1'
            request.find('profileOwner/ProfileData/cl_10/name').text = 'Do not save'
            for _ in range(2):
                ET.SubElement(request.find('profileOwner/ProfileData'), '{m}GV1').text = '-1'
            reply = ET.fromstring(store.update(request))
            self.assertEqual(reply[0].findtext('statusCode'), 'INVALID_UPDATE')
            self.assertEqual(store.lookup(lookup()), before)

    def test_new_player_has_real_challenge_counters_not_only_rank_total(self):
        with tempfile.TemporaryDirectory() as directory:
            store = ProfileStore(directory)
            query = lookup('NewPlayer')
            ET.SubElement(query, 'Item', {TYPE: 'xs:string'}).text = 'challenges/NewPlayer'
            ET.SubElement(query, 'Item', {TYPE: 'xs:string'}).text = 'daily_challenges/NewPlayer'
            response = ET.fromstring(store.lookup(query))
            profile, challenges, daily = (owner.find('ProfileData') for owner in response)
            self.assertEqual(profile.findtext('{c}Total'), '2600')
            self.assertEqual(challenges.get('type'), 'challengesProfile')
            for key, value in CHALLENGE_FLOORS.items():
                self.assertGreaterEqual(int(challenges.findtext('{ch}' + key)), value)
            self.assertEqual(daily.get('type'), 'DailyChallengesProfile')
            entries = dict(part.split('=') for part in daily.findtext('serializedDailyChallengeMap').split(',') if part)
            for key, value in DAILY_FLOORS.items():
                self.assertGreaterEqual(int(entries[key].split('|')[0]), value)

    def test_backend_restores_choices_after_store_restart_and_ignores_old_replay(self):
        with tempfile.TemporaryDirectory() as directory:
            store = ProfileStore(directory)
            store.lookup(lookup())
            first = ET.parse(FIXTURES / 'native-loadout-update.xml').getroot()
            first.find('profileOwner/name').text = 'profile/Player1'
            first.find('profileOwner/UUID').text = str(uuid4())
            first.find('{context}OnlineID').text = 'Player1'
            first.find('profileOwner/ProfileData/cl_10/name').text = 'Saved Betty'
            first.find('profileOwner/ProfileData/loadoutID').text = '10'
            reply = ET.fromstring(store.update(first))
            self.assertEqual(reply[0].findtext('statusCode'), 'SUCCESS')
            newer = ET.fromstring(ET.tostring(first))
            newer.find('profileOwner/UUID').text = str(uuid4())
            newer.find('profileOwner/ProfileData/cl_10/name').text = 'Newer Betty'
            store.update(newer)
            store = ProfileStore(directory)
            store.update(first)
            data = ET.fromstring(store.lookup(lookup()))[0].find('ProfileData')
            self.assertEqual(data.findtext('cl_10/name'), 'Newer Betty')
            self.assertEqual(data.findtext('loadoutID'), '10')
            self.assertEqual(len([c for c in data if c.tag.startswith('cl_')]), 16)
            other = ET.fromstring(store.lookup(lookup('Player2')))[0].find('ProfileData')
            self.assertIsNone(other.find('cl_10'))


if __name__ == '__main__':
    unittest.main()
