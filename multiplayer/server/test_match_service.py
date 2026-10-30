"""Captured and RE-derived request checks, not shared-gameplay proof."""
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
import tempfile
from uuid import UUID

from match_service import MatchService
from playlist_store import PlaylistStore


XSI = '{http://www.w3.org/2001/XMLSchema-instance}type'
PLAYLIST = '3b5ad348-1914-4086-9d6c-610d23218ee6'
OTHER_PLAYLIST = 'a75ea0d5-fb11-4ab8-a7dc-1992db9bd6d8'


def request(identifier='request-1', playlist=PLAYLIST, faction='ISA'):
    root = ET.Element('kinMatchRequest')
    for name, value in (('id', identifier), ('buildId', 'captured-build'),
                        ('playlist', playlist)):
        ET.SubElement(root, name).text = value
    player = ET.SubElement(root, 'players', {XSI: 'kinPlayer'})
    ET.SubElement(player, 'faction').text = faction
    return root


class MatchServiceTest(unittest.TestCase):
    def setUp(self):
        self.playlists = {
            PLAYLIST: {'id': PLAYLIST, 'version': 0, 'round_count': 1, 'max_player_count': 24},
            OTHER_PLAYLIST: {'id': OTHER_PLAYLIST, 'version': 0, 'round_count': 1, 'max_player_count': 24},
        }
        settings = ET.parse(Path(__file__).parent / 'fixtures' / 'settings.xml').getroot()
        self.settings_version = int(settings.findtext('version'))
        self.service = MatchService(self.playlists.get, self.settings_version)

    def test_room_identities_are_explicit_and_follow_admission_membership(self):
        identities = {'HostName'}
        service = MatchService(self.playlists.get, self.settings_version, identities=identities)
        assigned = ET.fromstring(service.assign(request(faction=''), 'HostName'))
        self.assertEqual(assigned.findtext('assignments/faction'), 'ISA')
        with self.assertRaises(ValueError):
            service.assign(request(), 'Player1')
        identities.add('GuestName')
        joined = ET.fromstring(service.assign(request(faction=''), 'GuestName'))
        self.assertEqual(joined.findtext('assignments/faction'), 'HGH')
        identities.remove('GuestName')
        with self.assertRaises(ValueError):
            service.cancel(ET.fromstring('<matchCancelRequestKIN><id>x</id>'
                                        '<targetId>y</targetId><playlist>z</playlist>'
                                        '</matchCancelRequestKIN>'), 'GuestName')

    def test_departed_identity_loses_report_and_faction_allocation(self):
        incoming = self.game_update()
        self.service.update(incoming, 'Player1')
        self.service.unregister_player('Player1')
        self.service.unregister_player('Player1')
        self.assertNotIn('Player1', self.service._assigned_players)
        self.assertNotIn('Player1', self.service._game_reports)
        with self.assertRaises(ValueError):
            self.service.update(incoming, 'Player1')
        assignment = ET.fromstring(self.service.assign(
            request(playlist=incoming.findtext('game/playlistId'), faction=''), 'Player2'))
        self.assertEqual(assignment.findtext('assignments/faction'), 'ISA')

    def faction_switch(self, identifier='switch-1', faction='HGH', game_id=1):
        root = ET.Element('factionSwitchRequest', {XSI: 'factionSwitchRequest'})
        for name, value in (('id', identifier), ('playlist', PLAYLIST),
                            ('gameId', str(game_id)), ('targetFaction', faction)):
            ET.SubElement(root, name).text = value
        return root

    def test_faction_switch_is_unsupported_without_changing_assignments(self):
        self.service.assign(request(), 'Player1')
        self.service.assign(request(faction='HGH'), 'Player2')
        for number, faction in enumerate(('HGH', 'ISA', 'HGH')):
            incoming = self.faction_switch(f'switch-{number}', faction)
            before = ET.tostring(incoming)
            with self.assertRaises(NotImplementedError):
                self.service.switch_faction(incoming, 'Player1')
            self.assertEqual(self.service._assigned_players, {'Player1': 'ISA', 'Player2': 'HGH'})
            self.assertEqual(self.service._player_games, {'Player1': 1, 'Player2': 1})
            self.assertEqual(self.service._build_ids,
                             {'Player1': 'captured-build', 'Player2': 'captured-build'})
            self.assertEqual(self.service._next_game_id, 2)
            self.assertEqual(self.service._outbox, {})
            self.assertEqual(ET.tostring(incoming), before)
        rejoin = ET.fromstring(self.service.assign(request(faction=''), 'Player1'))
        self.assertEqual(rejoin.findtext('assignments/faction'), 'ISA')

    def test_faction_switch_rejects_bad_dto_and_unassigned_identity_without_mutation(self):
        self.service.assign(request(), 'Player1')
        cases = []
        for name in ('id', 'playlist', 'gameId', 'targetFaction'):
            for change in ('missing', 'empty', 'duplicate', 'nested', 'oversize'):
                node = self.faction_switch()
                if change == 'missing':
                    node.remove(node.find(name))
                elif change == 'empty':
                    node.find(name).text = ''
                elif change == 'duplicate':
                    ET.SubElement(node, name).text = node.findtext(name)
                elif change == 'nested':
                    ET.SubElement(node.find(name), 'nested')
                else:
                    node.find(name).text = 'x' * 257
                cases.append(node)
        for name, value in (('gameId', '0'), ('gameId', '2'), ('gameId', '1.0'),
                            ('gameId', '4294967295'), ('playlist', OTHER_PLAYLIST),
                            ('targetFaction', 'unknown')):
            node = self.faction_switch()
            node.find(name).text = value
            cases.append(node)
        wrong = self.faction_switch(); wrong.tag = 'kinMatchRequest'; cases.append(wrong)
        wrong = self.faction_switch(); wrong.set(XSI, 'kinMatchRequest'); cases.append(wrong)
        for node in cases:
            with self.subTest(xml=ET.tostring(node)), self.assertRaises(ValueError):
                self.service.switch_faction(node, 'Player1')
        for identity in ('Player2', 'NotLocal', ''):
            with self.subTest(identity=identity), self.assertRaises(ValueError):
                self.service.switch_faction(self.faction_switch(), identity)
        self.assertEqual(self.service._assigned_players, {'Player1': 'ISA'})
        self.assertEqual(self.service._player_games, {'Player1': 1})
        self.assertEqual(self.service._build_ids, {'Player1': 'captured-build'})

    def test_faction_switch_rejection_preserves_teams_across_next_round(self):
        service, report = self.transition_service()
        with self.assertRaises(NotImplementedError):
            service.switch_faction(self.faction_switch(), 'Player1')
        report.find('game/nextRoundIndex').text = '1'
        service.update(report, 'Player1')
        self.assertEqual(service._assigned_players, {'Player1': 'ISA', 'Player2': 'HGH'})
        self.assertEqual(service._player_games, {'Player1': 2, 'Player2': 2})
        with self.assertRaises(ValueError):
            service.switch_faction(self.faction_switch(faction='ISA'), 'Player1')
        with self.assertRaises(NotImplementedError):
            service.switch_faction(self.faction_switch('new-round', 'HGH', game_id=2), 'Player1')
        published = {identity: ET.fromstring(payload) for identity, _, payload in self.published}
        self.assertEqual(published['Player1'].findtext('faction'), 'ISA')
        self.assertEqual(published['Player2'].findtext('faction'), 'HGH')

    def transition_service(self, round_count=2):
        self.published = []
        service = MatchService(self.playlists.get, self.settings_version,
                               publish_assignment=lambda *event: self.published.append(event),
                               is_master=lambda identity, game_id: identity == 'Player1')
        report = ET.parse(Path(__file__).parent / 'fixtures' / 'captured-match-game-update.xml').getroot()
        report.find('game/playlistId').text = PLAYLIST
        self.playlists[PLAYLIST]['round_count'] = round_count
        service.created_assignment(PLAYLIST, 'Player1')
        service.assign(request(faction=''), 'Player2')
        return service, report

    def test_distinct_warzones_have_distinct_transport_games(self):
        first = ET.fromstring(self.service.assign(request(), 'Player1')).find('assignments')
        second = ET.fromstring(self.service.assign(request(playlist=OTHER_PLAYLIST), 'Player2')).find('assignments')
        self.assertNotEqual(first.findtext('gameId'), second.findtext('gameId'))
        self.assertEqual(second.findtext('gameId'), second.findtext('turbineContext/gameId'))
        self.assertEqual(self.service.game_for('Player2'), int(second.findtext('gameId')))
        joined = ET.fromstring(self.service.assign(request(faction=''), 'Player2')).find('assignments')
        self.assertEqual(joined.findtext('gameId'), first.findtext('gameId'))
        self.service.unregister_player('Player2')
        self.assertIsNone(self.service.game_for('Player2'))

    def test_master_next_round_publishes_one_new_generation_to_both_players(self):
        service, report = self.transition_service()
        report.find('game/nextRoundIndex').text = '1'
        self.assertEqual(service.update(report, 'Player1'), b'<matchResult/>')
        self.assertEqual(len(self.published), 2)
        game_ids = set()
        for identity, key, payload in self.published:
            assignment = ET.fromstring(payload)
            self.assertEqual(assignment.tag, 'kinAssignmentMessage')
            self.assertEqual(assignment.findtext('currentRound'), '1')
            self.assertEqual(assignment.findtext('faction'), 'ISA' if identity == 'Player1' else 'HGH')
            self.assertEqual(assignment.findtext('gameId'), assignment.findtext('turbineContext/gameId'))
            game_ids.add(int(assignment.findtext('gameId')))
            self.assertIn(str(service.game_for(identity)), key)
        self.assertEqual(len(game_ids), 1)
        self.assertNotIn(1, game_ids)
        # A retry from the previous transport cannot rotate again.
        self.assertEqual(service.update(report, 'Player1'), b'<matchResult/>')
        self.assertEqual(len(self.published), 2)
        rejoin = ET.fromstring(service.assign(request(), 'Player2')).find('assignments')
        self.assertEqual(rejoin.findtext('currentRound'), '1')
        self.assertEqual(int(rejoin.findtext('gameId')), service.game_for('Player1'))
        self.assertEqual(rejoin.findtext('faction'), 'HGH')

    def test_single_map_repeat_changes_game_id_without_inventing_a_round(self):
        service, report = self.transition_service(round_count=1)
        report.find('game/nextRoundIndex').text = '0'
        service.update(report, 'Player1')
        for _, _, payload in self.published:
            assignment = ET.fromstring(payload)
            self.assertEqual(assignment.findtext('currentRound'), '0')
            self.assertNotEqual(assignment.findtext('gameId'), '1')

    def test_transition_requires_master_and_does_not_mix_other_warzone(self):
        service, report = self.transition_service()
        service.assign(request(playlist=OTHER_PLAYLIST), 'Player2')
        report.find('game/nextRoundIndex').text = '1'
        other_id = service.game_for('Player2')
        service.is_master = lambda identity, game_id: False
        with self.assertRaises(ValueError):
            service.update(report, 'Player1')
        self.assertEqual(service.game_for('Player1'), 1)
        self.assertEqual(self.published, [])
        service.is_master = lambda identity, game_id: identity == 'Player1'
        service.update(report, 'Player1')
        self.assertEqual([event[0] for event in self.published], ['Player1'])
        self.assertEqual(service.game_for('Player2'), other_id)
        report.find('game/group/players/id').text = 'Player2'
        with self.assertRaises(ValueError):
            service.update(report, 'Player2')

    def test_old_report_identity_and_transport_still_require_validation(self):
        service, report = self.transition_service()
        report.find('game/nextRoundIndex').text = '1'
        service.update(report, 'Player1')
        for path, value in (('game/id', '999'), ('game/turbineContext/gameId', '999'),
                            ('game/currentRound', '1')):
            altered = ET.fromstring(ET.tostring(report))
            altered.find(path).text = value
            with self.subTest(path=path), self.assertRaises(ValueError):
                service.update(altered, 'Player1')
        service.unregister_player('Player1')
        with self.assertRaises(ValueError):
            service.update(report, 'Player1')

    def test_failed_event_publication_is_retried_without_second_generation(self):
        service, report = self.transition_service()
        report.find('game/nextRoundIndex').text = '1'
        delivered = {}
        fail_once = [True]
        def publish(identity, key, payload):
            if identity == 'Player2' and fail_once:
                fail_once.pop()
                raise OSError('test storage failure')
            delivered[(identity, key)] = payload
        service.publish_assignment = publish
        with self.assertRaises(OSError):
            service.update(report, 'Player1')
        generation = service.game_for('Player1')
        service.update(report, 'Player1')
        self.assertEqual(service.game_for('Player1'), generation)
        self.assertEqual(len(delivered), 2)

    def test_empty_requests_allocate_opposing_factions_and_keep_rejoins_stable(self):
        # Creating the Warzone already assigns its creator, before a normal
        # kinMatchRequest arrives from the other authenticated client.
        creator = ET.fromstring(self.service.created_assignment(PLAYLIST, 'Player1'))
        self.assertEqual(creator.findtext('faction'), 'ISA')
        for _ in range(2):
            joining = ET.fromstring(self.service.assign(request(faction=''), 'Player2'))
            self.assertEqual(joining.findtext('assignments/faction'), 'HGH')
            rejoin = ET.fromstring(self.service.assign(request(faction=''), 'Player1'))
            self.assertEqual(rejoin.findtext('assignments/faction'), 'ISA')
        repeated_event = ET.fromstring(self.service.created_assignment(PLAYLIST, 'Player2'))
        self.assertEqual(repeated_event.findtext('faction'), 'HGH')

    def test_explicit_faction_is_preserved_and_counted_for_next_empty_request(self):
        first = ET.fromstring(self.service.assign(request(faction='HGH'), 'Player2'))
        self.assertEqual(first.findtext('assignments/faction'), 'HGH')
        second = ET.fromstring(self.service.assign(request(faction=''), 'Player1'))
        self.assertEqual(second.findtext('assignments/faction'), 'ISA')
        # An explicit preference on a repeated request cannot reassign a player.
        preferred = ET.fromstring(self.service.assign(request(faction='HGH'), 'Player1'))
        self.assertEqual(preferred.findtext('assignments/faction'), 'ISA')
        repeated = ET.fromstring(self.service.assign(request(faction=''), 'Player1'))
        self.assertEqual(repeated.findtext('assignments/faction'), 'ISA')

    def game_update(self):
        incoming = ET.parse(Path(__file__).parent / 'fixtures' / 'captured-match-game-update.xml').getroot()
        playlist_id = incoming.findtext('game/playlistId')
        self.playlists[playlist_id] = {'id': playlist_id, 'version': 0, 'round_count': 1, 'max_player_count': 24}
        self.service.created_assignment(playlist_id, 'Player1')
        return incoming

    def test_captured_game_update_is_acknowledged_without_changing_assignment(self):
        incoming = self.game_update()
        original = ET.tostring(incoming)
        for _ in range(2):
            self.assertEqual(self.service.update(incoming, 'Player1'), b'<matchResult/>')
        self.assertEqual(self.service._game_reports['Player1'], original)
        self.assertEqual(ET.tostring(incoming), original)
        assignment = ET.fromstring(self.service.assign(request('second', incoming.findtext('game/playlistId')), 'Player2'))
        self.assertEqual(assignment.findtext('assignments/gameId'), incoming.findtext('game/id'))
        self.assertEqual(assignment.findtext('assignments/currentRound'), '0')
        self.assertNotIn('Player2', self.service._game_reports)

    def test_game_update_rejects_unassigned_or_mismatched_report_without_mutation(self):
        original = self.game_update()
        for identity in ('Player2', 'NotLocal', ''):
            with self.subTest(identity=identity), self.assertRaises(ValueError):
                self.service.update(original, identity)
        cases = []
        for path, value in (
                ('id', ''), ('game/id', '2'), ('game/version', '5'),
                ('game/playlistId', 'missing'), ('game/currentRound', '1'),
                ('game/currentRound', '-1'), ('game/nextRoundIndex', '-2'),
                ('game/currentRunTime', 'not-a-number'),
                ('game/turbineContext/ip', '192.0.2.1'),
                ('game/turbineContext/port', '17802'),
                ('game/turbineContext/gameId', '2'),
                ('game/group/players/id', 'Player2')):
            node = ET.fromstring(ET.tostring(original));node.find(path).text = value;cases.append(node)
        duplicate = ET.fromstring(ET.tostring(original));duplicate.append(duplicate.find('game'));cases.append(duplicate)
        wrong_type = ET.fromstring(ET.tostring(original));wrong_type.find('game').set(XSI, 'otherGame');cases.append(wrong_type)
        for malformed in cases:
            with self.subTest(xml=ET.tostring(malformed)), self.assertRaises(ValueError):
                self.service.update(malformed, 'Player1')
        self.assertEqual(self.service._game_reports, {})
        self.assertEqual(self.service.update(original, 'Player1'), b'<matchResult/>')

    def test_captured_native_request_and_late_cancel_keep_shared_game(self):
        fixtures = Path(__file__).parent / 'fixtures'
        incoming = ET.parse(fixtures / 'captured-match-request.xml').getroot()
        cancel = ET.parse(fixtures / 'captured-match-cancel.xml').getroot()
        playlist_id = incoming.findtext('playlist')
        self.playlists[playlist_id] = {'id': playlist_id, 'version': 0, 'round_count': 1, 'max_player_count': 24}
        self.assertEqual(cancel.findtext('targetId'), incoming.findtext('id'))
        self.assertEqual(cancel.findtext('playlist'), playlist_id)
        before = ET.tostring(cancel)
        first = ET.fromstring(self.service.assign(incoming, 'Player1'))
        self.assertEqual(first.findtext('requestId'), incoming.findtext('id'))
        self.assertEqual(first.findtext('assignments/buildId'), incoming.findtext('buildId'))
        self.assertEqual(first.findtext('assignments/playlistId'), playlist_id)
        self.assertEqual(first.findtext('assignments/faction'), 'ISA')
        # Matching is immediate. Late/repeated cancellation acknowledges that
        # no matching work remains; it must not tear down an assigned game.
        for identity in ('Player1', 'Player1', 'Player2'):
            result = ET.fromstring(self.service.cancel(cancel, identity))
            self.assertEqual(result.tag, 'matchResult')
            self.assertEqual(len(result), 0)
            self.assertEqual(result.attrib, {})
        self.assertEqual(ET.tostring(cancel), before)
        second = ET.fromstring(self.service.assign(request('second', playlist_id), 'Player2'))
        self.assertEqual(ET.tostring(first.find('assignments/turbineContext')),
                         ET.tostring(second.find('assignments/turbineContext')))
        separate = ET.fromstring(self.service.assign(request(playlist=OTHER_PLAYLIST), 'Player1'))
        self.assertNotEqual(separate.findtext('assignments/gameId'), first.findtext('assignments/gameId'))
        self.assertEqual(self.service.game_for('Player2'), int(second.findtext('assignments/gameId')))

    def test_cancel_validates_native_dto_without_binding_the_relay(self):
        original = ET.parse(Path(__file__).parent / 'fixtures' / 'captured-match-cancel.xml').getroot()
        malformed = []
        for name in ('id', 'targetId', 'playlist'):
            for change in ('missing', 'empty', 'duplicate', 'nested', 'oversize'):
                node = ET.fromstring(ET.tostring(original))
                if change == 'missing':
                    node.remove(node.find(name))
                elif change == 'empty':
                    node.find(name).text = ''
                elif change == 'duplicate':
                    ET.SubElement(node, name).text = 'duplicate'
                elif change == 'nested':
                    ET.SubElement(node.find(name), 'value').text = 'nested'
                else:
                    node.find(name).text = 'x' * 257
                malformed.append(node)
        wrong_root = ET.fromstring(ET.tostring(original))
        wrong_root.tag = 'matchGameUpdateKIN'
        malformed.append(wrong_root)
        wrong_type = ET.fromstring(ET.tostring(original))
        wrong_type.set(XSI, 'kinMatchRequest')
        malformed.append(wrong_type)
        for node in malformed:
            with self.subTest(xml=ET.tostring(node)), self.assertRaises(ValueError):
                self.service.cancel(node, 'Player1')
        for identity in ('', 'NotLocal', 'Player3'):
            with self.subTest(identity=identity), self.assertRaises(ValueError):
                self.service.cancel(original, identity)
        # Cancel can arrive after service restart, or target an already completed
        # request. With no queued work, it is an idempotent empty acknowledgement.
        self.assertEqual(ET.fromstring(self.service.cancel(original, 'Player1')).tag, 'matchResult')
        assigned = ET.fromstring(self.service.assign(request(playlist=OTHER_PLAYLIST), 'Player2'))
        self.assertEqual(assigned.findtext('assignments/playlistId'), OTHER_PLAYLIST)

    def test_two_accounts_get_one_native_assignment_for_same_existing_game(self):
        outputs = []
        for number, faction in ((1, 'ISA'), (2, 'HGH')):
            incoming = request(f'request-{number}', faction=faction)
            snapshot = ET.tostring(incoming)
            raw = self.service.assign(incoming, f'Player{number}')
            result = ET.fromstring(raw)
            assignments = result.findall('assignments')
            self.assertEqual(len(assignments), 1,
                             'Successful matching must deliver one assignment at HTTP EOF')
            self.assertEqual(result.tag, 'matchRequestResult')
            self.assertEqual(result.findtext('requestId'), f'request-{number}')
            self.assertEqual(float(result.findtext('progress')), 1.0)
            self.assertFalse(result.findtext('failReason'))
            assignment = assignments[0]
            self.assertEqual(assignment.attrib[XSI], 'kinAssignmentMessage')
            self.assertEqual(assignment.findtext('requestId'), f'request-{number}')
            self.assertEqual(assignment.findtext('originRequestId'), f'request-{number}')
            self.assertEqual(assignment.findtext('playlistId'), PLAYLIST)
            self.assertEqual(assignment.findtext('playlistVersion'), '0')
            self.assertEqual(assignment.findtext('currentRound'), '0')
            self.assertEqual(assignment.findtext('buildId'), 'captured-build')
            self.assertEqual(assignment.findtext('faction'), faction)
            self.assertEqual(assignment.findtext('isSpectator'), 'false')
            context = assignment.find('turbineContext')
            self.assertIsNotNone(context)
            self.assertEqual(context.findtext('gameId'), '1')
            self.assertEqual(context.findtext('ip'), '127.0.0.1')
            self.assertEqual(context.findtext('port'), '17801')
            self.assertFalse(context.findtext('key'))
            self.assertEqual(raw, self.service.assign(incoming, f'Player{number}'))
            self.assertEqual(ET.tostring(incoming), snapshot)
            outputs.append(assignment)
        self.assertEqual(outputs[0].findtext('gameId'), outputs[1].findtext('gameId'))
        self.assertEqual(ET.tostring(outputs[0].find('turbineContext')),
                         ET.tostring(outputs[1].find('turbineContext')))

    def test_bad_request_cannot_assign_or_bind_game(self):
        cases = []
        missing = request()
        missing.remove(missing.find('id'))
        cases.append(missing)
        missing = request()
        missing.remove(missing.find('playlist'))
        cases.append(missing)
        duplicate = request()
        ET.SubElement(duplicate, 'playlist').text = OTHER_PLAYLIST
        cases.append(duplicate)
        wrong_type = request()
        wrong_type.tag = 'kinClanMatchRequest'
        cases.append(wrong_type)
        conflicting_type = request()
        conflicting_type.set(XSI, 'kinClanMatchRequest')
        cases.append(conflicting_type)
        cases.extend((request(identifier=''), request(identifier='x' * 257),
                      request(playlist='missing'), request(faction='unknown')))
        for malformed in cases:
            with self.subTest(xml=ET.tostring(malformed)):
                with self.assertRaises(ValueError):
                    self.service.assign(malformed, 'Player1')
        for identity in ('', 'NotLocal', 'Player3'):
            with self.subTest(identity=identity), self.assertRaises(ValueError):
                self.service.assign(request(), identity)
        # A failed request must not reserve the single relay for its playlist.
        valid = ET.fromstring(self.service.assign(request(playlist=OTHER_PLAYLIST), 'Player2'))
        self.assertEqual(valid.findtext('assignments/playlistId'), OTHER_PLAYLIST)

    def test_playlists_keep_versions_and_reject_unloadable_rounds(self):
        self.playlists[PLAYLIST]['round_count'] = 0
        with self.assertRaises(ValueError):
            self.service.assign(request(), 'Player1')
        self.playlists[PLAYLIST]['round_count'] = 1
        self.service.assign(request(), 'Player1')
        separate = ET.fromstring(self.service.assign(request(playlist=OTHER_PLAYLIST), 'Player2'))
        self.assertNotEqual(int(separate.findtext('assignments/gameId')), self.service.game_for('Player1'))
        self.playlists[PLAYLIST]['version'] = 1
        with self.assertRaises(ValueError):
            self.service.assign(request(), 'Player2')

    def test_missing_faction_is_allocated_and_optional_build_remains_empty(self):
        incoming = request()
        incoming.remove(incoming.find('buildId'))
        incoming.remove(incoming.find('players'))
        output = ET.fromstring(self.service.assign(incoming, 'Player'))
        self.assertEqual(len(output.findall('assignments')), 1)
        self.assertEqual(output.findtext('assignments/faction'), 'ISA')
        self.assertFalse(output.findtext('assignments/buildId'))

    def test_assignment_uses_global_settings_version_for_native_start_dependency(self):
        settings = ET.parse(Path(__file__).parent / 'fixtures' / 'settings.xml').getroot()
        # Native 0x45f794 waits for exact settings-version equality before Turbine starts.
        for identity in ('Player1', 'Player2'):
            output = ET.fromstring(self.service.assign(request(identity), identity))
            self.assertEqual(output.findtext('assignments/version'), settings.findtext('version'))
        event = ET.fromstring(self.service.created_assignment(PLAYLIST, 'Player1'))
        self.assertEqual(event.findtext('version'), settings.findtext('version'))
        # A changed settings fixture must not require a second protocol constant.
        newer = MatchService(self.playlists.get, self.settings_version + 1)
        output = ET.fromstring(newer.assign(request(), 'Player1'))
        self.assertEqual(output.findtext('assignments/version'), str(self.settings_version + 1))
        event = ET.fromstring(newer.created_assignment(PLAYLIST, 'Player2'))
        self.assertEqual(event.findtext('version'), str(self.settings_version + 1))
        for invalid in (None, True, -1, 1 << 63, '4'):
            with self.subTest(version=invalid), self.assertRaises(ValueError):
                MatchService(self.playlists.get, invalid)

    def test_two_accounts_use_actual_saved_native_warzone(self):
        fixture = ET.parse(Path(__file__).parent / 'fixtures' / 'initial-playlist-add.xml').getroot()
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory:
            store = PlaylistStore(directory)
            added = ET.fromstring(store.add(fixture))
            identifier = added.findtext('playlistId')
            service = MatchService(store.lookup, self.settings_version)
            snapshots = {p.name: p.read_bytes() for p in Path(directory).glob('*.xml')}
            # A saved Warzone can enter the same native path without an earlier
            # kinMatchRequest. This response is one activeconnection event.
            event = ET.fromstring(service.created_assignment(identifier, 'Player1'))
            self.assertEqual(event.findtext('playlistId'), identifier)
            self.assertEqual(event.tag, 'kinAssignmentMessage')
            self.assertEqual(event.get(XSI), 'kinAssignmentMessage')
            self.assertEqual(event.findtext('requestId'), event.findtext('originRequestId'))
            self.assertEqual(UUID(event.findtext('requestId')).version, 4)
            self.assertFalse(event.findtext('buildId'))
            self.assertEqual(event.findtext('faction'), 'ISA')
            self.assertEqual(event.findtext('isSpectator'), 'false')
            self.assertIsNone(event.find('assignments'))
            for identity in ('Player1', 'Player2'):
                result = ET.fromstring(service.assign(request(identity, identifier), identity))
                self.assertEqual(result.findtext('assignments/playlistId'), identifier)
                self.assertEqual(result.findtext('assignments/currentRound'), '0')
                self.assertEqual(result.findtext('assignments/turbineContext/gameId'), '1')
                self.assertEqual(ET.tostring(event.find('turbineContext')),
                                 ET.tostring(result.find('assignments/turbineContext')))
            self.assertEqual(snapshots,
                             {p.name: p.read_bytes() for p in Path(directory).glob('*.xml')})

    def test_created_assignment_rejects_unknown_target_or_playlist_before_binding(self):
        with self.assertRaises(ValueError):
            self.service.created_assignment(PLAYLIST, 'Player3')
        with self.assertRaises(ValueError):
            self.service.created_assignment('missing', 'Player1')
        event = ET.fromstring(self.service.created_assignment(OTHER_PLAYLIST, 'Player1'))
        self.assertEqual(event.findtext('playlistId'), OTHER_PLAYLIST)
        switched = ET.fromstring(self.service.created_assignment(PLAYLIST, 'Player1'))
        self.assertEqual(switched.findtext('playlistId'), PLAYLIST)
        self.assertNotEqual(switched.findtext('gameId'), event.findtext('gameId'))


if __name__ == '__main__':
    unittest.main()
