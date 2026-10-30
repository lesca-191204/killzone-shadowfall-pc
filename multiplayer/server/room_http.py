"""Bound native HTTP requests to the identity authenticated by the room ticket."""
from uuid import UUID
import xml.etree.ElementTree as ET
import zlib
from player_identity import validate_nickname


MAX_BODY = 4 * 1024 * 1024
MAX_OWNERS = 32
MAX_XML_DEPTH = 64
MAX_XML_NODES = 10000
XSI_TYPE = '{http://www.w3.org/2001/XMLSchema-instance}type'
GET_PATHS = frozenset(('/auth/auth', '/events/activeconnection', '/clan/myclan'))
XML_ROOTS = {
    '/system/settings': ('settingsDelta',),
    '/profile/get': ('jaxbCollection',),
    '/profile/update': ('profileOwnerList',),
    '/match/message': ('kinMatchRequest', 'matchCancelRequestKIN', 'matchGameUpdateKIN',
                       'factionSwitchRequest'),
    '/playlist/add': ('addPlaylistRequest',),
    '/playlist/get': ('playlistIdList', 'playlistDataVersionRequest'),
    '/playlist/get-with-version': ('playlistIdList', 'playlistDataVersionRequest'),
    '/playlist/find-by-ids': ('playlistIdList',),
    '/playlist/find-by-tags': ('playlistInfoByTagsRequest',),
    '/playlist/find-by-filter': ('playlistInfoByFilterRequest',),
}
RAW_PATHS = frozenset(('/network/pinglist', '/playlist/get-recommended-by-tags'))
PROFILE_NAMESPACES = frozenset(('profile', 'stats/allplaylists', 'challenges',
                              'daily_challenges', 'playlisthistory', 'coop_stats/allplaylists'))


class _NoDTDBuilder(ET.TreeBuilder):
    def __init__(self):
        super().__init__()
        self.depth = self.nodes = 0

    def start(self, tag, attrs):
        if self.depth >= MAX_XML_DEPTH or self.nodes >= MAX_XML_NODES:
            raise ValueError('XML structure exceeds limit')
        self.depth += 1
        self.nodes += 1
        return super().start(tag, attrs)

    def end(self, tag):
        result = super().end(tag)
        self.depth -= 1
        return result

    def doctype(self, name, pubid, system):
        raise ValueError('Unsupported XML declaration')


def _owner_name(name, player):
    if not name or len(name) > 256 or not name.endswith('/' + player):
        raise ValueError('Profile identity does not match room ticket')
    namespace = name.rsplit('/', 1)[0]
    if namespace in PROFILE_NAMESPACES:
        return
    parts = namespace.split('/')
    if len(parts) == 2 and parts[0] == 'stats':
        try:
            if str(UUID(parts[1])) == parts[1]:
                return
        except ValueError:
            pass
    raise ValueError('Unsupported profile namespace')


def _one_text(root, name):
    fields = root.findall(name)
    if len(fields) != 1 or len(fields[0]) or not fields[0].text:
        raise ValueError('Missing or ambiguous identity field')
    return fields[0].text


def validate_http_request(method: str, path: str, headers: dict[str, str],
                          body: bytes, player: str) -> tuple[dict[str, str], bytes]:
    validate_nickname(player)
    if (not isinstance(path, str)
            or not isinstance(headers, dict) or not isinstance(body, bytes)
            or len(body) > MAX_BODY or len(headers) > 64):
        raise ValueError('Invalid room HTTP request')
    if not ((method == 'GET' and path in GET_PATHS)
            or (method == 'POST' and (path in XML_ROOTS or path in RAW_PATHS))):
        raise ValueError('Unsupported room HTTP endpoint')
    normalized = {}
    for name, value in headers.items():
        if (not isinstance(name, str) or not isinstance(value, str)
                or not name or len(name) > 128 or len(value) > 8192
                or any(not (c.isascii() and (c.isalnum() or c in "!#$%&'*+-.^_`|~")) for c in name)
                or any(ord(c) < 32 or ord(c) > 126 for c in value)
                or name.lower() in normalized):
            raise ValueError('Invalid or ambiguous HTTP header')
        normalized[name.lower()] = value
    encoding = normalized.get('content-encoding', '').strip().lower()
    if encoding == 'gzip':
        try:
            decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
            body = decoder.decompress(body, MAX_BODY + 1)
            if len(body) > MAX_BODY or not decoder.eof or decoder.unused_data:
                raise ValueError('Invalid or oversized compressed body')
        except zlib.error:
            raise ValueError('Invalid compressed body') from None
    elif encoding:
        raise ValueError('Unsupported content encoding')
    forwarded = {name: normalized[name.lower()] for name in ('Content-Type', 'Accept')
                 if name.lower() in normalized}
    if method == 'GET':
        if body:
            raise ValueError('Unexpected GET body')
        return forwarded, body
    if path == '/network/pinglist':
        if body != b'\0':
            raise ValueError('Invalid ping-list body')
    elif path == '/playlist/get-recommended-by-tags':
        try:
            tag = body.decode('utf-8')
        except UnicodeDecodeError:
            raise ValueError('Invalid recommendation tag') from None
        if not tag or len(tag) > 1024 or '\0' in tag:
            raise ValueError('Invalid recommendation tag')
    else:
        try:
            root = ET.fromstring(body, parser=ET.XMLParser(target=_NoDTDBuilder()))
        except (ET.ParseError, LookupError, ValueError):
            raise ValueError('Invalid XML body') from None
        tag = root.tag.rsplit('}', 1)[-1] if path == '/system/settings' else root.tag
        if tag not in XML_ROOTS[path]:
            raise ValueError('Unexpected XML root')
        for context in root.iter('{context}OnlineID'):
            if len(context) or context.text != player:
                raise ValueError('Context identity does not match room ticket')
        if path == '/profile/get':
            if not 1 <= len(root) <= MAX_OWNERS:
                raise ValueError('Invalid profile lookup batch')
            for item in root:
                if item.tag != 'Item' or item.get(XSI_TYPE) != 'xs:string' or len(item):
                    raise ValueError('Invalid profile lookup item')
                _owner_name(item.text, player)
        elif path == '/profile/update':
            owners = root.findall('profileOwner')
            if not 1 <= len(owners) <= MAX_OWNERS:
                raise ValueError('Invalid profile update batch')
            names = set()
            for owner in owners:
                name = _one_text(owner, 'name')
                _owner_name(name, player)
                if name in names:
                    raise ValueError('Duplicate profile owner')
                names.add(name)
        elif path == '/playlist/add':
            data = root.findall('playlistData')
            if len(data) != 1 or _one_text(data[0], 'creator') != player:
                raise ValueError('Playlist creator does not match room ticket')
    return forwarded, body
