from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET
from playlist_store import PlaylistStore


class PlaylistStoreTest(unittest.TestCase):
    def test_native_configuration_survives_add_retry_restart_and_lookup(self):
        request = ET.parse(Path(__file__).parent / 'fixtures' / 'initial-playlist-add.xml').getroot()
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as directory:
            store = PlaylistStore(directory)
            created = ET.fromstring(store.add(request))
            self.assertEqual(created.tag, 'addPlaylistResult')
            identifier = created.findtext('playlistId')
            self.assertTrue(identifier)
            self.assertEqual(store.add(request), ET.tostring(created))
            self.assertEqual(len(list(Path(directory).glob('*.xml'))), 1)
            store = PlaylistStore(directory)
            self.assertEqual(store.lookup(identifier)['max_player_count'], 12)
            query = ET.fromstring(f'<playlistIdList><playlistIds>{identifier}</playlistIds></playlistIdList>')
            result = ET.fromstring(store.get(query))
            self.assertEqual(result.findtext('playlist/playlistId'), identifier)
            data = result.find('playlist/data')
            expected = request.find('playlistData')
            self.assertEqual(data.attrib, expected.attrib)
            self.assertEqual([ET.tostring(x) for x in data], [ET.tostring(x) for x in expected])
            versioned = ET.fromstring(f'<playlistDataVersionRequest><playlistId>{identifier}</playlistId><version>0</version></playlistDataVersionRequest>')
            self.assertEqual(store.get(versioned), ET.tostring(result))
            versioned.find('version').text = '99'
            self.assertEqual(len(ET.fromstring(store.get(versioned))), 0)
            info = ET.fromstring(store.recommend('warzone'))
            self.assertEqual(info.findtext('totalPlaylists'), '1')
            self.assertEqual(info.findtext('playlistInfo/id'), identifier)
            self.assertEqual(info.findtext('playlistInfo/tags/tag'), 'warzone')
            self.assertEqual(ET.fromstring(store.recommend('missing')).findtext('totalPlaylists'), '0')
            empty_ids = ET.fromstring('<playlistIdList/>')
            self.assertEqual(ET.fromstring(store.info(empty_ids)).findtext('totalPlaylists'), '0')
            filtered = ET.fromstring('<playlistInfoByFilterRequest><filter/><pageIndex>0</pageIndex><pageSize>1</pageSize><requiredTag>warzone</requiredTag></playlistInfoByFilterRequest>')
            page = ET.fromstring(store.info(filtered))
            self.assertEqual(page.findtext('totalPlaylists'), '1')
            self.assertEqual(page.findtext('playlistInfo/id'), identifier)
            filtered.find('pageIndex').text = '1'
            page = ET.fromstring(store.info(filtered))
            self.assertEqual(page.findtext('totalPlaylists'), '1')
            self.assertIsNone(page.find('playlistInfo'))
            ET.SubElement(filtered.find('filter'), 'unknownPredicate')
            with self.assertRaises(NotImplementedError):
                store.info(filtered)
            unknown = ET.fromstring('<playlistIdList><playlistIds>../../outside</playlistIds></playlistIdList>')
            with self.assertRaises(ValueError):
                store.get(unknown)


if __name__ == '__main__':
    unittest.main()
