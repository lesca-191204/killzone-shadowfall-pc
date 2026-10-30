"""Local GameService experiment, based on the client's observed XML contracts."""
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from hashlib import sha256
import secrets
from pathlib import Path
import time
from urllib.parse import urlsplit
import xml.etree.ElementTree as ET
import zlib
from profile_store import ProfileStore
from playlist_store import PlaylistStore
from match_service import MatchService
from event_queue import EventQueue

MAX_BODY = 4 * 1024 * 1024
LOCAL_AUTH = 'KZSF_LOCAL_TEST'
LOCAL_IDENTITIES = {
    LOCAL_AUTH: 'Player',
    f'{LOCAL_AUTH}:Player1': 'Player1',
    f'{LOCAL_AUTH}:Player2': 'Player2',
}
# Fields/type aliases follow guest schema tables. Live acceptance of this typed
# collection is a separate check from the Python HTTP/schema tests.
SETTINGS = (Path(__file__).parent / 'fixtures' / 'settings.xml').read_bytes()
PING_LIST = (Path(__file__).parent / 'fixtures' / 'ping-list.xml').read_bytes()
PLAYLIST_PATHS = frozenset(
    '/playlist/' + action for action in (
        'get', 'get-with-version', 'get-active', 'add', 'find-by-tags',
        'find-by-ids', 'find-by-filter', 'get-recommended-by-tags'))
PLAYLIST_QUERIES = {
    '/playlist/find-by-tags': 'playlistInfoByTagsRequest',
    '/playlist/find-by-filter': 'playlistInfoByFilterRequest',
    '/playlist/get-recommended-by-tags': None,  # Raw tag string: guest 0x774fd5.
}


class PrivateService(ThreadingHTTPServer):
    def __init__(self, address=('127.0.0.1', 17800), data_dir=None, *, identities=None,
                 profile_directory=None):
        if address[0] != '127.0.0.1':
            raise ValueError('This experimental service only accepts loopback clients')
        super().__init__(address, Handler)
        self.sessions = {}
        self.auth_identities = dict(LOCAL_IDENTITIES if identities is None else identities)
        self.identities = set(self.auth_identities.values())
        self.data_dir = Path(data_dir) if data_dir is not None else Path(__file__).parent / 'data'
        self.profiles = ProfileStore(profile_directory if profile_directory is not None
                                     else self.data_dir / 'profiles')
        self.playlists = PlaylistStore(self.data_dir / 'playlists')
        self.events = EventQueue(self.data_dir / 'events')
        self.matches = MatchService(self.playlists.lookup,
                                    int(ET.fromstring(SETTINGS).findtext('version')), self.identities,
                                    publish_assignment=self.events.enqueue_once)

    def record_request(self, operation, body):
        # Synthetic local accounts only. Preserve bounded protocol evidence
        # locally; names and payloads never enter the console log.
        directory = self.data_dir / 'requests'
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / f'{operation}-{sha256(body).hexdigest()}.xml'
        if not path.exists():
            path.write_bytes(body)


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def setup(self):
        super().setup()
        self.connection.settimeout(5)

    def reply(self, status, body=b'', headers=()):
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Content-Type', 'application/xml')
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def authenticated(self):
        self.online_id = None
        self.session_token = None
        cookie = SimpleCookie()
        try:
            cookie.load(self.headers.get('Cookie', ''))
        except Exception:
            return False
        session = cookie.get('kzsf_session')
        if session is not None:
            self.session_token = session.value
            self.online_id = self.server.sessions.get(session.value)
        return self.online_id is not None

    def send_events(self):
        delivery = None
        try:
            if self.server.events.connected(self.session_token):
                delivery = self.server.events.claim(self.online_id)
            body = b'<clientConnectMessage/>' + (delivery.payload if delivery else b'')
            time.sleep(1)
            self.reply(200, body)
            self.wfile.flush()
            if delivery:
                self.server.events.acknowledge(delivery)
            self.server.events.mark_connected(self.session_token)
        except OSError:
            # Keep the durable pending event for the next poll after a failed
            # response write or failed delivery-ledger write.
            self.close_connection = True
        finally:
            if delivery:
                self.server.events.release(delivery)

    def do_GET(self):
        path = urlsplit(self.path).path
        if path in ('/events/activeconnection', '/clan/myclan'):
            if not self.authenticated():
                self.reply(401, headers=(('WWW-Authenticate', 'psn-auth-code'),))
            elif path == '/clan/myclan':
                # The private service has no clans. Guest 0x4a0b00 accepts an
                # empty ServerClanList and completes startup at 0x4a0e11.
                self.reply(200, b'<clanList/>')
            else:
                # Client closes/reopens this request after EOF. A short bounded
                # poll works with the current synchronous HTTP adapter. The
                # ClientConnectMessage has no own fields (guest 0x15de310) and
                # confirms this real, authenticated event endpoint connection.
                self.send_events()
        elif path != '/auth/auth':
            self.reply(501)
        elif self.headers.get('x-psn-auth-code') not in self.server.auth_identities:
            self.reply(401, headers=(('WWW-Authenticate', 'psn-auth-code'),))
        elif len(self.server.sessions) >= 64:
            self.reply(503)
        else:
            session = secrets.token_urlsafe(24)
            self.server.sessions[session] = self.server.auth_identities[self.headers['x-psn-auth-code']]
            self.reply(200, headers=(('Set-Cookie', f'kzsf_session={session}; Path=/; HttpOnly; SameSite=Strict'),))

    def do_POST(self):
        try:
            size = int(self.headers.get('Content-Length', '-1'))
            if size < 0 or size > MAX_BODY:
                self.close_connection = True
                self.reply(413)
                return
            body = self.rfile.read(size)
            if len(body) != size:
                raise ValueError('Incomplete body')
            encoding = self.headers.get('Content-Encoding', '').lower()
            if encoding == 'gzip':
                decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
                body = decoder.decompress(body, MAX_BODY + 1)
                if len(body) > MAX_BODY or not decoder.eof or decoder.unused_data:
                    raise ValueError('Invalid or oversized gzip')
            elif encoding:
                raise ValueError('Unsupported content encoding')
        except (ValueError, zlib.error):
            self.reply(400)
            return
        if not self.authenticated():
            self.reply(401, headers=(('WWW-Authenticate', 'psn-auth-code'),))
            return
        path = urlsplit(self.path).path
        if path in ('/profile/get', '/profile/update', '/network/pinglist', '/match/message') or path in PLAYLIST_PATHS:
            try:
                operation = path.strip('/').replace('/', '-') if path in PLAYLIST_PATHS or path == '/match/message' else path.rsplit('/', 1)[-1]
                self.server.record_request(operation, body)
            except OSError:
                self.reply(503)
                return
        if path not in ('/system/settings', '/network/pinglist', '/profile/get', '/profile/update', '/match/message') and path not in PLAYLIST_PATHS:
            self.reply(501)
            return
        try:
            if path == '/network/pinglist':
                # Guest 0x45d1ac..0x45d1d0 posts one terminating NUL. This
                # request has no XML DTO. Advertise the separate native RTT
                # UDP responder; the client measures real replies itself.
                if body != b'\0':
                    raise ValueError('Unexpected ping-list request body')
                self.reply(200, PING_LIST)
                return
            if path == '/playlist/get-recommended-by-tags':
                self.reply(200, self.server.playlists.recommend(body.decode('utf-8')))
                return
            root = ET.fromstring(body)
            if path == '/match/message':
                if root.tag == 'matchCancelRequestKIN':
                    response = self.server.matches.cancel(root, self.online_id)
                elif root.tag == 'matchGameUpdateKIN':
                    response = self.server.matches.update(root, self.online_id)
                elif root.tag == 'factionSwitchRequest':
                    response = self.server.matches.switch_faction(root, self.online_id)
                else:
                    response = self.server.matches.assign(root, self.online_id)
            elif path == '/playlist/add':
                response = self.server.playlists.add(root)
                identifier = ET.fromstring(response).findtext('playlistId')
                event = self.server.matches.created_assignment(identifier, self.online_id)
                event_id = ET.fromstring(event).findtext('requestId')
                self.server.events.enqueue_once(self.online_id, 'created:' + event_id, event)
            elif path in ('/playlist/get', '/playlist/get-with-version'):
                response = self.server.playlists.get(root)
            elif path == '/playlist/find-by-ids':
                response = self.server.playlists.info(root)
            elif path in PLAYLIST_QUERIES:
                if root.tag != PLAYLIST_QUERIES[path]:
                    raise ValueError('Unexpected playlist-query DTO')
                response = self.server.playlists.info(root)
            elif path in PLAYLIST_PATHS:
                raise NotImplementedError('Unimplemented playlist operation')
            elif path == '/profile/get':
                response = self.server.profiles.lookup(root)
            elif path == '/profile/update':
                response = self.server.profiles.update(root)
            elif root.tag.rsplit('}', 1)[-1] != 'settingsDelta':
                raise ValueError('Unexpected XML root')
            else:
                response = SETTINGS
        except (ET.ParseError, ValueError):
            self.reply(400)
            return
        except OSError:
            self.reply(503)
            return
        except NotImplementedError:
            self.reply(501)
            return
        self.reply(200, response)

    def log_message(self, fmt, *args):
        # Never record auth headers, session cookies or request body/account data.
        print(f'PRIVATE SERVICE {self.command} {urlsplit(self.path).path} {args[1] if len(args)>1 else ""}', flush=True)


if __name__ == '__main__':
    with PrivateService() as server:
        print('Private GameService listening on http://127.0.0.1:17800', flush=True)
        server.serve_forever()
