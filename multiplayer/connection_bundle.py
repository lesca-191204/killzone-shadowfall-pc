"""Import public host trust data and switch connections with a recoverable backup."""
from contextlib import contextmanager, ExitStack
import json
from pathlib import Path
import re
import ssl
import stat
import tempfile
from uuid import uuid4
import zipfile
import zlib

from private_session import connection_defaults
from file_lock import exclusive_file_lock

FILE_LIMITS = {'metadata.json': 4096, 'server-cert.pem': 65536}
ZIP_LIMIT = 131072


def _plain_path(path, *, directory=False):
    information = path.lstat()
    expected = stat.S_ISDIR if directory else stat.S_ISREG
    if (not expected(information.st_mode) or path.is_symlink()
            or getattr(information, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT):
        raise ValueError('The connection package must contain plain files and folders.')


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Repeated field in metadata.json.')
        result[key] = value
    return result


def _validate_files(files):
    if not isinstance(files, dict) or set(files) != set(FILE_LIMITS):
        raise ValueError('The connection package must contain only metadata.json and server-cert.pem.')
    for name, limit in FILE_LIMITS.items():
        if not isinstance(files[name], bytes) or not 0 < len(files[name]) <= limit:
            raise ValueError('Invalid connection file size.')
    try:
        metadata = json.loads(files['metadata.json'].decode('utf-8'), object_pairs_hook=_unique_object)
        required = {'version', 'server', 'certificate', 'certificate_sha256'}
        if (not isinstance(metadata, dict) or not required <= set(metadata)
                or set(metadata) - required - {'dns_name'} or type(metadata['version']) is not int
                or metadata['version'] != 1 or metadata['certificate'] != 'server-cert.pem'
                or not isinstance(metadata['certificate_sha256'], str)
                or not re.fullmatch('[0-9a-f]{64}', metadata['certificate_sha256'])):
            raise ValueError('Invalid metadata.json format.')
        server = metadata['server']
        if (not isinstance(server, str) or not re.fullmatch(r'[A-Za-z0-9.-]+:[0-9]{1,5}', server)
                or not 1 <= int(server.rpartition(':')[2]) <= 65535):
            raise ValueError('Invalid connection server address.')
        certificate = files['server-cert.pem'].decode('ascii').strip()
        if not re.fullmatch(r'-----BEGIN CERTIFICATE-----\s+[A-Za-z0-9+/=\s]+\s+-----END CERTIFICATE-----', certificate):
            raise ValueError('Expected one public certificate.')
        ssl.create_default_context(cadata=certificate)
    except (UnicodeError, RecursionError, ssl.SSLError) as error:
        raise ValueError('The connection package is damaged.') from error


def read_bundle_directory(directory):
    """Read only the public pair; a local legacy Connect.cmd is never imported."""
    directory = Path(directory)
    _plain_path(directory, directory=True)
    files = {}
    for name, limit in FILE_LIMITS.items():
        path = directory / name
        _plain_path(path)
        with path.open('rb') as stream:
            files[name] = stream.read(limit + 1)
    _validate_files(files)
    return files


def read_bundle_zip(path):
    path = Path(path)
    _plain_path(path)
    if path.stat().st_size > ZIP_LIMIT:
        raise ValueError('The connection archive is too large.')
    try:
        with zipfile.ZipFile(path) as archive:
            entries = archive.infolist()
            if len(entries) != 2 or {entry.filename for entry in entries} != set(FILE_LIMITS):
                raise ValueError('The archive must hold only metadata.json and server-cert.pem, no folders or programs.')
            files = {}
            for entry in entries:
                limit = FILE_LIMITS[entry.filename]
                if (entry.is_dir() or entry.flag_bits & 1 or stat.S_ISLNK(entry.external_attr >> 16)
                        or entry.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)
                        or not 0 < entry.file_size <= limit):
                    raise ValueError('Invalid file in the connection archive.')
                with archive.open(entry) as stream:
                    data = stream.read(limit + 1)
                if len(data) != entry.file_size:
                    raise ValueError('Invalid connection file size.')
                files[entry.filename] = data
    except (zipfile.BadZipFile, RuntimeError, NotImplementedError, EOFError, zlib.error) as error:
        raise ValueError('The connection archive is damaged.') from error
    _validate_files(files)
    return files


@contextmanager
def activate_connection(root, files, verify=None):
    """Validate before mutation; roll back the pair if its settings cannot be saved."""
    root = Path(root)
    _plain_path(root, directory=True)
    _validate_files(files)
    lock = root / '.connection-activation.lock'
    with ExitStack() as locks:
        try:
            locks.enter_context(exclusive_file_lock(lock))
        except OSError as error:
            raise ValueError('Could not lock the connection settings. '
                             'Close the other host/join window and check access to the folder.') from error
        with tempfile.TemporaryDirectory(prefix='.connection-stage-', dir=root) as temporary:
            staged_root = Path(temporary)
            staged = staged_root / 'connection'
            staged.mkdir()
            for name, data in files.items():
                (staged / name).write_bytes(data)
            # Reuse the launcher's fingerprint and DNS validation, not a parallel trust policy.
            server, certificate = connection_defaults(staged_root)
            ssl.create_default_context(cafile=str(certificate))
            if verify is not None:
                verify(server, certificate)
            active = root / 'connection'
            backup = None
            if active.exists() or active.is_symlink():
                _plain_path(active, directory=True)
                try:
                    unchanged = read_bundle_directory(active) == files
                except (OSError, ValueError):
                    unchanged = False
                if unchanged:
                    # In particular, preserve the source host's strict Connect.cmd.
                    yield server, active / 'server-cert.pem'
                    return
                backups = root / 'connection-backups'
                backups.mkdir(exist_ok=True)
                _plain_path(backups, directory=True)
                backup = backups / uuid4().hex
                active.rename(backup)
            installed = False
            try:
                staged.rename(active)
                installed = True
                yield server, active / 'server-cert.pem'
            except BaseException:
                if installed:
                    active.rename(staged)
                if backup is not None:
                    backup.rename(active)
                raise
