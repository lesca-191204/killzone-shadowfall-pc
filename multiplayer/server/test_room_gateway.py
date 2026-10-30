"""Real TLS tests for the new outer transport, never the running game services."""
import asyncio
import json
from pathlib import Path
import shutil
import ssl
import struct
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

from room_gateway import RoomGateway
from room_bridge import RoomBridge
from room_protocol import read_json, write_json
from player_identity import MAX_PLAYERS
from turbine_relay import envelope, HELLO, ACK, DATA, read_envelope
from turbine_codec import Frame, encode_frame, decode_frame, decode_body, encode_blob, encode_join_body, encode_game_body


class RoomGatewayTest(unittest.IsolatedAsyncioTestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        root = Path(cls.directory.name)
        cls.cert, cls.key = root / 'cert.pem', root / 'key.pem'
        openssl = shutil.which('openssl')
        if not openssl:
            openssl = next((str(p) for p in (
                Path('C:/Program Files/Git/usr/bin/openssl.exe'),
                Path('C:/msys64/clang64/bin/openssl.exe')) if p.exists()), None)
        if not openssl:
            raise RuntimeError('OpenSSL is required to generate the test-only TLS certificate')
        subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-keyout', str(cls.key), '-out', str(cls.cert), '-days', '1',
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    async def asyncSetUp(self):
        self.errors = []
        asyncio.get_running_loop().set_exception_handler(lambda loop, context: self.errors.append(context))
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(self.cert, self.key)
        self.test_data = tempfile.TemporaryDirectory(dir=self.directory.name)
        self.gateway = RoomGateway(Path(self.test_data.name) / 'rooms', lease_timeout=0.7)
        await self.gateway.start(context, '127.0.0.1', 0)
        self.address = self.gateway.server.sockets[0].getsockname()
        self.bridges = []
        self.writers = []

    async def asyncTearDown(self):
        for bridge in self.bridges:
            await bridge.close()
        for writer in self.writers:
            writer.close()
        await self.gateway.close()
        self.test_data.cleanup()
        self.assertEqual(self.errors, [], 'A background connection raised an unhandled exception')

    async def bridge(self, code=None, nickname='', profile_directory=None):
        bridge = RoomBridge(*self.address, ca=str(self.cert), ports=(0, 0, 0), heartbeat_interval=0.1,
                            profile_directory=profile_directory)
        self.bridges.append(bridge)
        ready = await bridge.start(code, nickname=nickname)
        self.assertEqual(ready, {'event': 'ready', 'code': bridge.code,
                                 'player': bridge.player, 'nickname': bridge.nickname})
        return bridge

    def assign(self, bridge, playlist=None):
        room = self.gateway.room
        if playlist is None:
            request = ET.parse(Path(__file__).parent / 'fixtures' / 'initial-playlist-add.xml').getroot()
            request.find('playlistData/creator').text = bridge.nickname
            playlist = ET.fromstring(room.http.playlists.add(request)).findtext('playlistId')
        room.http.matches.created_assignment(playlist, bridge.nickname)
        return playlist

    async def raw(self, message, *, trusted=True):
        context = ssl.create_default_context(cafile=str(self.cert) if trusted else None)
        reader, writer = await asyncio.open_connection(*self.address, ssl=context, server_hostname='localhost')
        self.writers.append(writer)
        await write_json(writer, message)
        return reader, writer, await read_json(reader)

    async def test_wrong_code_full_room_cert_failure_and_cleanup(self):
        first = await self.bridge()
        self.assertEqual(first.player, 'Player1')
        _, _, wrong = await self.raw({'op': 'join', 'code': 'ZZZZZZZZ'})
        self.assertFalse(wrong['ok'])
        remaining = []
        for number in range(2, MAX_PLAYERS + 1):
            bridge = await self.bridge(first.code)
            self.assertEqual(bridge.player, f'Player{number}')
            remaining.append(bridge)
        self.assertEqual(len(self.gateway.room.members), MAX_PLAYERS)
        _, _, full = await self.raw({'op': 'join', 'code': first.code})
        self.assertFalse(full['ok'])
        self.assertEqual(len(self.gateway.room.members), MAX_PLAYERS)
        with self.assertRaises(ssl.SSLCertVerificationError):
            await self.raw({'op': 'join', 'code': first.code}, trusted=False)
        old_ticket = first.ticket
        await first.close()
        await asyncio.sleep(0.05)
        _, _, expired = await self.raw({'op': 'http', 'ticket': old_ticket})
        self.assertFalse(expired['ok'])
        await asyncio.gather(*(bridge.close() for bridge in remaining))
        for _ in range(100):
            if self.gateway.room is None and not list(self.gateway.data_root.glob('room-*')):
                break
            await asyncio.sleep(0.02)
        self.assertIsNone(self.gateway.room)
        self.assertFalse(list(self.gateway.data_root.glob('room-*')))

    async def test_custom_nickname_is_bound_to_ticket_and_duplicate_is_rejected(self):
        first = await self.bridge(nickname='Alpha_7')
        self.assertEqual((first.player, first.nickname), ('Player1', 'Alpha_7'))
        self.assertEqual(set(self.gateway.room.http.sessions.values()), {'Alpha_7'})
        _, _, duplicate = await self.raw({'op': 'join', 'code': first.code, 'nickname': 'alpha_7'})
        self.assertFalse(duplicate['ok'])
        self.assertEqual(len(self.gateway.room.members), 1)
        second = await self.bridge(first.code, nickname='Bravo')
        self.assertEqual((second.player, second.nickname), ('Player2', 'Bravo'))
        for name, expected in (('Alpha_7', 200), ('Bravo', 403), ('Player1', 403)):
            reader, writer = await asyncio.open_connection(*first.http_address)
            self.writers.append(writer)
            body = ('<jaxbCollection xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">'
                    f'<Item xsi:type="xs:string">profile/{name}</Item></jaxbCollection>').encode()
            writer.write(b'POST /profile/get HTTP/1.1\r\nHost: localhost\r\nContent-Length: '
                         + str(len(body)).encode() + b'\r\n\r\n' + body)
            await writer.drain()
            self.assertIn(f' {expected} '.encode(), await asyncio.wait_for(reader.readline(), 2))

    async def test_invalid_nickname_does_not_create_a_room(self):
        for name in ('ab', 'x' * 17, '../user', 'User name', 123, None):
            _, _, reply = await self.raw({'op': 'create', 'nickname': name})
            self.assertFalse(reply['ok'])
            self.assertIsNone(self.gateway.room)

    async def test_authenticated_http_channels_survive_idle_and_remain_usable(self):
        from unittest.mock import patch

        bridge = await self.bridge()

        async def short_http_deadline(reader, timeout=8, **kwargs):
            if timeout == 30 or kwargs.get('allow_idle'):
                timeout = 0.05
            return await read_json(reader, timeout, **kwargs)

        with patch('room_gateway.read_json', new=short_http_deadline):
            channels = [await bridge.open_channel('http') for _ in range(4)]
            self.writers.extend(writer for _, writer in channels)
            await asyncio.sleep(0.15)
            member = self.gateway.room.members[bridge.ticket]
            self.assertEqual(member.active['http'], 4, 'Idle channels must live with their valid lease')
            for reader, writer in channels:
                await write_json(writer, {'method': 'GET', 'path': '/clan/myclan',
                                          'headers': {}, 'size': 0})
                response = await read_json(reader)
                self.assertEqual(response['status'], 200)
                self.assertEqual(await reader.readexactly(response['size']), b'<clanList/>')
            # Once a frame starts, an incomplete header still has a deadline.
            reader, writer = channels[0]
            writer.write(b'\0')
            await writer.drain()
            self.assertEqual(await asyncio.wait_for(reader.read(), 1), b'')
            self.assertEqual(member.active['http'], 3)

    async def profile_request(self, bridge, path, body, *, gzip_body=False):
        import gzip

        if gzip_body:
            body = gzip.compress(body)
        reader, writer = await asyncio.open_connection(*bridge.http_address)
        self.writers.append(writer)
        headers = f'POST {path} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {len(body)}\r\n'
        if gzip_body:
            headers += 'Content-Encoding: gzip\r\n'
        writer.write(headers.encode() + b'\r\n' + body)
        await writer.drain()
        try:
            status = int((await asyncio.wait_for(reader.readline(), 3)).split()[1])
            size = None
            while (line := await asyncio.wait_for(reader.readline(), 3)) != b'\r\n':
                self.assertTrue(line, 'Profile response ended before its headers')
                if line.lower().startswith(b'content-length:'):
                    size = int(line.partition(b':')[2])
            self.assertIsNotNone(size)
            return status, await asyncio.wait_for(reader.readexactly(size), 3)
        finally:
            writer.close()
            await writer.wait_closed()

    async def test_faction_switch_is_rejected_over_tls_without_reassignment(self):
        first = await self.bridge(nickname='Alpha')
        second = await self.bridge(first.code, nickname='Bravo')
        playlist = self.assign(first)
        self.assign(second, playlist)
        matches = self.gateway.room.http.matches
        game_id = matches.game_for('Alpha')
        other_faction = matches._assigned_players['Bravo']
        for number, faction in enumerate(('HGH', 'ISA')):
            incoming = ET.fromstring(
                '<factionSwitchRequest xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" '
                'xsi:type="factionSwitchRequest"><id>switch</id><playlist/>'
                '<gameId/><targetFaction/><players><id>Bravo</id></players></factionSwitchRequest>')
            incoming.find('id').text = f'switch-{number}'
            incoming.find('playlist').text = playlist
            incoming.find('gameId').text = str(game_id)
            incoming.find('targetFaction').text = faction
            body = ET.tostring(incoming)
            status, response = await self.profile_request(first, '/match/message', body, gzip_body=True)
            self.assertEqual((status, response), (501, b''))
            self.assertEqual(matches._assigned_players['Alpha'], 'ISA')
            self.assertEqual(matches._assigned_players['Bravo'], other_faction)
            self.assertEqual(matches.game_for('Bravo'), game_id)
            self.assertEqual((await self.profile_request(first, '/match/message', body)), (501, b''))
        incoming.find('gameId').text = str(game_id + 1)
        self.assertEqual((await self.profile_request(first, '/match/message', ET.tostring(incoming)))[0], 400)
        self.assertEqual(matches._assigned_players['Alpha'], 'ISA')
        for bridge in (first, second):
            self.assertFalse(bridge.failed.is_set())
            self.assertFalse(bridge.lease_writer.is_closing())

    async def test_loadouts_survive_host_restart_without_relying_on_client_overlay(self):
        with tempfile.TemporaryDirectory(dir=self.directory.name) as directory:
            bridge = await self.bridge(nickname='Alpha', profile_directory=directory)
            update = self.loadout_update('Alpha')
            data = update.find('profileOwner/ProfileData')
            data.find('loadoutID').text = '10'
            data.find('cl_10/name').text = 'Persistent Betty'
            self.assertEqual((await self.profile_request(
                bridge, '/profile/update', ET.tostring(update)))[0], 200)
            await bridge.close()
            context, host_data = self.gateway.context, self.gateway.data_root
            await self.gateway.close()
            self.gateway = RoomGateway(host_data, lease_timeout=0.7)
            await self.gateway.start(context, '127.0.0.1', 0)
            self.address = self.gateway.server.sockets[0].getsockname()
            # Disable the PC overlay: the server itself must retain the set.
            restored = await self.bridge(nickname='Alpha')
            query = (b'<jaxbCollection xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">'
                     b'<Item xsi:type="xs:string">profile/Alpha</Item></jaxbCollection>')
            status, response = await self.profile_request(restored, '/profile/get', query)
            self.assertEqual(status, 200)
            profile = ET.fromstring(response)[0].find('ProfileData')
            self.assertEqual(profile.findtext('cl_10/name'), 'Persistent Betty')
            self.assertEqual(profile.findtext('loadoutID'), '10')
            self.assertEqual(profile.findtext('cl_10/primaryWeapon'),
                             data.findtext('cl_10/primaryWeapon'))

    async def test_every_joining_player_gets_maximum_progression_defaults(self):
        from private_progression import CHALLENGE_FLOORS, DAILY_FLOORS

        first = await self.bridge(nickname='Alpha')
        second = await self.bridge(first.code, nickname='Bravo')
        for bridge in (first, second):
            query = ET.Element('jaxbCollection')
            for prefix in ('profile/', 'challenges/', 'daily_challenges/'):
                ET.SubElement(query, 'Item', {'{http://www.w3.org/2001/XMLSchema-instance}type': 'xs:string'}).text = prefix + bridge.nickname
            status, response = await self.profile_request(bridge, '/profile/get', ET.tostring(query))
            self.assertEqual(status, 200)
            main, challenges, daily = (owner.find('ProfileData') for owner in ET.fromstring(response))
            self.assertEqual(main.findtext('{c}Total'), '2600')
            self.assertEqual(challenges.get('type'), 'challengesProfile')
            for key, floor in CHALLENGE_FLOORS.items():
                self.assertGreaterEqual(int(challenges.findtext('{ch}' + key)), floor)
            entries = dict(part.split('=') for part in daily.findtext('serializedDailyChallengeMap').split(',') if part)
            for key, floor in DAILY_FLOORS.items():
                self.assertGreaterEqual(int(entries[key].split('|')[0]), floor)

    async def test_existing_pc_loadouts_are_synchronized_before_native_startup(self):
        from local_profile import LocalProfile

        with tempfile.TemporaryDirectory(dir=self.directory.name) as directory:
            update = self.loadout_update('Alpha')
            update.find('profileOwner/ProfileData/cl_10/name').text = 'PC Betty'
            result = ET.Element('jaxbCollection')
            item = ET.SubElement(result, 'Item', {'{http://www.w3.org/2001/XMLSchema-instance}type': 'profileUpdateResult'})
            for name, value in (('profileOwner', 'profile/Alpha'),
                                ('UUID', update.findtext('profileOwner/UUID')),
                                ('statusCode', 'INVALID_UPDATE')):
                ET.SubElement(item, name).text = value
            LocalProfile(directory, 'Alpha').apply_update(ET.tostring(update), ET.tostring(result))
            bridge = await self.bridge(nickname='Alpha', profile_directory=directory)
            query = ET.fromstring(
                '<jaxbCollection xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">'
                '<Item xsi:type="xs:string">profile/Alpha</Item></jaxbCollection>')
            # Read the host directly so the bridge overlay cannot conceal failure.
            profile = ET.fromstring(self.gateway.room.http.profiles.lookup(query))[0].find('ProfileData')
            self.assertEqual(profile.findtext('cl_10/name'), 'PC Betty')

    @staticmethod
    def loadout_update(nickname, fixture='native-loadout-update.xml'):
        from uuid import uuid4

        root = ET.parse(Path(__file__).parent / 'fixtures' / fixture).getroot()
        for owner in root.findall('profileOwner'):
            owner.find('name').text = owner.findtext('name').rsplit('/', 1)[0] + '/' + nickname
            owner.find('UUID').text = str(uuid4())
        root.find('{context}OnlineID').text = nickname
        return root

    async def test_boost_inventory_resets_only_at_launch_and_preserves_local_choices(self):
        from boost_inventory import BOOST_IDS
        from local_profile import LocalProfile

        with tempfile.TemporaryDirectory(dir=self.directory.name) as directory:
            LocalProfile.reset_boosts(directory)
            bridge = await self.bridge(nickname='Alpha', profile_directory=directory)
            query = (b'<jaxbCollection xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">'
                     b'<Item xsi:type="xs:string">profile/Alpha</Item></jaxbCollection>')
            status, response = await self.profile_request(bridge, '/profile/get', query)
            self.assertEqual(status, 200)
            profile = ET.fromstring(response)[0].find('ProfileData')
            self.assertEqual({key: profile.findtext('{m}' + key) for key in BOOST_IDS},
                             dict.fromkeys(BOOST_IDS, '5'))

            choices = self.loadout_update(bridge.nickname)
            data = choices.find('profileOwner/ProfileData')
            data.find('cl_5/name').text = 'My saved loadout'
            data.find('loadoutID').text = '5'
            ET.SubElement(data, 'cardID').text = '760'
            status, response = await self.profile_request(bridge, '/profile/update', ET.tostring(choices))
            self.assertEqual(status, 200)
            self.assertEqual(ET.fromstring(response)[0].findtext('statusCode'), 'SUCCESS')

            consume = self.loadout_update(bridge.nickname)
            data = consume.find('profileOwner/ProfileData')
            for child in list(data):
                data.remove(child)
            ET.SubElement(data, '{m}GV1').text = '-1'
            body = ET.tostring(consume)
            status, response = await self.profile_request(bridge, '/profile/update', body, gzip_body=True)
            self.assertEqual(status, 200)
            self.assertEqual(ET.fromstring(response)[0].findtext('statusCode'), 'SUCCESS')
            saved = Path(directory) / 'profile.json'
            before = saved.read_bytes()
            status, response = await self.profile_request(bridge, '/profile/update', body, gzip_body=True)
            self.assertEqual(status, 200)
            self.assertEqual(ET.fromstring(response)[0].findtext('statusCode'), 'ALREADY_APPLIED')
            self.assertEqual(saved.read_bytes(), before)

            status, response = await self.profile_request(bridge, '/profile/get', query)
            self.assertEqual(status, 200)
            profile = ET.fromstring(response)[0].find('ProfileData')
            self.assertEqual(profile.findtext('{m}GV1'), '4')
            self.assertTrue(all(profile.findtext('{m}' + key) == '5' for key in BOOST_IDS if key != 'GV1'))
            self.assertEqual(saved.read_bytes(), before)
            LocalProfile.reset_boosts(directory)
            status, response = await self.profile_request(bridge, '/profile/get', query)
            self.assertEqual(status, 200)
            profile = ET.fromstring(response)[0].find('ProfileData')
            self.assertEqual({key: profile.findtext('{m}' + key) for key in BOOST_IDS},
                             dict.fromkeys(BOOST_IDS, '5'))
            self.assertEqual(profile.findtext('loadoutID'), '5')
            self.assertEqual(profile.findtext('cl_5/name'), 'My saved loadout')
            self.assertEqual(profile.findtext('cardID'), '760')

    async def test_delayed_old_launch_boost_update_cannot_spend_new_allocation(self):
        from local_profile import LocalProfile
        from unittest.mock import patch
        from uuid import uuid4

        with tempfile.TemporaryDirectory(dir=self.directory.name) as directory:
            LocalProfile.reset_boosts(directory)
            bridge = await self.bridge(nickname='Alpha', profile_directory=directory)
            query = (b'<jaxbCollection xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">'
                     b'<Item xsi:type="xs:string">profile/Alpha</Item></jaxbCollection>')
            self.assertEqual((await self.profile_request(bridge, '/profile/get', query))[0], 200)
            consume = self.loadout_update(bridge.nickname)
            data = consume.find('profileOwner/ProfileData')
            for child in list(data):
                data.remove(child)
            ET.SubElement(data, '{m}GV1').text = '-1'
            body = ET.tostring(consume)
            room = self.gateway.room
            original_request = room.request
            blocked, release = asyncio.Event(), asyncio.Event()

            async def delayed_response(*args, **kwargs):
                result = await original_request(*args, **kwargs)
                blocked.set()
                await release.wait()
                return result

            # Pause the old request after the real backend replied, while the
            # bridge still awaits that reply through TLS. Start the next game
            # allocation before allowing the old response to arrive.
            with patch.object(room, 'request', new=delayed_response):
                pending = asyncio.create_task(self.profile_request(bridge, '/profile/update', body))
                try:
                    await asyncio.wait_for(blocked.wait(), 2)
                    LocalProfile.reset_boosts(directory)
                finally:
                    release.set()
                status, response = await pending
            self.assertEqual(status, 200)
            self.assertEqual(ET.fromstring(response)[0].findtext('statusCode'), 'SUCCESS')
            status, response = await self.profile_request(bridge, '/profile/get', query)
            self.assertEqual(status, 200)
            self.assertEqual(ET.fromstring(response)[0].findtext('ProfileData/{m}GV1'), '5')
            status, response = await self.profile_request(bridge, '/profile/update', body)
            self.assertEqual(status, 200)
            self.assertEqual(ET.fromstring(response)[0].findtext('statusCode'), 'ALREADY_APPLIED')
            self.assertEqual(json.loads((Path(directory) / 'profile.json').read_text())['boosts']['GV1'], 5)

            consume.find('profileOwner/UUID').text = str(uuid4())
            status, response = await self.profile_request(bridge, '/profile/update', ET.tostring(consume))
            self.assertEqual(status, 200)
            self.assertEqual(ET.fromstring(response)[0].findtext('statusCode'), 'SUCCESS')
            status, response = await self.profile_request(bridge, '/profile/get', query)
            self.assertEqual(status, 200)
            self.assertEqual(ET.fromstring(response)[0].findtext('ProfileData/{m}GV1'), '4')

    async def test_local_profile_survives_room_recreation_and_new_nickname_and_slot(self):
        from unittest.mock import patch

        # Exercise expiry of ordinary acknowledged replays without hundreds of
        # requests; the still-rejected mixed update must remain remembered.
        with tempfile.TemporaryDirectory(dir=self.directory.name) as directory, \
                patch('local_profile.MAX_REPLAYS', 2):
            first = await self.bridge(nickname='Alpha', profile_directory=directory)
            self.assertEqual(first.player, 'Player1')
            initial_query = (b'<jaxbCollection xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">'
                             b'<Item xsi:type="xs:string">profile/Alpha</Item></jaxbCollection>')
            status, _ = await self.profile_request(first, '/profile/get', initial_query)
            self.assertEqual(status, 200)
            backend_profile = self.gateway.room.http.profiles._filename('profile/Alpha')
            backend_before = backend_profile.read_bytes()
            mixed = ET.tostring(self.loadout_update(first.nickname, 'native-profile-mixed-update.xml'))
            status, response = await self.profile_request(first, '/profile/update', mixed, gzip_body=True)
            self.assertEqual(status, 200)
            self.assertEqual([item.findtext('statusCode') for item in ET.fromstring(response)],
                             ['INVALID_UPDATE', 'SUCCESS'])
            saved = Path(directory) / 'profile.json'
            mixed_state = json.loads(saved.read_text(encoding='utf-8'))
            self.assertEqual(len(mixed_state['loadouts']), 16)
            self.assertEqual(mixed_state['loadouts']['12']['secondaryWeapon'], 741)
            self.assertEqual(backend_profile.read_bytes(), backend_before)

            update = self.loadout_update(first.nickname)
            data = update.find('profileOwner/ProfileData')
            data.find('loadoutID').text = '5'
            data.find('cl_5/name').text = 'Мой штурмовик'
            # Native field names with synthetic IDs: this checks transport and
            # persistence, not availability of particular cosmetics in the UI.
            personalization = {'cardID': '2', 'skinID': '3', 'ch_pack_ID': '4',
                               'cps_ISA_ID': '5', 'cps_HGH_ID': '6', 'cvp_ena': 'true',
                               'cvp_ISA_ID': '7', 'cvp_HGH_ID': '8'}
            for name, value in personalization.items():
                ET.SubElement(data, name).text = value
            status, response = await self.profile_request(
                first, '/profile/update', ET.tostring(update), gzip_body=True)
            self.assertEqual(status, 200)
            self.assertEqual(ET.fromstring(response)[0].findtext('statusCode'), 'SUCCESS')
            selection = self.loadout_update(first.nickname)
            selection_data = selection.find('profileOwner/ProfileData')
            for child in list(selection_data):
                if child.tag != 'loadoutID':
                    selection_data.remove(child)
            selection_data.find('loadoutID').text = '5'
            status, response = await self.profile_request(first, '/profile/update', ET.tostring(selection))
            self.assertEqual(status, 200)
            self.assertEqual(ET.fromstring(response)[0].findtext('statusCode'), 'SUCCESS')
            self.assertTrue(saved.is_file())
            before = saved.read_bytes()
            # Model delayed duplicate delivery after a newer local edit. This
            # does not assert that the native client automatically retries it.
            status, response = await self.profile_request(first, '/profile/update', mixed, gzip_body=True)
            self.assertEqual(status, 200)
            self.assertEqual([item.findtext('statusCode') for item in ET.fromstring(response)],
                             ['INVALID_UPDATE', 'ALREADY_APPLIED'])
            self.assertEqual(saved.read_bytes(), before)
            self.assertEqual(backend_profile.read_bytes(), backend_before)
            old_room = Path(self.gateway.room.storage.name)
            await first.close()
            for _ in range(100):
                if self.gateway.room is None and not old_room.exists():
                    break
                await asyncio.sleep(0.02)
            self.assertIsNone(self.gateway.room)
            self.assertFalse(old_room.exists())

            other_pc = Path(directory) / 'other-pc'
            host = await self.bridge(nickname='OtherHost', profile_directory=other_pc)
            other_query = (b'<jaxbCollection xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">'
                           b'<Item xsi:type="xs:string">profile/OtherHost</Item></jaxbCollection>')
            status, response = await self.profile_request(host, '/profile/get', other_query)
            self.assertEqual(status, 200)
            other_profile = ET.fromstring(response)[0].find('ProfileData')
            self.assertIsNone(other_profile.find('cl_5'))
            for name in personalization:
                self.assertIsNone(other_profile.find(name))
            self.assertFalse((other_pc / 'profile.json').exists())
            restored = await self.bridge(host.code, nickname='Bravo', profile_directory=directory)
            self.assertEqual(restored.player, 'Player2')
            query = (b'<jaxbCollection xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">'
                     b'<Item xsi:type="xs:string">profile/Bravo</Item></jaxbCollection>')
            status, response = await self.profile_request(restored, '/profile/get', query)
            self.assertEqual(status, 200)
            owner = ET.fromstring(response)[0]
            self.assertEqual(owner.findtext('name'), 'profile/Bravo')
            profile = owner.find('ProfileData')
            self.assertEqual(profile.findtext('loadoutID'), '5')
            self.assertEqual(ET.tostring(profile.find('cl_5')), ET.tostring(data.find('cl_5')))
            self.assertEqual(profile.findtext('cl_5/name'), 'Мой штурмовик')
            self.assertEqual(profile.findtext('cl_5/primaryWeapon'), '143')
            self.assertEqual(profile.findtext('cl_5/secondaryWeapon'), '199')
            self.assertEqual(profile.findtext('cl_12/secondaryWeapon'), '741')
            for name, value in personalization.items():
                self.assertEqual(profile.findtext(name), value)
            self.assertEqual(profile.findtext('{c}Total'), '2600')
            self.assertEqual(saved.read_bytes(), before)

    async def test_local_loadouts_reject_foreign_malformed_and_unknown_updates(self):
        with tempfile.TemporaryDirectory(dir=self.directory.name) as directory:
            bridge = await self.bridge(nickname='Alpha', profile_directory=directory)
            update = self.loadout_update(bridge.nickname)
            status, response = await self.profile_request(bridge, '/profile/update', ET.tostring(update))
            self.assertEqual(status, 200)
            self.assertEqual(ET.fromstring(response)[0].findtext('statusCode'), 'SUCCESS')
            saved = Path(directory) / 'profile.json'
            before = saved.read_bytes()

            foreign = self.loadout_update(bridge.nickname)
            foreign.find('profileOwner/name').text = 'profile/OtherPlayer'
            status, _ = await self.profile_request(bridge, '/profile/update', ET.tostring(foreign))
            self.assertEqual(status, 403)
            self.assertEqual(saved.read_bytes(), before)
            status, _ = await self.profile_request(bridge, '/profile/update', b'<profileOwnerList><broken>')
            self.assertEqual(status, 403)
            self.assertEqual(saved.read_bytes(), before)

            for unknown in (True, False):
                with self.subTest(unknown=unknown):
                    rejected = self.loadout_update(bridge.nickname)
                    loadout = rejected.find('profileOwner/ProfileData/cl_5')
                    loadout.find('name').text = 'Must not be saved'
                    if unknown:
                        ET.SubElement(loadout, 'unknown').text = '1'
                    else:
                        loadout.remove(loadout.find('primaryAbility'))
                    status, response = await self.profile_request(
                        bridge, '/profile/update', ET.tostring(rejected))
                    self.assertEqual(status, 200)
                    self.assertEqual(ET.fromstring(response)[0].findtext('statusCode'), 'INVALID_UPDATE')
                    self.assertEqual(saved.read_bytes(), before)

    async def test_http_identity_is_bound_and_ping_crosses_tls(self):
        first = await self.bridge()
        reader, writer = await asyncio.open_connection(*first.http_address)
        self.writers.append(writer)
        body = b'<jaxbCollection xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><Item xsi:type="xs:string">profile/Player2</Item></jaxbCollection>'
        writer.write(b'POST /profile/get HTTP/1.1\r\nHost: localhost\r\nCookie: kzsf_session=stolen\r\n'
                     b'x-psn-auth-code: KZSF_LOCAL_TEST:Player2\r\nContent-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body)
        await writer.drain()
        self.assertIn(b' 403 ', await asyncio.wait_for(reader.readline(), 2))
        reader2, writer2 = await asyncio.open_connection(*first.http_address)
        self.writers.append(writer2)
        body = b'<settingsDelta><knownVersion>-1</knownVersion><targetVersion>4</targetVersion></settingsDelta>'
        writer2.write(b'POST /system/settings HTTP/1.1\r\nHost: localhost\r\nx-psn-auth-code: KZSF_LOCAL_TEST:Player2\r\nContent-Length: '
                      + str(len(body)).encode() + b'\r\n\r\n' + body)
        await writer2.drain()
        self.assertIn(b' 200 ', await asyncio.wait_for(reader2.readline(), 2))
        self.assertEqual(set(self.gateway.room.http.sessions.values()), {'Player1'})
        reply = asyncio.get_running_loop().create_future()
        class Receiver(asyncio.DatagramProtocol):
            def datagram_received(self, data, address):
                if not reply.done(): reply.set_result(data)
        udp, _ = await asyncio.get_running_loop().create_datagram_endpoint(Receiver, local_addr=('127.0.0.1', 0))
        try:
            payload = b'RTT 7'.ljust(24, b'\0')
            udp.sendto(payload, first.ping_address)
            self.assertEqual(await asyncio.wait_for(reply, 3), payload)
        finally:
            udp.close()

    async def test_relay_channels_use_server_nonce_and_preserve_native_payload(self):
        first = await self.bridge()
        second = await self.bridge(first.code)
        self.assign(second, self.assign(first))
        local_readers = {}
        for channel in (20, 21):
            reader, writer = await asyncio.open_connection(*first.relay_address)
            self.writers.append(writer)
            local_readers[channel] = reader
            writer.write(envelope(HELLO, channel, struct.pack('>QHH', 123, 10000 + channel - 20, 0)))
            await writer.drain()
            kind, actual_channel, flags, payload = await asyncio.wait_for(read_envelope(reader), 3)
            self.assertEqual((kind, actual_channel, flags, payload), (ACK, channel, 0, b''))
        native_probe = await asyncio.wait_for(read_envelope(local_readers[20]), 3)
        self.assertEqual(native_probe[3], encode_frame(Frame(7, 1, 255, encode_blob(bytes(1333)))))
        member = self.gateway.room.members[first.ticket]
        self.assertNotEqual(member.nonce, 123)
        self.assertEqual(set(self.gateway.room.relays[1].clients), {member.nonce})
        _, _, duplicate = await self.raw({'op': 'relay', 'ticket': first.ticket, 'channel': 20})
        self.assertFalse(duplicate['ok'])
        _, _, unknown = await self.raw({'op': 'relay', 'ticket': 'unknown', 'channel': 21})
        self.assertFalse(unknown['ok'])
        # A second ticket using the same client-chosen nonce gets its own identity.
        reader, writer, response = await self.raw({'op': 'relay', 'ticket': second.ticket, 'channel': 20})
        self.assertTrue(response['ok'])
        writer.write(envelope(HELLO, 20, struct.pack('>QHH', 123, 10000, 0)))
        await writer.drain()
        self.assertEqual((await read_envelope(reader))[0], ACK)
        self.assertEqual(len(self.gateway.room.relays[1].clients), 2)

    async def test_lease_timeout_revokes_routes(self):
        _, writer, response = await self.raw({'op': 'create'})
        ticket = response['ticket']
        idle_reader, _, accepted = await self.raw({'op': 'http', 'ticket': ticket})
        self.assertTrue(accepted['ok'])
        await asyncio.sleep(0.85)
        self.assertEqual(await asyncio.wait_for(idle_reader.read(), 1), b'')
        _, _, denied = await self.raw({'op': 'ping', 'ticket': ticket})
        self.assertFalse(denied['ok'])

    async def test_slot_rejoin_reclaims_backend_sessions_and_local_bind_failure_releases_lease(self):
        first = await self.bridge()
        for _ in range(3):
            second = await self.bridge(first.code)
            await second.close()
            for _ in range(100):
                if len(self.gateway.room.members) == 1:
                    break
                await asyncio.sleep(0.01)
            self.assertEqual(set(self.gateway.room.http.sessions.values()), {'Player1'})
        conflict = RoomBridge(*self.address, ca=str(self.cert),
                              ports=(first.http_address[1], 0, 0), heartbeat_interval=0.1)
        self.bridges.append(conflict)
        with self.assertRaises(OSError):
            await conflict.start(first.code)
        await asyncio.sleep(0.05)
        self.assertEqual(len(self.gateway.room.members), 1)
        self.assertEqual(set(self.gateway.room.http.sessions.values()), {'Player1'})

    async def test_exit_during_event_poll_removes_revoked_session_ledger(self):
        first = await self.bridge()
        second = await self.bridge(first.code)
        member = self.gateway.room.members[second.ticket]
        token = member.cookie.partition('=')[2]
        reader, writer = await asyncio.open_connection(*second.http_address)
        self.writers.append(writer)
        writer.write(b'GET /events/activeconnection HTTP/1.1\r\nHost: localhost\r\n\r\n')
        await writer.drain()
        for _ in range(100):
            if member.jobs:
                break
            await asyncio.sleep(0.01)
        self.assertTrue(member.jobs)
        await second.close()
        await asyncio.sleep(1.1)
        self.assertNotIn(token, self.gateway.room.http.sessions)
        self.assertNotIn(token, self.gateway.room.http.events.connections)

    async def test_pending_release_reserves_nickname_until_cleanup_before_rejoin(self):
        from unittest.mock import patch
        import xml.etree.ElementTree as ET

        first = await self.bridge(nickname='Host')
        second = await self.bridge(first.code, nickname='Raven')
        room = self.gateway.room
        member = room.members[second.ticket]
        pending = asyncio.get_running_loop().create_future()
        member.jobs.add(pending)
        with patch.object(room.http.matches, 'unregister_player',
                          wraps=room.http.matches.unregister_player) as unregister:
            release = asyncio.create_task(self.gateway.release(room, member))
            try:
                await asyncio.sleep(0)
                _, _, revoked = await self.raw({'op': 'ping', 'ticket': second.ticket})
                self.assertFalse(revoked['ok'])
                _, _, busy = await self.raw({'op': 'join', 'code': first.code, 'nickname': 'Raven'})
                self.assertFalse(busy['ok'])
                self.assertFalse(release.done())
                pending.set_result(None)
                await asyncio.wait_for(release, 3)
                unregister.assert_called_once_with('Raven')
                replacement = await self.bridge(first.code, nickname='Raven')
                self.assertEqual((replacement.player, replacement.nickname), ('Player2', 'Raven'))
                self.assertEqual(set(room.http.sessions.values()), {'Host', 'Raven'})
                fixtures = Path(__file__).parent / 'fixtures'
                request = ET.fromstring((fixtures / 'initial-playlist-add.xml').read_bytes())
                request.find('playlistData/creator').text = 'Raven'
                playlist = ET.fromstring(room.http.playlists.add(request)).findtext('playlistId')
                assignment = ET.fromstring(room.http.matches.created_assignment(playlist, 'Raven'))
                report = ET.fromstring((fixtures / 'captured-match-game-update.xml').read_bytes())
                report.find('game/id').text = assignment.findtext('gameId')
                report.find('game/turbineContext/gameId').text = assignment.findtext('gameId')
                report.find('game/playlistId').text = playlist
                for player in report.findall('game/group/players/id'):
                    player.text = 'Raven'
                await asyncio.sleep(0)
                self.assertEqual(room.http.matches.update(report, 'Raven'), b'<matchResult/>')
                unregister.assert_called_once_with('Raven')
            finally:
                if not pending.done():
                    pending.set_result(None)
                await asyncio.wait_for(release, 3)

    async def test_profile_lookup_storage_is_bounded(self):
        first = await self.bridge()
        reader, writer = await asyncio.open_connection(*first.http_address)
        self.writers.append(writer)
        limit = MAX_PLAYERS * 64
        name = lambda number: f'stats/00000000-0000-4000-8000-{number:012d}/Player1'
        # Seed the accounting set, then exercise real HTTP/storage at the boundary.
        # This keeps the quota test independent of thousands of fixture writes.
        self.gateway.room.profile_names.update(name(number) for number in range(limit - 1))
        profiles = self.gateway.room.http.profiles.directory
        files_before = len(list(profiles.glob('*.xml')))
        statuses = []
        for number in (limit - 1, limit - 1, limit):
            names = [name(number)]
            body = ('<jaxbCollection xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">'
                    + ''.join(f'<Item xsi:type="xs:string">{name}</Item>' for name in names)
                    + '</jaxbCollection>').encode()
            writer.write(b'POST /profile/get HTTP/1.1\r\nHost: localhost\r\nContent-Length: '
                         + str(len(body)).encode() + b'\r\n\r\n' + body)
            await writer.drain()
            statuses.append(int((await reader.readline()).split()[1]))
            size = 0
            while (line := await reader.readline()) != b'\r\n':
                if line.lower().startswith(b'content-length:'):
                    size = int(line.partition(b':')[2])
            await reader.readexactly(size)
        self.assertEqual(statuses, [200, 200, 507])
        self.assertEqual(len(self.gateway.room.profile_names), limit)
        self.assertIn(name(limit - 1), self.gateway.room.profile_names)
        self.assertNotIn(name(limit), self.gateway.room.profile_names)
        self.assertEqual(len(list(profiles.glob('*.xml'))), files_before + 1)

    async def test_cli_ready_has_no_ticket_and_stop_or_eof_releases_room(self):
        for send_stop in (True, False):
            process = await asyncio.create_subprocess_exec(
                sys.executable, str(Path(__file__).with_name('room_bridge.py')),
                '--server', f'{self.address[0]}:{self.address[1]}', '--create',
                '--ca', str(self.cert), '--test-ephemeral-ports',
                stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
            try:
                ready = json.loads(await asyncio.wait_for(process.stdout.readline(), 5))
                self.assertEqual(set(ready), {'event', 'code', 'player', 'nickname'})
                self.assertEqual((ready['event'], ready['player']), ('ready', 'Player1'))
                if send_stop:
                    process.stdin.write(b'stop\n')
                    await process.stdin.drain()
                else:
                    process.stdin.close()
                output, errors = await asyncio.wait_for(process.communicate(), 5)
                self.assertEqual((process.returncode, output, errors), (0, b'', b''))
                for _ in range(100):
                    if self.gateway.room is None:
                        break
                    await asyncio.sleep(0.01)
                self.assertIsNone(self.gateway.room)
            finally:
                if process.returncode is None:
                    process.kill()
                    await process.wait()

    async def native_client(self, bridge):
        game_id = self.gateway.room.http.matches.game_for(bridge.nickname)
        readers, writers = {}, {}
        for channel in (20, 21):
            reader, writer = await asyncio.open_connection(*bridge.relay_address)
            self.writers.append(writer)
            readers[channel], writers[channel] = reader, writer
            writer.write(envelope(HELLO, channel, struct.pack('>QHH', 999, channel, 0)))
            await writer.drain()
            self.assertEqual((await read_envelope(reader))[0], ACK)

        async def receive():
            return decode_frame((await asyncio.wait_for(read_envelope(readers[20]), 3))[3])

        async def send(kind, body=b'', index=255):
            game = 0xffffffff if kind in (0x17, 0x21, 0x1f, 0x20) else game_id
            writers[20].write(envelope(DATA, 20, encode_frame(Frame(kind, game, index, body)), 0x40))
            await writers[20].drain()

        self.assertEqual((await receive()).message_type, 7)
        await send(0x17, encode_blob(b'\1' + bytes(1332)))
        self.assertEqual((await receive()).message_type, 8)
        await send(0x1f)
        for _ in range(22):
            await receive()
        await send(0x20, struct.pack('>I', 1000000))
        await send(0x21)
        await send(2)
        self.assertEqual((await receive()).message_type, 15)
        await send(3, encode_join_body(bridge.player.encode() + b'\0', bridge.player.encode().ljust(32, b'\0')))
        joined = await receive()
        self.assertEqual(joined.message_type, 16)
        for _ in range(decode_body(joined)['value']):
            self.assertEqual((await receive()).message_type, 17)
        self.assertEqual((await receive()).message_type, 21)
        return readers, writers, joined.client_index

    async def test_native_game_payload_is_unchanged_in_both_directions(self):
        one = await self.bridge()
        two = await self.bridge(one.code)
        self.assign(two, self.assign(one))
        readers1, writers1, index1 = await self.native_client(one)
        readers2, writers2, index2 = await self.native_client(two)
        self.assertEqual(decode_frame((await read_envelope(readers1[20]))[3]).message_type, 17)
        for source, destination, index, target, channel in (
                (writers1, readers2, index1, index2, 20),
                (writers2, readers1, index2, index1, 21)):
            body = encode_game_body(False, bytes([target]), b'\x06\x80\x10\0\xffnative\0payload')
            native = encode_frame(Frame(0x28, 1, index, body))
            packet = envelope(DATA, channel, native, 0x48)
            source[channel].write(packet[:5])
            await source[channel].drain()
            source[channel].write(packet[5:])
            await source[channel].drain()
            kind, actual, flags, received = await asyncio.wait_for(read_envelope(destination[channel]), 3)
            self.assertEqual((kind, actual, received), (DATA, channel, native))

    async def test_room_close_cleans_a_relay_still_being_created(self):
        from unittest.mock import patch

        await self.bridge()
        room = self.gateway.room
        entered, resume = asyncio.Event(), asyncio.Event()
        start_server = asyncio.start_server

        async def delayed_start(*args, **kwargs):
            entered.set()
            await resume.wait()
            return await start_server(*args, **kwargs)

        closing = None
        with patch('room_gateway.asyncio.start_server', new=delayed_start):
            creating = asyncio.create_task(room.relay_address(42))
            try:
                await asyncio.wait_for(entered.wait(), 2)
                closing = asyncio.create_task(room.close())
                await asyncio.sleep(0)
                resume.set()
                await asyncio.wait_for(asyncio.gather(creating, closing), 3)
                self.assertTrue(room.closed)
                self.assertFalse(room.relay_servers[42].is_serving())
            finally:
                resume.set()
                await asyncio.gather(*(task for task in (creating, closing) if task), return_exceptions=True)
                for server in room.relay_servers.values():
                    server.close()
                    await server.wait_closed()

    async def test_release_prunes_empty_warzone_after_pending_backend_work(self):
        first = await self.bridge(nickname='Host')
        second = await self.bridge(first.code, nickname='Raven')
        self.assign(first)
        self.assign(second)
        room = self.gateway.room
        await self.native_client(second)
        member = room.members[second.ticket]
        pending = asyncio.get_running_loop().create_future()
        member.jobs.add(pending)
        try:
            await second.close()
            for _ in range(100):
                if member.leaving and not room.relays[2].clients:
                    break
                await asyncio.sleep(0.01)
            self.assertTrue(member.leaving)
            self.assertFalse(room.relays[2].clients)
            pending.set_result(None)
            for _ in range(100):
                if member.ticket not in room.members:
                    break
                await asyncio.sleep(0.01)
            self.assertNotIn(member.ticket, room.members)
            self.assertNotIn(2, room.relays)
            self.assertIn(1, room.relays)
        finally:
            if not pending.done():
                pending.set_result(None)

    async def test_relay_rejects_unassigned_player_and_releases_channel_reservation(self):
        first = await self.bridge()
        room = self.gateway.room
        member = room.members[first.ticket]
        reader, writer, reply = await self.raw({'op': 'relay', 'ticket': first.ticket, 'channel': 20})
        self.assertTrue(reply['ok'])
        writer.write(envelope(HELLO, 20, struct.pack('>QHH', 999, 20, 0)))
        await writer.drain()
        self.assertEqual(await asyncio.wait_for(reader.read(1), 2), b'')
        self.assertFalse(member.channels)
        self.assertFalse(any(relay.clients for relay in room.relays.values()))
        self.assign(first)
        _, _, index = await self.native_client(first)
        self.assertEqual(index, 0)

    async def test_assignment_change_between_hellos_keeps_both_channels_in_one_game(self):
        first = await self.bridge(nickname='Alpha')
        self.assign(first)
        room = self.gateway.room
        member = room.members[first.ticket]
        readers, writers = {}, {}
        for channel in (20, 21):
            reader, writer, reply = await self.raw({'op': 'relay', 'ticket': first.ticket, 'channel': channel})
            self.assertTrue(reply['ok'])
            readers[channel], writers[channel] = reader, writer
            writer.write(envelope(HELLO, channel, struct.pack('>QHH', 999, channel, 0)))
            await writer.drain()
            self.assertEqual((await asyncio.wait_for(read_envelope(reader), 2))[0], ACK)
            if channel == 20:
                request = ET.parse(Path(__file__).parent / 'fixtures' / 'initial-playlist-add.xml').getroot()
                request.find('playlistData/creator').text = 'Alpha'
                request.find('playlistData/name').text = 'Another Warzone'
                playlist = ET.fromstring(room.http.playlists.add(request)).findtext('playlistId')
                self.assign(first, playlist)
                self.assertEqual(room.http.matches.game_for('Alpha'), 2)
        mtu = decode_frame((await asyncio.wait_for(read_envelope(readers[20]), 2))[3])
        self.assertEqual((mtu.message_type, mtu.game_id), (7, 1))
        self.assertEqual(set(room.relays[1].clients[member.nonce].channels), {20, 21})
        for writer in writers.values():
            writer.close()
        for _ in range(100):
            if not member.channels:
                break
            await asyncio.sleep(0.01)
        self.assertFalse(member.channels)
        _, _, index = await self.native_client(first)
        self.assertEqual(index, 0)
        self.assertIn(member.nonce, room.relays[2].clients)

    async def test_distinct_warzones_and_next_round_use_separate_native_relays(self):
        one = await self.bridge(nickname='Alpha')
        two = await self.bridge(one.code, nickname='Bravo')
        playlist = self.assign(one)
        self.assign(two)
        room = self.gateway.room
        _, writers1, index1 = await self.native_client(one)
        _, _, index2 = await self.native_client(two)
        self.assertEqual((index1, index2), (0, 0))
        self.assertEqual(set(room.relays), {1, 2})
        self.assertTrue(room.is_master('Alpha', 1))
        self.assertFalse(room.is_master('Bravo', 1))
        report = ET.parse(Path(__file__).parent / 'fixtures' / 'captured-match-game-update.xml').getroot()
        report.find('game/playlistId').text = playlist
        report.find('game/group/players/id').text = 'Alpha'
        report.find('game/nextRoundIndex').text = '0'
        self.assertEqual(room.http.matches.update(report, 'Alpha'), b'<matchResult/>')
        self.assertEqual((room.http.matches.game_for('Alpha'), room.http.matches.game_for('Bravo')), (3, 2))
        self.assertIn(1, room.relays, 'The old client must consume the assignment before transport closes')
        for writer in writers1.values():
            writer.close()
        for _ in range(100):
            if not room.members[one.ticket].channels and 1 not in room.relays:
                break
            await asyncio.sleep(0.01)
        self.assertNotIn(1, room.relays)
        _, _, new_index = await self.native_client(one)
        self.assertEqual(new_index, 0)
        self.assertEqual(set(room.relays), {2, 3})
        self.assertTrue(room.is_master('Alpha', 3))
        self.assertTrue(room.is_master('Bravo', 2))


if __name__ == '__main__':
    unittest.main()
