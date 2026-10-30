"""Local-only endpoints forwarding one game's traffic through a verified TLS lease."""
import argparse
import asyncio
import json
import os
from pathlib import Path
import re
import ssl
import struct
import sys
import threading

from ping_service import ping_reply
from player_identity import player_number, validate_nickname
from local_profile import LocalProfile, TYPE, _xml
from room_http import validate_http_request
from room_protocol import IO_TIMEOUT, body_size, close_writer, read_json, write_json, write_data
from turbine_relay import read_envelope, envelope, HELLO, ACK, DATA


def server_address(value):
    host, separator, port = value.rpartition(':')
    if not separator or not host or any(c in host for c in '/\\@?# \t\r\n'):
        raise ValueError('Server must be HOST:PORT')
    if host.startswith('[') and host.endswith(']'):
        host = host[1:-1]
    try:
        port = int(port)
    except ValueError as error:
        raise ValueError('Invalid server port') from error
    if not 1 <= port <= 65535:
        raise ValueError('Invalid server port')
    return host, port


async def http_request(reader):
    async def read():
        line = await reader.readline()
        if not line:
            return None
        if len(line) > 8192 or not line.endswith(b'\r\n'):
            raise ValueError('Invalid HTTP request')
        try:
            method, path, version = line[:-2].decode('ascii').split(' ')
        except (UnicodeError, ValueError) as error:
            raise ValueError('Invalid HTTP request') from error
        if method not in ('GET', 'POST') or version not in ('HTTP/1.0', 'HTTP/1.1'):
            raise ValueError('Unsupported HTTP request')
        headers = {}
        total = len(line)
        while True:
            line = await reader.readline()
            total += len(line)
            if total > 16384 or not line.endswith(b'\r\n'):
                raise ValueError('Invalid HTTP headers')
            if line == b'\r\n':
                break
            if len(headers) >= 32:
                raise ValueError('Too many HTTP headers')
            name, separator, value = line[:-2].partition(b':')
            if not separator or not re.fullmatch(rb'[A-Za-z0-9-]+', name):
                raise ValueError('Invalid HTTP header')
            name = name.decode('ascii').lower()
            if name in headers or any(c < 32 and c != 9 or c == 127 for c in value):
                raise ValueError('Invalid HTTP header')
            headers[name] = value.decode('latin1').strip()
        if 'transfer-encoding' in headers or 'expect' in headers:
            raise ValueError('Unsupported HTTP framing')
        length = headers.get('content-length', '0')
        if not re.fullmatch(r'[0-9]{1,10}', length):
            raise ValueError('Invalid HTTP body length')
        size = body_size(int(length))
        body = await reader.readexactly(size)
        return {'method': method, 'path': path, 'headers': headers, 'size': size}, body
    return await asyncio.wait_for(read(), 30)


async def http_reply(writer, status, headers, body):
    if type(status) is not int or not 100 <= status <= 599 or not isinstance(headers, dict):
        raise ValueError('Invalid gateway HTTP response')
    lines = [f'HTTP/1.1 {status} Response', f'Content-Length: {len(body)}']
    for key, value in headers.items():
        if key.lower() not in ('content-type', 'set-cookie') or not isinstance(value, str):
            raise ValueError('Invalid gateway HTTP header')
        if len(value) > 4096 or any(ord(c) < 32 or ord(c) == 127 for c in value):
            raise ValueError('Invalid gateway HTTP header')
        lines.append(f'{key}: {value}')
    await write_data(writer, ('\r\n'.join(lines) + '\r\n\r\n').encode('latin1') + body)


class PingBridge(asyncio.DatagramProtocol):
    def __init__(self, bridge):
        self.bridge = bridge

    def datagram_received(self, data, address):
        if address[0] != '127.0.0.1' or ping_reply(data) is None:
            return
        try:
            self.bridge.pings.put_nowait((data, address))
        except asyncio.QueueFull:
            pass


class RoomBridge:
    def __init__(self, host, port, *, ca=None, ports=(17800, 17801, 17802), heartbeat_interval=10,
                 profile_directory=None):
        self.host, self.port = host, port
        self.context = ssl.create_default_context(cafile=ca)
        self.context.minimum_version = ssl.TLSVersion.TLSv1_2
        self.ports = ports  # Ephemeral values are used by tests, never native launch.
        self.heartbeat_interval = heartbeat_interval
        self.code = self.player = self.ticket = None
        self.nickname = None
        self.profile_directory = profile_directory
        self.profile = None
        self.lease_writer = None
        self.http_server = self.relay_server = self.ping_transport = None
        self.tasks = set()
        self.writers = set()
        self.channels = set()
        self.heartbeat_task = self.ping_task = None
        self.pings = asyncio.Queue(maxsize=32)
        self.failed = asyncio.Event()
        self.closed = False

    async def connect(self):
        return await asyncio.wait_for(asyncio.open_connection(
            self.host, self.port, ssl=self.context, server_hostname=self.host,
            ssl_handshake_timeout=5), IO_TIMEOUT)

    async def open_channel(self, op, **fields):
        reader, writer = await self.connect()
        self.writers.add(writer)
        try:
            await write_json(writer, {'op': op, 'ticket': self.ticket, **fields})
            response = await read_json(reader)
            if response.get('ok') is not True:
                raise ValueError('Room channel was rejected')
            return reader, writer
        except BaseException:
            self.writers.discard(writer)
            await close_writer(writer)
            raise

    async def start(self, code=None, *, nickname=''):
        validate_nickname(nickname, allow_empty=True)
        try:
            reader, writer = await self.connect()
            self.lease_writer = writer
            request = {'op': 'join', 'code': code} if code is not None else {'op': 'create'}
            request['nickname'] = nickname
            await write_json(writer, request)
            response = await read_json(reader)
            if (response.get('ok') is not True
                    or not isinstance(response.get('ticket'), str) or not 32 <= len(response['ticket']) <= 128
                    or not isinstance(response.get('code'), str)
                    or not re.fullmatch(r'[A-Z2-9]{8}', response['code'])):
                raise ValueError('Room is unavailable, code is invalid, or room is full')
            player_number(response.get('player'))
            self.code, self.player, self.ticket = response['code'], response['player'], response['ticket']
            self.nickname = validate_nickname(response.get('nickname', self.player))
            if self.nickname != (nickname or self.player):
                raise ValueError('Server returned an unexpected nickname')
            if self.profile_directory is not None:
                self.profile = LocalProfile(self.profile_directory, self.nickname)
            self.heartbeat_task = asyncio.create_task(self.heartbeat(reader, writer))
            await self.synchronize_choices()
            self.http_server = await asyncio.start_server(self.http, '127.0.0.1', self.ports[0], limit=16384)
            self.relay_server = await asyncio.start_server(self.relay, '127.0.0.1', self.ports[1])
            self.ping_transport, _ = await asyncio.get_running_loop().create_datagram_endpoint(
                lambda: PingBridge(self), local_addr=('127.0.0.1', self.ports[2]))
            self.http_address = self.http_server.sockets[0].getsockname()
            self.relay_address = self.relay_server.sockets[0].getsockname()
            self.ping_address = self.ping_transport.get_extra_info('sockname')
            ping_reader, ping_writer = await self.open_channel('ping')
            self.ping_task = asyncio.create_task(self.ping(ping_reader, ping_writer))
        except BaseException:
            await self.close()
            raise
        return {'event': 'ready', 'code': self.code, 'player': self.player, 'nickname': self.nickname}

    async def synchronize_choices(self):
        body = self.profile.snapshot_update() if self.profile is not None else None
        if body is None:
            return
        reader, writer = await self.open_channel('http')
        try:
            await write_json(writer, {'method': 'POST', 'path': '/profile/update',
                                     'headers': {'Content-Type': 'application/xml'}, 'size': len(body)})
            await write_data(writer, body)
            response = await read_json(reader)
            reply = await asyncio.wait_for(reader.readexactly(body_size(response.get('size'))), IO_TIMEOUT)
            if response.get('status') != 200:
                raise ValueError('Host could not restore saved loadouts')
            result = _xml(reply, 'jaxbCollection')
            expected = _xml(body, 'profileOwnerList').findtext('profileOwner/UUID')
            if (len(result) != 1 or result[0].get(TYPE) != 'profileUpdateResult'
                    or result[0].findtext('profileOwner') != self.profile.owner
                    or result[0].findtext('UUID') != expected
                    or result[0].findtext('statusCode') not in ('SUCCESS', 'ALREADY_APPLIED')):
                raise ValueError('Host did not acknowledge saved loadouts')
        finally:
            self.writers.discard(writer)
            await close_writer(writer)
    async def heartbeat(self, reader, writer):
        try:
            while True:
                await asyncio.sleep(self.heartbeat_interval)
                await write_json(writer, {'op': 'keepalive'})
                if (await read_json(reader)).get('ok') is not True:
                    raise ValueError('Room lease expired')
        except (OSError, ValueError, TimeoutError, asyncio.IncompleteReadError):
            self.failed.set()

    async def ping(self, reader, writer):
        try:
            while True:
                data, address = await self.pings.get()
                await write_data(writer, data)
                reply = await asyncio.wait_for(reader.readexactly(24), IO_TIMEOUT)
                if reply != data:
                    raise ValueError('Invalid RTT reply')
                self.ping_transport.sendto(reply, address)
        except (OSError, ValueError, TimeoutError, asyncio.IncompleteReadError):
            self.failed.set()
        finally:
            self.writers.discard(writer)
            await close_writer(writer)

    async def http(self, reader, writer):
        task = asyncio.current_task()
        if self.closed or len(self.tasks) >= 8:
            await close_writer(writer)
            return
        self.tasks.add(task)
        self.writers.add(writer)
        remote_writer = None
        try:
            # Capture before the first await: buffered/keepalive requests on an
            # old game's connection must not spend a later launch's allocation.
            boost_generation = self.profile.capture_boost_generation() if self.profile is not None else None
            request = await http_request(reader)
            if request is None:
                return
            remote_reader, remote_writer = await self.open_channel('http')
            while request is not None:
                metadata, request_body = request
                await write_json(remote_writer, metadata)
                await write_data(remote_writer, request_body)
                response = await read_json(remote_reader)
                size = body_size(response.get('size'))
                body = await asyncio.wait_for(remote_reader.readexactly(size), IO_TIMEOUT)
                if (self.profile is not None and response.get('status') == 200
                        and metadata['path'] in ('/profile/get', '/profile/update')):
                    _, decoded = validate_http_request(metadata['method'], metadata['path'],
                                                       metadata['headers'], request_body, self.nickname)
                    if metadata['path'] == '/profile/get':
                        body = self.profile.overlay_lookup(body)
                    else:
                        body = self.profile.apply_update(decoded, body, boost_generation=boost_generation)
                await http_reply(writer, response.get('status'), response.get('headers'), body)
                request = await http_request(reader)
        except (OSError, ValueError, TimeoutError, asyncio.IncompleteReadError):
            try:
                await http_reply(writer, 502, {}, b'')
            except (OSError, TimeoutError):
                pass
        finally:
            if remote_writer:
                self.writers.discard(remote_writer)
                await close_writer(remote_writer)
            self.writers.discard(writer)
            await close_writer(writer)
            self.tasks.discard(task)

    async def relay(self, reader, writer):
        task = asyncio.current_task()
        if self.closed or len(self.tasks) >= 8:
            await close_writer(writer)
            return
        self.tasks.add(task)
        self.writers.add(writer)
        channel = None
        remote_writer = None
        try:
            kind, requested, flags, body = await asyncio.wait_for(read_envelope(reader), IO_TIMEOUT)
            if kind != HELLO or requested in self.channels:
                raise ValueError('Invalid or duplicate local channel')
            channel = requested
            self.channels.add(channel)
            remote_reader, remote_writer = await self.open_channel('relay', channel=channel)
            await write_data(remote_writer, envelope(HELLO, channel, body, flags))

            async def pump(source, target, from_client):
                while True:
                    kind, actual, flags, body = await read_envelope(source)
                    if actual != channel or kind not in ((DATA,) if from_client else (ACK, DATA)):
                        raise ValueError('Invalid local relay channel')
                    await write_data(target, envelope(kind, channel, body, flags))
            pumps = [asyncio.create_task(pump(reader, remote_writer, True)),
                     asyncio.create_task(pump(remote_reader, writer, False))]
            try:
                done, _ = await asyncio.wait(pumps, return_when=asyncio.FIRST_COMPLETED)
                for pump in done:
                    pump.result()
            finally:
                for pump in pumps:
                    pump.cancel()
                await asyncio.gather(*pumps, return_exceptions=True)
        except (OSError, ValueError, TimeoutError, asyncio.IncompleteReadError):
            pass
        finally:
            if channel is not None:
                self.channels.discard(channel)
            if remote_writer:
                self.writers.discard(remote_writer)
                await close_writer(remote_writer)
            self.writers.discard(writer)
            await close_writer(writer)
            self.tasks.discard(task)

    async def close(self):
        if self.closed:
            return
        self.closed = True
        for server in (self.http_server, self.relay_server):
            if server:
                server.close()
        if self.ping_transport:
            self.ping_transport.close()
        owned = list(self.tasks) + [t for t in (self.heartbeat_task, self.ping_task) if t]
        for task in owned:
            task.cancel()
        for writer in tuple(self.writers):
            writer.close()
        await asyncio.gather(*owned, return_exceptions=True)
        if self.lease_writer:
            try:
                await write_json(self.lease_writer, {'op': 'stop'})
            except (OSError, TimeoutError):
                pass
            await close_writer(self.lease_writer)
        self.ticket = None
        for server in (self.http_server, self.relay_server):
            if server:
                await server.wait_closed()


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
    host, port = server_address(args.server)
    bridge = RoomBridge(host, port, ca=args.ca,
                        ports=(0, 0, 0) if args.test_ephemeral_ports else (17800, 17801, 17802),
                        profile_directory=None if args.test_ephemeral_ports else
                        Path(os.environ.get('KZSF_ONLINE_DIR') or Path(__file__).resolve().parents[1]) / 'local-profile')
    try:
        ready = await bridge.start(args.join.upper() if args.join is not None else None, nickname=args.nickname)
        print(json.dumps(ready), flush=True)
        stop = asyncio.Event()
        watch_stdin(asyncio.get_running_loop(), stop)
        waits = [asyncio.create_task(stop.wait()), asyncio.create_task(bridge.failed.wait())]
        try:
            await asyncio.wait(waits, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for task in waits:
                task.cancel()
            await asyncio.gather(*waits, return_exceptions=True)
        if bridge.failed.is_set():
            print(json.dumps({'event': 'error', 'message': 'Room connection closed'}), flush=True)
    finally:
        await bridge.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', required=True)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--create', action='store_true')
    action.add_argument('--join')
    parser.add_argument('--ca', help='Explicit trusted CA file; verification always remains enabled')
    parser.add_argument('--nickname', default='')
    parser.add_argument('--test-ephemeral-ports', action='store_true', help=argparse.SUPPRESS)
    try:
        asyncio.run(main(parser.parse_args()))
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError, TimeoutError, asyncio.IncompleteReadError):
        print(json.dumps({'event': 'error', 'message': 'Unable to open room; check server, certificate, code and local ports'}), flush=True)
        sys.exit(1)
