"""Native assignments and round generations for the room's saved Warzones.

The authenticated identity is supplied by the HTTP session, never the XML.
Only the actual relay master may select the next native round.
"""
from dataclasses import dataclass
from threading import RLock
from uuid import uuid4
import re
import xml.etree.ElementTree as ET
from player_identity import MAX_PLAYERS


XSI_NAMESPACE = 'http://www.w3.org/2001/XMLSchema-instance'
XSI_TYPE = '{' + XSI_NAMESPACE + '}type'
ET.register_namespace('xsi', XSI_NAMESPACE)
LOCAL_IDENTITIES = frozenset(('Player', 'Player1', 'Player2'))
LOCAL_GAME_ID = '1'
TRANSPORT_FIELDS = (('regionId', ''), ('nodeId', ''), ('instanceId', ''),
                    ('gameId', LOCAL_GAME_ID), ('key', ''),
                    ('ip', '127.0.0.1'), ('port', 17801))


def transport_fields(game_id):
    return tuple((name, str(game_id) if name == 'gameId' else value)
                 for name, value in TRANSPORT_FIELDS)


@dataclass(frozen=True)
class GameBinding:
    game_id: int
    playlist_id: str
    playlist_version: int
    current_round: int = 0


def _field(root, name, required=False):
    fields = root.findall(name)
    if len(fields) > 1 or (required and not fields):
        raise ValueError('Missing or duplicate match field')
    if not fields:
        return ''
    value = fields[0].text or ''
    if len(fields[0]) or len(value) > 256 or any(ord(c) < 32 for c in value):
        raise ValueError('Invalid match field')
    if required and not value:
        raise ValueError('Empty required match field')
    return value


def _put(parent, name, value):
    ET.SubElement(parent, name).text = str(value)


def _object(parent, name, type_name):
    fields = parent.findall(name)
    if len(fields) != 1 or fields[0].get(XSI_TYPE, type_name) != type_name:
        raise ValueError('Missing, duplicate or unsupported match object')
    return fields[0]


def _integer(parent, name, minimum=0, maximum=0x7fffffffffffffff):
    value = _field(parent, name, required=True)
    if not re.fullmatch(r'-?[0-9]+', value):
        raise ValueError('Invalid numeric match field')
    value = int(value)
    if not minimum <= value <= maximum:
        raise ValueError('Out of range match field')
    return value


class MatchService:
    def __init__(self, playlist_lookup, settings_version, identities=None,
                 publish_assignment=None, is_master=None):
        if type(settings_version) is not int or not 0 <= settings_version <= 0x7fffffffffffffff:
            raise ValueError('Invalid global settings version')
        self.playlist_lookup = playlist_lookup
        self.settings_version = settings_version
        # Room admission owns this collection and can revoke a departed name.
        self.identities = LOCAL_IDENTITIES if identities is None else identities
        self.publish_assignment = publish_assignment
        self.is_master = is_master
        self._lock = RLock()
        self._games = {}
        self._playlist_games = {}
        self._player_games = {}
        self._retired_games = {}
        self._next_game_id = 1
        self._assigned_players = {}
        self._build_ids = {}
        self._created_requests = {}
        self._game_reports = {}
        self._outbox = {}

    def unregister_player(self, online_id):
        """Drop match state; the room owner separately revokes admission."""
        with self._lock:
            self._assigned_players.pop(online_id, None)
            self._player_games.pop(online_id, None)
            self._build_ids.pop(online_id, None)
            self._created_requests.pop(online_id, None)
            self._game_reports.pop(online_id, None)
            for _, members in self._retired_games.values():
                members.discard(online_id)
            self._outbox = {key: value for key, value in self._outbox.items() if key[0] != online_id}

    def game_for(self, online_id):
        with self._lock:
            return self._player_games.get(online_id)

    def _new_game(self, playlist_id, playlist_version, current_round=0):
        if self._next_game_id >= 0xffffffff:
            raise ValueError('Private game IDs exhausted')
        binding = GameBinding(self._next_game_id, playlist_id, playlist_version, current_round)
        self._next_game_id += 1
        self._games[binding.game_id] = binding
        self._playlist_games[playlist_id] = binding.game_id
        return binding

    def created_assignment(self, playlist_id, online_id):
        """Return one addressed event; HTTP caller owns queueing/deduplication.

        Native MatchAssignmentHandler accepts an unsolicited assignment while
        active, and explicitly permits an empty buildId (0x1019ad9).
        """
        assignment = self._assignment(playlist_id, online_id, None)
        return ET.tostring(assignment, encoding='utf-8')

    def assign(self, request, online_id):
        if request.tag != 'kinMatchRequest' or request.get(XSI_TYPE, 'kinMatchRequest') != 'kinMatchRequest':
            raise ValueError('Unsupported match request type')
        identifier = _field(request, 'id', required=True)
        playlist_id = _field(request, 'playlist', required=True)
        build_id = _field(request, 'buildId')
        players = request.findall('players')
        # Each locally authenticated client currently submits its own request.
        # A party with several player entries needs captured identity semantics.
        if len(players) > 1:
            raise ValueError('Multi-player party requests are not implemented')
        faction = _field(players[0], 'faction') if players else ''
        if faction not in ('', 'ISA', 'HGH'):
            raise ValueError('Unsupported faction')
        assignment = self._assignment(playlist_id, online_id, identifier, build_id, faction)
        return self._match_result(identifier, assignment)

    def switch_faction(self, request, online_id):
        """Validate faction switch requests without reassigning players."""
        if (request.tag != 'factionSwitchRequest'
                or request.get(XSI_TYPE, 'factionSwitchRequest') != 'factionSwitchRequest'):
            raise ValueError('Unsupported faction switch type')
        _field(request, 'id', required=True)
        playlist_id = _field(request, 'playlist', required=True)
        game_id = _integer(request, 'gameId', 1, 0xfffffffe)
        faction = _field(request, 'targetFaction', required=True)
        if faction not in ('ISA', 'HGH'):
            raise ValueError('Unsupported faction')
        with self._lock:
            binding = self._games.get(game_id)
            if (online_id not in self.identities or online_id not in self._assigned_players
                    or self._player_games.get(online_id) != game_id
                    or binding is None or binding.playlist_id != playlist_id):
                raise ValueError('Faction switch does not match the assigned game')
            raise NotImplementedError('Mid-match faction switching is not implemented')

    @staticmethod
    def _match_result(identifier, assignment):
        # 0x76d0df retains assignments[0], and 0x76d050 dispatches it at HTTP EOF.
        result = ET.Element('matchRequestResult')
        _put(result, 'requestId', identifier)
        _put(result, 'progress', '1')
        _put(result, 'failReason', '')
        assignment.tag = 'assignments'
        result.append(assignment)
        return ET.tostring(result, encoding='utf-8')

    def cancel(self, request, online_id):
        """Acknowledge native cancellation when no matching work is pending.

        assign() completes immediately; there is no queued match to revoke.
        A late/repeated cancel must not tear down the shared assigned game.
        Native 0xd8b7f3 clears its cancel request on a base MatchResult response.
        """
        if (request.tag != 'matchCancelRequestKIN'
                or request.get(XSI_TYPE, 'matchCancelRequestKIN') != 'matchCancelRequestKIN'):
            raise ValueError('Unsupported match cancellation type')
        if online_id not in self.identities:
            raise ValueError('Unknown local identity')
        for name in ('id', 'targetId', 'playlist'):
            _field(request, name, required=True)
        return b'<matchResult/>'

    def update(self, request, online_id):
        """Acknowledge telemetry; publish assignments for a master's next round.

        The HTTP response listener is a no-op. Online transition requires an
        addressed assignment event; the client does not take the Botzone path.
        """
        if (request.tag != 'matchGameUpdateKIN'
                or request.get(XSI_TYPE, 'matchGameUpdateKIN') != 'matchGameUpdateKIN'):
            raise ValueError('Unsupported game update type')
        _field(request, 'id', required=True)
        game = _object(request, 'game', 'kinGame')
        game_id = _integer(game, 'id', 1, 0xfffffffe)
        with self._lock:
            binding = self._games.get(game_id)
            retired = self._retired_games.get(game_id)
            if online_id not in self.identities or online_id not in self._assigned_players:
                raise ValueError('Game update requires an assigned local identity')
            if retired is not None:
                binding, previous_members = retired
                if online_id not in previous_members:
                    raise ValueError('Identity was not assigned to the retired game')
            elif binding is None or self._player_games.get(online_id) != game_id:
                raise ValueError('Game update does not match the assigned game')
            next_round = self._validate_report(request, game, binding, online_id)
            if retired is not None:
                # Bounded history accepts in-flight retries from actual old
                # members. It cannot initiate another generation or overwrite
                # the latest accepted telemetry from the new game.
                self._publish_pending()
                return b'<matchResult/>'
            if next_round >= 0:
                if (self.publish_assignment is None or self.is_master is None
                        or not self.is_master(online_id, game_id)):
                    raise ValueError('Round transition requires the connected session master')
                members = {identity for identity, assigned_id in self._player_games.items()
                           if assigned_id == game_id}
                new = self._new_game(binding.playlist_id, binding.playlist_version, next_round)
                del self._games[game_id]
                self._retired_games[game_id] = (binding, members)
                # Only a bounded number of previous generations may retry.
                while len(self._retired_games) > 64:
                    del self._retired_games[next(iter(self._retired_games))]
                for identity in sorted(members):
                    self._player_games[identity] = new.game_id
                    self._created_requests.pop(identity, None)
                    assignment = self._serialize_assignment(
                        new, identity, str(uuid4()), self._build_ids.get(identity, ''))
                    self._outbox[(identity, f'round:{new.game_id}')] = ET.tostring(assignment, encoding='utf-8')
            self._game_reports[online_id] = ET.tostring(request, encoding='utf-8')
            self._publish_pending()
            return b'<matchResult/>'

    def _publish_pending(self):
        for (identity, key), payload in list(self._outbox.items()):
            self.publish_assignment(identity, key, payload)
            del self._outbox[(identity, key)]

    def _validate_report(self, request, game, binding, online_id):
        if (_integer(game, 'version') != self.settings_version
                or _field(game, 'playlistId', required=True) != binding.playlist_id):
            raise ValueError('Game update does not match the assigned game')
        playlist = self.playlist_lookup(binding.playlist_id)
        if playlist is None or playlist['version'] != binding.playlist_version:
            raise ValueError('Assigned playlist version is unavailable')
        if _integer(game, 'currentRound', 0, playlist['round_count'] - 1) != binding.current_round:
            raise ValueError('Game update has a different current round')
        next_round = _integer(game, 'nextRoundIndex', -1, playlist['round_count'] - 1)
        _integer(game, 'maxPlayerCount', 1, 0x7fffffff)
        for name in ('currentRunTime', 'expectedTimeRemaining'):
            _integer(game, name)
        if _integer(game, 'creationTime') != 0:
            raise ValueError('Unexpected assigned creation time')
        _field(game, 'buildId')
        context = _object(game, 'turbineContext', 'turbineContext')
        for name, expected in transport_fields(binding.game_id):
            if _field(context, name) != str(expected):
                raise ValueError('Game update changed the assigned transport')
        # Other players' telemetry remains untrusted report data. It neither
        # authenticates those players nor adds them to the assigned membership.
        players = set()
        for group in game.findall('group'):
            if group.get(XSI_TYPE, 'playerGroup') != 'playerGroup':
                raise ValueError('Unsupported game player group')
            _field(group, 'requestId', required=True)
            for player in group.findall('players'):
                if player.get(XSI_TYPE, 'kinPlayer') != 'kinPlayer':
                    raise ValueError('Unsupported reported player')
                identifier = _field(player, 'id', required=True)
                if identifier in players:
                    raise ValueError('Duplicate reported player')
                players.add(identifier)
        if online_id not in players:
            raise ValueError('Reporting identity is absent from its game report')
        _field(request, 'clanISA')
        _field(request, 'clanHGH')
        return next_round

    def _assignment(self, playlist_id, online_id, identifier, build_id='', faction=''):
        if online_id not in self.identities:
            raise ValueError('Unknown local identity')
        playlist = self.playlist_lookup(playlist_id)
        if (playlist is None or playlist['id'] != playlist_id
                or not isinstance(playlist['version'], int)
                or not 0 <= playlist['version'] <= 0x7fffffff
                or not isinstance(playlist['round_count'], int)
                or playlist['round_count'] < 1):
            raise ValueError('Playlist has no loadable first round')
        capacity = playlist.get('max_player_count')
        if type(capacity) is not int or not 1 <= capacity <= MAX_PLAYERS:
            raise ValueError('Invalid playlist player capacity')
        with self._lock:
            if online_id not in self.identities:
                raise ValueError('Unknown local identity')
            binding = self._games.get(self._playlist_games.get(playlist_id))
            if binding is not None and binding.playlist_version != playlist['version']:
                raise ValueError('Assigned playlist version is unavailable')
            if binding is None:
                binding = self._new_game(playlist_id, playlist['version'])
            previous_game = self._player_games.get(online_id)
            if (previous_game != binding.game_id
                    and sum(game == binding.game_id for game in self._player_games.values()) >= capacity):
                raise ValueError('Warzone is full')
            if previous_game == binding.game_id:
                faction = self._assigned_players[online_id]
            elif not faction:
                # Fresh native requests leave faction empty. Allocate new
                # identities to either team, counting the creator assignment.
                faction = min(('ISA', 'HGH'), key=lambda team:
                              sum(value == team and self._player_games.get(identity) == binding.game_id
                                  for identity, value in self._assigned_players.items()))
            if previous_game != binding.game_id:
                self._game_reports.pop(online_id, None)
                self._created_requests.pop(online_id, None)
                self._outbox = {key: value for key, value in self._outbox.items() if key[0] != online_id}
                for _, members in self._retired_games.values():
                    members.discard(online_id)
            self._player_games[online_id] = binding.game_id
            self._assigned_players[online_id] = faction
            self._build_ids[online_id] = build_id
            if identifier is None:
                identifier = self._created_requests.setdefault(online_id, str(uuid4()))
            else:
                self._created_requests.pop(online_id, None)
            return self._serialize_assignment(binding, online_id, identifier, build_id)

    def _serialize_assignment(self, binding, online_id, identifier, build_id):
        assignment = ET.Element('kinAssignmentMessage', {XSI_TYPE: 'kinAssignmentMessage'})
        for name, value in (
                ('requestId', identifier), ('originRequestId', identifier),
                # Native 0xd8eeb8/0x45f794 gates Turbine on this global settings version.
                ('version', self.settings_version), ('gameId', binding.game_id)):
            _put(assignment, name, value)
        context = ET.SubElement(assignment, 'turbineContext', {XSI_TYPE: 'turbineContext'})
        for name, value in transport_fields(binding.game_id):
            _put(context, name, value)
        # Native d8cdbc..d8cddf maps HGH to faction 1 and ISA to 0.
        # Spectator and timestamp fields keep their established defaults.
        for name, value in (
                ('faction', self._assigned_players[online_id]), ('buildId', build_id), ('playlistId', binding.playlist_id),
                ('playlistVersion', binding.playlist_version), ('currentRound', binding.current_round),
                ('gameCreationTime', 0), ('isSpectator', 'false'),
                ('clanISA', ''), ('clanHGH', '')):
            _put(assignment, name, value)
        return assignment
