"""TLS admission gateway for one isolated native game room of up to 24 players.

PrivateService and TurbineRelay stay loopback-only. No direct public backend,
PSN credentials, arbitrary destinations, or changes to native game payloads.
"""
import argparse
import asyncio
from dataclasses import dataclass, field
import http.client
import json
from pathlib import Path
import secrets
import ssl
import struct
import sys
import tempfile
import threading
import xml.etree.ElementTree as ET

from private_service import PrivateService, LOCAL_AUTH
from turbine_relay import TurbineRelay, read_envelope, envelope, HELLO, ACK, DATA
from ping_service import ping_reply
from room_http import validate_http_request
from player_identity import MAX_PLAYERS, PLAYER_IDS, validate_nickname
from room_protocol import (MAX_BODY, IO_TIMEOUT, RateLimit, ByteLimit, body_size,
                           close_writer, read_json, write_json, write_data)


class RoomService(PrivateService):
    def record_request(self, operation, body):
        pass  # Public sessions must not create unlimited diagnostic captures.

    # The inherited logger never includes cookies, tickets, or request bodies.


@dataclass
class Member:
    ticket: str
    player: str
    nickname: str
    leaving: bool = False
    nonce: int = field(default_factory=lambda: secrets.randbits(64) or 1)
    cookie: str = ''
    writers: set = field(default_factory=set)
    jobs: set = field(default_factory=set)
    channels: set = field(default_factory=set)
    relay_game: int | None = None
    active: dict = field(default_factory=lambda: {'http': 0, 'ping': 0})
    requests: RateLimit = field(default_factory=lambda: RateLimit(300))
    traffic: ByteLimit = field(default_factory=ByteLimit)


class Room:
    def __init__(self, data_root):
        self.code = ''.join(secrets.choice('ABCDEFGHJKLMNPQRSTUVWXYZ23456789') for _ in range(8))
        self.storage = tempfile.TemporaryDirectory(prefix='room-', dir=data_root)
        self.http = RoomService(('127.0.0.1', 0), self.storage.name, identities={},
                                profile_directory=Path(data_root) / 'profiles')
        self.thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        self.thread.start()
        self.relays = {}
        self.relay_servers = {}
        self.relay_lock = asyncio.Lock()
        self.closed = False
        self.members = {}
        self.jobs = set()
        self.profile_names = set()
        self.http.matches.is_master = self.is_master

    async def start(self):
        await self.relay_address(1)

    def is_master(self, identity, game_id):
        relay = self.relays.get(game_id)
        if relay is None:
            return False
        member = next((m for m in tuple(self.members.values()) if m.nickname == identity and not m.leaving), None)
        client = relay.clients.get(member.nonce) if member else None
        return client is not None and client.joined and client.index == relay.master

    async def relay_address(self, game_id):
        async with self.relay_lock:
            if self.closed:
                raise ValueError('Room is closed')
            if game_id not in self.relays:
                relay = TurbineRelay(game_id)

                async def accept(reader, writer):
                    try:
                        await relay.accept(reader, writer)
                    finally:
                        await self.prune_relays()

                server = await asyncio.start_server(accept, '127.0.0.1', 0)
                self.relays[game_id] = relay
                self.relay_servers[game_id] = server
            return self.relay_servers[game_id].sockets[0].getsockname()

    async def prune_relays(self):
        async with self.relay_lock:
            if self.closed:
                return
            assigned = {self.http.matches.game_for(m.nickname) for m in self.members.values()}
            for game_id, relay in tuple(self.relays.items()):
                # Let the native clients consume their assignment before closing the old transport.
                if game_id not in assigned and not relay.clients:
                    server = self.relay_servers.pop(game_id)
                    self.relays.pop(game_id)
                    server.close()
                    await server.wait_closed()

    def _request(self, method, path, body, headers):
        connection = http.client.HTTPConnection(*self.http.server_address, timeout=4)
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            payload = response.read(MAX_BODY + 1)
            if len(payload) > MAX_BODY:
                raise ValueError('Backend response exceeds limit')
            return response.status, dict(response.getheaders()), payload
        finally:
            connection.close()

    async def request(self, *args, member=None):
        job = asyncio.create_task(asyncio.to_thread(self._request, *args))
        self.jobs.add(job)
        if member:
            member.jobs.add(job)
        def completed(result):
            self.jobs.discard(result)
            if member:
                member.jobs.discard(result)
            if not result.cancelled():
                result.exception()  # Also retrieve a result after its caller was cancelled.
        job.add_done_callback(completed)
        return await asyncio.shield(job)

    async def authenticate(self, member):
        self.http.auth_identities[f'{LOCAL_AUTH}:{member.player}'] = member.nickname
        self.http.identities.add(member.nickname)
        status, headers, _ = await self.request('GET', '/auth/auth', b'',
                                               {'x-psn-auth-code': f'{LOCAL_AUTH}:{member.player}'}, member=member)
        if status != 200 or 'Set-Cookie' not in headers:
            raise ValueError('Backend authentication failed')
        member.cookie = headers['Set-Cookie'].split(';', 1)[0]

    async def close(self):
        async with self.relay_lock:
            if self.closed:
                return
            self.closed = True
            servers = tuple(self.relay_servers.items())
        for game_id, server in servers:
            server.close()
            relay = self.relays[game_id]
            for client in list(relay.clients.values()):
                await relay.disconnect(client)
            await server.wait_closed()
        if self.jobs:
            await asyncio.gather(*tuple(self.jobs), return_exceptions=True)
        await asyncio.to_thread(self.http.shutdown)
        self.http.server_close()
        await asyncio.to_thread(self.thread.join, 1)
        self.storage.cleanup()


class RoomGateway:
    def __init__(self, data_root, *, lease_timeout=45):
        self.data_root = Path(data_root)
        self.data_root.mkdir(parents=True, exist_ok=True)
        self.lease_timeout = lease_timeout
        self.room = None
        self.server = None
        self.lock = asyncio.Lock()
        self.tasks = set()
        self.writers = set()
        self.admissions = RateLimit(MAX_PLAYERS * 2)
        self.closed = False

    async def start(self, context, host='127.0.0.1', port=17803):
        if context is None:
            raise ValueError('TLS is required')
        self.context = context
        # Count accepted sockets before TLS, including slow/incomplete handshakes.
        self.server = await asyncio.start_server(self.accept, host, port, backlog=MAX_PLAYERS * 8 + 16)

    async def admit(self, request):
        async with self.lock:
            if self.closed or not self.admissions.take():
                raise ValueError('Room admission unavailable')
            op = request.get('op')
            nickname = validate_nickname(request.get('nickname', ''), allow_empty=True)
            if op == 'create':
                if self.room is not None:
                    raise ValueError('A room is already active')
                room = Room(self.data_root)
                try:
                    await room.start()
                except BaseException:
                    await room.close()
                    raise
                self.room = room
            elif op != 'join' or self.room is None or request.get('code') != self.room.code:
                raise ValueError('Room code is invalid or expired')
            room = self.room
            used = {member.player for member in room.members.values()}
            player = next((p for p in PLAYER_IDS if p not in used), None)
            if player is None:
                raise ValueError('Room is full')
            nickname = nickname or player
            if any(m.nickname.casefold() == nickname.casefold() for m in room.members.values()):
                raise ValueError('Nickname is already in use')
            member = Member(secrets.token_urlsafe(32), player, nickname)
            try:
                await room.authenticate(member)
            except BaseException:
                room.http.auth_identities.pop(f'{LOCAL_AUTH}:{player}', None)
                room.http.identities.discard(nickname)
                if not room.members:
                    self.room = None
                    await room.close()
                raise
            room.members[member.ticket] = member
            return room, member

    async def release(self, room, member):
        async with self.lock:
            if room.members.get(member.ticket) is not member or member.leaving:
                return
            member.leaving = True
            for writer in tuple(member.writers):
                writer.close()
            token = member.cookie.partition('=')[2]
            room.http.sessions.pop(token, None)
            member.cookie = ''
            room.http.auth_identities.pop(f'{LOCAL_AUTH}:{member.player}', None)
            room.http.identities.discard(member.nickname)
        # A backend activeconnection response can mark its token after sleeping.
        # Wait for that member's bounded in-flight work before clearing its ledger.
        if member.jobs:
            await asyncio.gather(*tuple(member.jobs), return_exceptions=True)
        with room.http.events.lock:
            room.http.events.connections.discard(token)
        async with self.lock:
            room.http.matches.unregister_player(member.nickname)
            room.members.pop(member.ticket, None)
            empty = not room.members
            if empty and self.room is room:
                self.room = None
        if empty:
            await room.close()
        else:
            await room.prune_relays()

    async def lease(self, reader, writer, request):
        room, member = await self.admit(request)
        member.writers.add(writer)
        try:
            await write_json(writer, {'ok': True, 'code': room.code, 'player': member.player,
                                      'nickname': member.nickname, 'ticket': member.ticket})
            while True:
                control = await read_json(reader, self.lease_timeout)
                if control == {'op': 'stop'}:
                    break
                if control != {'op': 'keepalive'}:
                    raise ValueError('Invalid lease control')
                await write_json(writer, {'ok': True})
        finally:
            await self.release(room, member)

    def member(self, request):
        ticket = request.get('ticket')
        room = self.room
        if (not isinstance(ticket, str) or len(ticket) > 128 or room is None
                or ticket not in room.members or room.members[ticket].leaving):
            raise ValueError('Room lease is invalid or expired')
        return room, room.members[ticket]

    async def http(self, room, member, reader, writer):
        while True:
            request = await read_json(reader, 30, allow_idle=True)
            size = body_size(request.get('size'))
            body = await asyncio.wait_for(reader.readexactly(size), IO_TIMEOUT)
            if not member.requests.take():
                raise ValueError('Request rate exceeded')
            try:
                headers, body = validate_http_request(request.get('method'), request.get('path'),
                                                      request.get('headers'), body, member.nickname)
            except ValueError:
                await write_json(writer, {'status': 403, 'headers': {}, 'size': 0})
                continue
            method, path = request['method'], request['path']
            if method == 'GET' and path == '/auth/auth':
                status, response_headers, payload = 200, {'Set-Cookie': 'kzsf_session=local; Path=/; HttpOnly'}, b''
            else:
                if path in ('/profile/get', '/profile/update'):
                    root = ET.fromstring(body)  # Already parsed without DTD by the guard.
                    names = ({item.text for item in root} if path == '/profile/get'
                             else {owner.findtext('name') for owner in root.findall('profileOwner')})
                    if len(room.profile_names | names) > MAX_PLAYERS * 64:
                        await write_json(writer, {'status': 507, 'headers': {}, 'size': 0})
                        continue
                    room.profile_names.update(names)
                if method == 'POST' and path in ('/playlist/add', '/profile/update', '/profile/get'):
                    stored = sum(p.stat().st_size for p in Path(room.storage.name).rglob('*.xml'))
                    stored += sum(p.stat().st_size for p in room.http.profiles.directory.rglob('*')
                                  if p.is_file() and p.suffix in ('.xml', '.json'))
                    if stored + len(body) > 32 * 1024 * 1024:
                        await write_json(writer, {'status': 507, 'headers': {}, 'size': 0})
                        continue
                headers['Cookie'] = member.cookie
                status, backend_headers, payload = await room.request(method, path, body, headers, member=member)
                response_headers = {'Content-Type': backend_headers.get('Content-Type', 'application/xml')}
            await write_json(writer, {'status': status, 'headers': response_headers, 'size': len(payload)})
            await write_data(writer, payload)

    async def relay(self, room, member, reader, writer, channel):
        kind, actual, flags, body = await asyncio.wait_for(read_envelope(reader), IO_TIMEOUT)
        if kind != HELLO or actual != channel:
            raise ValueError('Relay HELLO required')
        _, local, reserved = struct.unpack('>QHH', body)
        if not local or reserved:
            raise ValueError('Invalid relay HELLO')
        if member.relay_game is None:
            game_id = room.http.matches.game_for(member.nickname)
            if game_id is None:
                raise ValueError('A Warzone assignment is required')
            # A new round can be assigned between the two channel HELLOs.
            member.relay_game = game_id
        game_id = member.relay_game
        address = await room.relay_address(game_id)
        backend_reader, backend_writer = await asyncio.wait_for(asyncio.open_connection(*address), IO_TIMEOUT)
        try:
            await write_data(backend_writer, envelope(HELLO, channel, struct.pack('>QHH', member.nonce, local, 0)))

            async def pump(source, target, from_client):
                while True:
                    kind, actual, flags, body = await read_envelope(source)
                    if actual != channel or kind not in ((DATA,) if from_client else (ACK, DATA)):
                        raise ValueError('Invalid relay direction or channel')
                    if not member.traffic.take(len(body) + 16):
                        raise ValueError('Relay traffic limit exceeded')
                    await write_data(target, envelope(kind, channel, body, flags))
            tasks = [asyncio.create_task(pump(reader, backend_writer, True)),
                     asyncio.create_task(pump(backend_reader, writer, False))]
            try:
                done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
                for task in done:
                    task.result()
            finally:
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
        finally:
            await close_writer(backend_writer)

    async def ping(self, member, reader, writer):
        limiter = RateLimit(60)
        while True:
            data = await reader.readexactly(24)
            if not limiter.take() or ping_reply(data) is None:
                raise ValueError('Invalid RTT request')
            await write_data(writer, data)

    async def accept(self, reader, writer):
        task = asyncio.current_task()
        # One lease, two relay, four HTTP and one ping connection per member, plus handshakes.
        if self.closed or len(self.tasks) >= MAX_PLAYERS * 8 + 16:
            await close_writer(writer)
            return
        self.tasks.add(task)
        self.writers.add(writer)
        writer.transport.set_write_buffer_limits(65536, 16384)
        member = None
        reserved = None
        tls_established = False
        try:
            await writer.start_tls(self.context, ssl_handshake_timeout=5)
            tls_established = True
            request = await read_json(reader, 5)
            op = request.get('op')
            if op in ('create', 'join'):
                await self.lease(reader, writer, request)
                return
            room, member = self.member(request)
            if op == 'relay':
                channel = request.get('channel')
                if type(channel) is not int or channel not in (20, 21) or channel in member.channels:
                    raise ValueError('Relay channel is invalid or already connected')
                member.channels.add(channel)
                reserved = channel
            elif op in ('http', 'ping'):
                if member.active[op] >= (4 if op == 'http' else 1):
                    raise ValueError('Connection limit exceeded')
                member.active[op] += 1
                reserved = op
            else:
                raise ValueError('Unsupported room operation')
            member.writers.add(writer)
            await write_json(writer, {'ok': True})
            if op == 'relay':
                await self.relay(room, member, reader, writer, channel)
            elif op == 'http':
                await self.http(room, member, reader, writer)
            else:
                await self.ping(member, reader, writer)
        except (ValueError, OSError, TimeoutError, asyncio.IncompleteReadError):
            # No exception detail: it might contain a ticket, cookie or payload.
            if reserved is None and tls_established and not writer.is_closing():
                try:
                    await write_json(writer, {'ok': False, 'message': 'Room request rejected'})
                except (OSError, TimeoutError):
                    pass
        finally:
            if member:
                member.writers.discard(writer)
                if isinstance(reserved, int):
                    member.channels.discard(reserved)
                    if not member.channels:
                        member.relay_game = None
                elif reserved:
                    member.active[reserved] -= 1
            await close_writer(writer)
            self.writers.discard(writer)
            self.tasks.discard(task)

    async def close(self):
        self.closed = True
        if self.server:
            self.server.close()
        for writer in tuple(self.writers):
            writer.close()
        if self.tasks:
            await asyncio.gather(*tuple(self.tasks), return_exceptions=True)
        if self.room:
            room, self.room = self.room, None
            await room.close()
        if self.server:
            await self.server.wait_closed()


def watch_stdin(loop, stop):
    def read():
        while True:
            line = sys.stdin.readline()
            if not line or line.strip().lower() == 'stop':
                try:
                    loop.call_soon_threadsafe(stop.set)
                except RuntimeError:
                    pass
                return
    threading.Thread(target=read, daemon=True).start()


async def main(args):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(args.cert, args.key)
    gateway = RoomGateway(args.data_root)
    try:
        await gateway.start(context, args.host, args.port)
        if getattr(args, 'managed', False):
            host, port = gateway.server.sockets[0].getsockname()[:2]
            print(json.dumps({'event': 'ready', 'host': host, 'port': port}), flush=True)
            stop = asyncio.Event()
            watch_stdin(asyncio.get_running_loop(), stop)
            await stop.wait()
        else:
            print('Private room gateway listening with TLS', flush=True)
            await asyncio.Future()
    finally:
        await gateway.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=17803)
    parser.add_argument('--cert', required=True)
    parser.add_argument('--key', required=True)
    parser.add_argument('--data-root', default=str(Path(__file__).parent / 'room-data'))
    parser.add_argument('--managed', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        asyncio.run(main(args))
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError):
        if not args.managed:
            raise
        print(json.dumps({'event': 'error', 'message': 'Unable to start host; check certificate, address and port'}), flush=True)
        sys.exit(1)
