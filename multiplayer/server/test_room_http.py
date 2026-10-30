import gzip
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

from room_http import MAX_BODY, validate_http_request


FIXTURES = Path(__file__).parent / 'fixtures'
XSI = '{http://www.w3.org/2001/XMLSchema-instance}type'


def profile_query(*names):
    root = ET.Element('jaxbCollection')
    for name in names:
        ET.SubElement(root, 'Item', {XSI: 'xs:string'}).text = name
    return ET.tostring(root)


class RoomHTTPTest(unittest.TestCase):
    def validate(self, path, body, headers=None, player='Player1', method='POST'):
        return validate_http_request(method, path, headers or {}, body, player)

    def test_captured_profile_update_and_playlist_are_preserved(self):
        profile = (FIXTURES / 'initial-profile-update.xml').read_bytes().replace(
            b'Player<', b'Player1<')
        playlist = (FIXTURES / 'initial-playlist-add.xml').read_bytes()
        for path, body in (('/profile/update', profile), ('/playlist/add', playlist)):
            with self.subTest(path=path):
                self.assertEqual(self.validate(path, body), ({}, body))

    def test_native_profile_names_include_playlist_stats(self):
        names = ['profile/Player1', 'stats/allplaylists/Player1', 'challenges/Player1',
                 'daily_challenges/Player1', 'playlisthistory/Player1',
                 'coop_stats/allplaylists/Player1',
                 'stats/b593598d-87a6-54f0-9a1f-afd36a415148/Player1']
        body = profile_query(*names)
        self.assertEqual(self.validate('/profile/get', body), ({}, body))
        for name in ('profile/Player2', 'profile/Player10', 'profile/Player1-extra',
                     'profile/Player1/Player2', 'Player1', '', 'arbitrary/Player1',
                     'stats/not-a-uuid/Player1', 'profile/../Player1'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.validate('/profile/get', profile_query('profile/Player1', name))
        with self.assertRaises(ValueError):
            self.validate('/profile/get', profile_query(*(['profile/Player1'] * 33)))

    def test_mixed_owner_update_context_spoof_and_ambiguous_names_rejected(self):
        original = (FIXTURES / 'initial-profile-update.xml').read_bytes().replace(
            b'Player<', b'Player1<')
        for change in ('other-owner', 'context', 'duplicate-name', 'missing-name'):
            root = ET.fromstring(original)
            owner = root.find('profileOwner')
            if change == 'other-owner':
                root.findall('profileOwner')[1].find('name').text = 'daily_challenges/Player2'
            elif change == 'context':
                root.find('{context}OnlineID').text = 'Player2'
            elif change == 'duplicate-name':
                ET.SubElement(owner, 'name').text = 'profile/Player2'
            else:
                owner.remove(owner.find('name'))
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.validate('/profile/update', ET.tostring(root))
        root = ET.fromstring(original)
        owner = ET.tostring(root.find('profileOwner'))
        for _ in range(31):
            root.append(ET.fromstring(owner))
        with self.assertRaises(ValueError):
            self.validate('/profile/update', ET.tostring(root))

    def test_playlist_creator_must_unambiguously_match_ticket(self):
        original = (FIXTURES / 'initial-playlist-add.xml').read_bytes()
        for change in ('other', 'missing', 'duplicate', 'duplicate-data'):
            root = ET.fromstring(original)
            data = root.find('playlistData')
            if change == 'other':
                data.find('creator').text = 'Player2'
            elif change == 'missing':
                data.remove(data.find('creator'))
            elif change == 'duplicate':
                ET.SubElement(data, 'creator').text = 'Player1'
            else:
                root.append(ET.fromstring(ET.tostring(data)))
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.validate('/playlist/add', ET.tostring(root))

    def test_gzip_is_bounded_decoded_once_then_identity_checked(self):
        body = profile_query('profile/Player1')
        headers, actual = self.validate('/profile/get', gzip.compress(body), {
            'content-encoding': 'GZip', 'CONTENT-LENGTH': '123',
            'Content-Type': 'application/xml', 'accept': '*/*'})
        self.assertEqual(actual, body)
        self.assertEqual(headers, {'Content-Type': 'application/xml', 'Accept': '*/*'})
        invalid = (gzip.compress(b'x' * (MAX_BODY + 1)), gzip.compress(body)[:-2],
                   gzip.compress(body) + b'trailing', gzip.compress(body) * 2,
                   gzip.compress(profile_query('profile/Player2')))
        for value in invalid:
            with self.subTest(size=len(value)), self.assertRaises(ValueError):
                self.validate('/profile/get', value, {'Content-Encoding': 'gzip'})
        with self.assertRaises(ValueError):
            self.validate('/profile/get', b'x' * (MAX_BODY + 1))
        with self.assertRaises(ValueError):
            self.validate('/profile/get', body, {'Content-Encoding': 'br'})

    def test_only_safe_representation_headers_survive(self):
        original = {'Authorization': 'Bearer wrong', 'cookie': 'kzsf_session=other',
                    'X-PsN-AuTh-CoDe': 'KZSF_LOCAL_TEST:Player2', 'Host': 'evil',
                    'Connection': 'keep-alive', 'Transfer-Encoding': 'chunked',
                    'Content-Length': '0', 'X-Other': 'arbitrary',
                    'content-type': 'application/xml; charset=UTF-8', 'ACCEPT': '*/*'}
        headers, _ = self.validate('/profile/get', profile_query('profile/Player1'), original)
        self.assertEqual(headers, {'Content-Type': 'application/xml; charset=UTF-8', 'Accept': '*/*'})
        self.assertEqual(original['Authorization'], 'Bearer wrong')
        for invalid in ({'Accept': 'xml\r\nCookie: other'},
                        {'Content-Type': 'x\0'},
                        {'Content-Encoding': 'gzip', 'content-encoding': ''}):
            with self.subTest(headers=invalid), self.assertRaises(ValueError):
                self.validate('/profile/get', profile_query('profile/Player1'), invalid)

    def test_endpoint_method_and_ticket_allowlists(self):
        for path in ('/auth/auth', '/events/activeconnection', '/clan/myclan'):
            self.assertEqual(self.validate(path, b'', method='GET'), ({}, b''))
        cases = [('GET', '/profile/get'), ('POST', '/auth/auth'), ('PUT', '/profile/get'),
                 ('POST', 'http://127.0.0.1/profile/get'), ('POST', '//profile/get'),
                 ('POST', '/profile/get?player=Player2'), ('POST', '/profile/get#x'),
                 ('POST', '/profile/%67et'), ('POST', '/playlist/get-active'),
                 ('POST', '/unknown')]
        for method, path in cases:
            with self.subTest(method=method, path=path), self.assertRaises(ValueError):
                self.validate(path, b'<jaxbCollection/>', method=method)
        for player in ('ab', 'x' * 17, '', 'Player1/Player2'):
            with self.subTest(player=player), self.assertRaises(ValueError):
                self.validate('/clan/myclan', b'', method='GET', player=player)
        with self.assertRaises(ValueError):
            self.validate('/auth/auth', b'body', method='GET')

    def test_raw_native_routes_are_checked_without_xml_conversion(self):
        self.assertEqual(self.validate('/network/pinglist', b'\0'), ({}, b'\0'))
        self.assertEqual(self.validate('/playlist/get-recommended-by-tags', b'warzone'),
                         ({}, b'warzone'))
        for value in (b'', b'\0\0', b'<ping/>'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.validate('/network/pinglist', value)
        for value in (b'', b'x\0y', b'\xff', b'x' * 1025):
            with self.subTest(size=len(value)), self.assertRaises(ValueError):
                self.validate('/playlist/get-recommended-by-tags', value)

    def test_xml_roots_and_context_are_checked_on_all_xml_routes(self):
        accepted = {'/system/settings': b'<settingsDelta/>',
                    '/match/message': b'<kinMatchRequest/>',
                    '/playlist/get': b'<playlistIdList/>',
                    '/playlist/get-with-version': b'<playlistDataVersionRequest/>',
                    '/playlist/find-by-ids': b'<playlistIdList/>',
                    '/playlist/find-by-tags': b'<playlistInfoByTagsRequest/>',
                    '/playlist/find-by-filter': b'<playlistInfoByFilterRequest/>'}
        for path, body in accepted.items():
            with self.subTest(path=path):
                self.assertEqual(self.validate(path, body), ({}, body))
                with self.assertRaises(ValueError):
                    self.validate(path, b'<wrong/>')
                root = ET.fromstring(body)
                ET.SubElement(root, '{context}OnlineID').text = 'Player2'
                with self.assertRaises(ValueError):
                    self.validate(path, ET.tostring(root))
        for body in (b'<broken',
                     b'<?xml version="1.0" encoding="made-up"?><settingsDelta/>',
                     b'<!DOCTYPE settingsDelta [<!ENTITY x "Player1">]><settingsDelta>&x;</settingsDelta>'):
            with self.assertRaises(ValueError):
                self.validate('/system/settings', body)

    def test_native_faction_switch_passes_the_match_allowlist(self):
        body = (b'<factionSwitchRequest><id>switch-1</id><playlist>warzone</playlist>'
                b'<gameId>1</gameId><targetFaction>HGH</targetFaction></factionSwitchRequest>')
        self.assertEqual(self.validate('/match/message', body), ({}, body))

    def test_xml_depth_is_bounded_before_backend_forwarding(self):
        valid = b'<settingsDelta>' + b'<x>' * 63 + b'</x>' * 63 + b'</settingsDelta>'
        self.assertEqual(self.validate('/system/settings', valid), ({}, valid))
        invalid = b'<settingsDelta>' + b'<x>' * 64 + b'</x>' * 64 + b'</settingsDelta>'
        with self.assertRaises(ValueError):
            self.validate('/system/settings', invalid)

    def test_xml_node_count_is_bounded_even_when_body_is_small(self):
        valid = b'<settingsDelta>' + b'<x/>' * 9999 + b'</settingsDelta>'
        self.assertEqual(self.validate('/system/settings', valid), ({}, valid))
        invalid = b'<settingsDelta>' + b'<x/>' * 10000 + b'</settingsDelta>'
        with self.assertRaises(ValueError):
            self.validate('/system/settings', invalid)


if __name__ == '__main__':
    unittest.main()
