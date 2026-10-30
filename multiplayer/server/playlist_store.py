"""Preserve native Warzone configurations and serve confirmed lookup DTOs."""
from copy import deepcopy
from hashlib import sha256
from pathlib import Path
from threading import RLock
from uuid import UUID, uuid5, NAMESPACE_URL
import xml.etree.ElementTree as ET
from player_identity import MAX_PLAYERS
from xml_storage import XSI, atomic_write


def _max_player_count(data):
    fields = data.findall('MaxPlayerCount')
    if len(fields) != 1 or len(fields[0]):
        raise ValueError('Expected one native MaxPlayerCount')
    count = int(fields[0].text or '')
    if not 1 <= count <= MAX_PLAYERS:
        raise ValueError('Unsupported native MaxPlayerCount')
    return count


class PlaylistStore:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.lock = RLock()

    def _read(self, identifier):
        identifier = str(UUID(identifier))
        filename = self.directory / (identifier + '.xml')
        if not filename.exists():
            return None
        try:
            root = ET.parse(filename).getroot()
        except ET.ParseError as error:
            raise OSError('Invalid stored playlist XML') from error
        if root.tag != 'privatePlaylist' or root.get('id') != identifier or root.find('playlistData') is None:
            raise OSError('Invalid stored playlist identity')
        return root

    def add(self, request):
        if request.tag != 'addPlaylistRequest' or len(request.findall('playlistData')) != 1:
            raise ValueError('Expected addPlaylistRequest with one configuration')
        if any(x.tag not in ('playlistData', 'tags') for x in request):
            raise ValueError('Unsupported add-playlist field')
        data = request.find('playlistData')
        if data.get(f'{{{XSI}}}type') != 'playlistDataKIN' or not data.findall('RoundSettings'):
            raise ValueError('Expected a native KIN configuration with rounds')
        int(data.findtext('version') or '')
        _max_player_count(data)
        tags = request.findall('tags')
        if len(tags) > 64 or any(not x.text or len(x.text) > 256 or len(x) for x in tags):
            raise ValueError('Invalid playlist tags')
        # The request has no operation ID. A content-derived private ID makes
        # a retry of this exact immutable configuration idempotent.
        digest = sha256(ET.tostring(request, encoding='utf-8')).hexdigest()
        identifier = str(uuid5(NAMESPACE_URL, 'kzsf-private-playlist:' + digest))
        with self.lock:
            if self._read(identifier) is None:
                stored = ET.Element('privatePlaylist', {'id': identifier})
                stored.append(deepcopy(data))
                for tag in tags:
                    stored.append(deepcopy(tag))
                atomic_write(self.directory / (identifier + '.xml'), stored)
        response = ET.Element('addPlaylistResult')
        ET.SubElement(response, 'playlistId').text = identifier
        return ET.tostring(response, encoding='utf-8')

    def lookup(self, identifier):
        with self.lock:
            stored = self._read(identifier)
            if stored is None:
                return None
            data = stored.find('playlistData')
            return {'id': stored.get('id'), 'version': int(data.findtext('version')),
                    'round_count': len(data.findall('RoundSettings')),
                    'max_player_count': _max_player_count(data), 'data': deepcopy(data)}

    @staticmethod
    def _ids(request):
        if request.tag != 'playlistIdList' or any(x.tag != 'playlistIds' for x in request):
            raise ValueError('Expected playlistIdList')
        if len(request) > 64:
            raise ValueError('Expected bounded playlist ID list')
        return [str(UUID(x.text or '')) for x in request]

    def get(self, request):
        version = None
        if request.tag == 'playlistDataVersionRequest':
            identifiers = [str(UUID(request.findtext('playlistId') or ''))]
            version = int(request.findtext('version') or '')
        else:
            identifiers = self._ids(request)
        response = ET.Element('playlistsResult')
        with self.lock:
            for identifier in identifiers:
                record = self.lookup(identifier)
                if record is None or (version is not None and record['version'] != version):
                    continue
                playlist = ET.SubElement(response, 'playlist', {f'{{{XSI}}}type': 'playlist'})
                ET.SubElement(playlist, 'playlistId').text = identifier
                data = record['data']
                data.tag = 'data'
                playlist.append(data)
        return ET.tostring(response, encoding='utf-8')

    @staticmethod
    def _info_response(records, total=None):
        response = ET.Element('playlistInfoResult')
        for stored in records:
            info = ET.SubElement(response, 'playlistInfo', {f'{{{XSI}}}type': 'playlistInfo'})
            ET.SubElement(info, 'id').text = stored.get('id')
            timestamp = stored.findtext('playlistData/createdTimestamp', '0')
            for tag in stored.findall('tags'):
                item = ET.SubElement(info, 'tags', {f'{{{XSI}}}type': 'tag'})
                ET.SubElement(item, 'tag').text = tag.text
                ET.SubElement(item, 'creationTime').text = timestamp
        ET.SubElement(response, 'totalPlaylists').text = str(len(records) if total is None else total)
        return ET.tostring(response, encoding='utf-8')

    def recommend(self, tag):
        if not tag or len(tag) > 1024 or '\0' in tag:
            raise ValueError('Invalid recommendation tag')
        return self._info_response(self._find_tags([tag], ''))

    def _find_tags(self, tags, required):
        records = []
        with self.lock:
            for filename in sorted(self.directory.glob('*.xml')):
                stored = self._read(filename.stem)
                known = {x.text for x in stored.findall('tags')}
                # The client constructs this owner filter from account_ + ID.
                creator = stored.findtext('playlistData/creator')
                if creator:
                    known.add('account_' + creator)
                if all(x in known for x in tags) and (not required or required in known):
                    records.append(stored)
        return records

    def info(self, request):
        if request.tag == 'playlistIdList':
            with self.lock:
                return self._info_response([stored for identifier in self._ids(request)
                                            if (stored := self._read(identifier)) is not None])
        if request.tag == 'playlistInfoByTagsRequest':
            return self._info_response(self._find_tags([x.text or '' for x in request.findall('tags')],
                                                       request.findtext('requiredTag', '')))
        if request.tag == 'playlistInfoByFilterRequest':
            filters = request.findall('filter')
            if len(filters) != 1 or len(filters[0]):
                raise NotImplementedError('Nonempty filter predicates are not implemented')
            page = int(request.findtext('pageIndex') or '')
            size = int(request.findtext('pageSize') or '')
            if not 0 <= page <= 10000 or not 1 <= size <= 100:
                raise ValueError('Invalid playlist pagination')
            records = self._find_tags([], request.findtext('requiredTag', ''))
            return self._info_response(records[page * size:(page + 1) * size], total=len(records))
        # Arbitrary predicates need their actual operator contract before use.
        raise NotImplementedError('Playlist filter operators are not implemented')
