"""Counter defaults and their extracted installed-game provenance."""
import importlib.util
import json
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

from private_progression import (apply_private_progression, CHALLENGE_FLOORS,
                                 DAILY_FLOORS, MAXIMUM_CHALLENGE_COUNT)

FIXTURES = Path(__file__).with_name('fixtures')
RESEARCH = Path(__file__).resolve().parents[2] / 'Research/profile-session-fix-20261029'


class PrivateProgressionTest(unittest.TestCase):
    def test_new_profile_has_maximum_rank_and_native_challenges_have_every_floor(self):
        main = ET.Element('ProfileData', {'type': 'profile'})
        challenges = ET.Element('ProfileData', {'type': 'challengesProfile'})
        apply_private_progression(main)
        apply_private_progression(challenges)
        self.assertEqual(int(main.findtext('{c}Total')), MAXIMUM_CHALLENGE_COUNT)
        self.assertEqual(len(challenges), 763)
        for key, floor in CHALLENGE_FLOORS.items():
            self.assertEqual(int(challenges.findtext('{ch}' + key)), floor)
        self.assertEqual(challenges.findtext('{ch}MP05SAR'), '25')
        self.assertEqual(challenges.findtext('{ch}CSpXP'), '500000')
        self.assertEqual(challenges.findtext('{ch}TrACC'), '1000')
        self.assertEqual(challenges.findtext('{ch}GP_05'), '150')
        self.assertEqual(challenges.findtext('{ch}BuyGame'), '0')

    def test_captured_partial_challenges_raised_without_replacing_accolades(self):
        request = ET.parse(FIXTURES / 'native-edited-loadout-update.xml').getroot()
        data = next(owner.find('ProfileData') for owner in request.findall('profileOwner')
                    if owner.findtext('name').startswith('challenges/'))
        accolades = [ET.tostring(child) for child in data if child.tag.startswith('{a}')]
        apply_private_progression(data)
        self.assertEqual(data.findtext('{ch}MP05SAR'), '25')
        self.assertEqual(data.findtext('{ch}BuyGame'), '243')
        self.assertEqual([ET.tostring(child) for child in data if child.tag.startswith('{a}')], accolades)

    def test_higher_unknown_and_nested_unrelated_fields_preserved_and_idempotent(self):
        data = ET.fromstring('<ProfileData type="challengesProfile" xmlns:ch="ch">'
                             '<ch:MP05SAR>00077</ch:MP05SAR><ch:Future>42</ch:Future>'
                             '<unrelated flag="x"><nested /></unrelated></ProfileData>')
        apply_private_progression(data)
        self.assertEqual(data.findtext('{ch}MP05SAR'), '00077')
        self.assertEqual(data.findtext('{ch}Future'), '42')
        self.assertEqual(data.find('unrelated').get('flag'), 'x')
        before = ET.tostring(data)
        apply_private_progression(data)
        self.assertEqual(ET.tostring(data), before)

    def test_existing_higher_rank_preserved(self):
        data = ET.fromstring('<ProfileData type="profile" xmlns:c="c">'
                             '<c:Total>3000</c:Total><valor>123</valor></ProfileData>')
        before = ET.tostring(data)
        apply_private_progression(data)
        self.assertEqual(ET.tostring(data), before)

    def test_invalid_target_is_rejected_before_any_mutation(self):
        for invalid in ('', '-1', '1.0', '2147483648', ' 1', '9' * 100):
            with self.subTest(invalid=invalid):
                data = ET.Element('ProfileData', {'type': 'challengesProfile'})
                ET.SubElement(data, '{ch}MP05SAR').text = '1'
                ET.SubElement(data, '{ch}TrACC').text = invalid
                before = ET.tostring(data)
                with self.assertRaises(ValueError):
                    apply_private_progression(data)
                self.assertEqual(ET.tostring(data), before)

    def test_duplicate_or_structured_target_rejected_atomically(self):
        for body in ('<ch:TrACC>1</ch:TrACC><ch:TrACC>2</ch:TrACC>',
                     '<ch:TrACC flag="x">1</ch:TrACC>',
                     '<ch:TrACC><nested /></ch:TrACC>'):
            with self.subTest(body=body):
                data = ET.fromstring('<ProfileData type="challengesProfile" xmlns:ch="ch">'
                                     + body + '</ProfileData>')
                before = ET.tostring(data)
                with self.assertRaises(ValueError):
                    apply_private_progression(data)
                self.assertEqual(ET.tostring(data), before)

    def test_unrelated_profile_types_are_untouched(self):
        for kind in ('statsProfile', None):
            data = ET.Element('ProfileData', {'type': kind} if kind else {})
            ET.SubElement(data, 'serializedDailyChallengeMap').text = 'DC_15=0|0'
            before = ET.tostring(data)
            apply_private_progression(data)
            self.assertEqual(ET.tostring(data), before)

    def test_native_daily_map_completed_preserving_rotation_and_claim_flags(self):
        request = ET.parse(FIXTURES / 'initial-profile-update.xml').getroot()
        data = next(owner.find('ProfileData') for owner in request.findall('profileOwner')
                    if owner.findtext('name').startswith('daily_challenges/'))
        data.find('serializedDailyChallengeMap').text = 'DC_08=1000|1,DC_13=0|0,Future=50|1,'
        timestamp = data.findtext('generationTimeStamp')
        apply_private_progression(data)
        self.assertEqual(data.findtext('generationTimeStamp'), timestamp)
        self.assertEqual(data.findtext('discardCount'), '0')
        entries = dict(part.split('=') for part in data.findtext('serializedDailyChallengeMap').split(',') if part)
        self.assertEqual(entries['DC_08'], '1000|1')
        self.assertEqual(entries['Future'], '50|1')
        self.assertEqual(len(DAILY_FLOORS), 21)
        for key, floor in DAILY_FLOORS.items():
            self.assertGreaterEqual(int(entries[key].split('|')[0]), floor)
        self.assertEqual(entries['DC_13'].split('|')[1], '0')
        before = ET.tostring(data)
        apply_private_progression(data)
        self.assertEqual(ET.tostring(data), before)

    def test_missing_daily_map_has_all_verified_thresholds_with_unclaimed_rewards(self):
        data = ET.Element('ProfileData', {'type': 'DailyChallengesProfile'})
        apply_private_progression(data)
        entries = data.findtext('serializedDailyChallengeMap').removesuffix(',').split(',')
        self.assertEqual(len(entries), 21)
        self.assertTrue(all(entry.endswith('|0') for entry in entries))
        self.assertIsNone(data.find('generationTimeStamp'))

    def test_malformed_daily_map_rejected_atomically(self):
        for text in ('DC_08=1|0,DC_08=2|1,', 'DC_08=1|2,', 'DC_08=-1|0,',
                     'DC_08=2147483648|0,', 'DC_08=1|0,,', 'DC_08=oops|0,'):
            with self.subTest(text=text):
                data = ET.Element('ProfileData', {'type': 'DailyChallengesProfile'})
                ET.SubElement(data, 'serializedDailyChallengeMap').text = text
                before = ET.tostring(data)
                with self.assertRaises(ValueError):
                    apply_private_progression(data)
                self.assertEqual(ET.tostring(data), before)
        data = ET.fromstring('<ProfileData type="DailyChallengesProfile">'
                             '<serializedDailyChallengeMap/><serializedDailyChallengeMap/></ProfileData>')
        before = ET.tostring(data)
        with self.assertRaises(ValueError):
            apply_private_progression(data)
        self.assertEqual(ET.tostring(data), before)

    @unittest.skipUnless((RESEARCH / 'application-patched.rttibin').exists(),
                         'Installed-resource research artifact is outside this server package')
    def test_fixture_reproduces_all_2909_installed_leaf_resource_thresholds(self):
        spec = importlib.util.spec_from_file_location('extract_challenges',
                                                     RESEARCH / 'progression/extract_challenges.py')
        extractor = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(extractor)
        resource = (RESEARCH / 'application-patched.rttibin').read_bytes()
        records = extractor.extract(resource)
        committed = json.loads((FIXTURES / 'private-challenges.json').read_text(encoding='utf-8'))
        self.assertEqual(extractor.fixture(resource, records), committed)
        self.assertEqual(sum(extractor.leaf_type_counts(resource).values()), len(records))
        self.assertEqual(len(records), 2909)
        self.assertEqual(len({row['key_offset'] for row in records}), 2909)
        for row in records:
            self.assertGreaterEqual(CHALLENGE_FLOORS[row['key']], row['threshold'])


if __name__ == '__main__':
    unittest.main()
