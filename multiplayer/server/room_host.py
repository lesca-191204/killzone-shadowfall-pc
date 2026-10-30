"""Prepare a private TLS room gateway on this PC's virtual-LAN IPv4.

Any virtual LAN works: Radmin VPN (26.x.x.x, found by itself), Tailscale (100.64.0.0/10, read
from its status), or another VPN or home network with --host-ip (private ranges only, so the
gateway is never opened on an Internet address).
Share Host-connection.zip, never room-host/ (which contains the private key).
"""
from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import hashlib
import io
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import ssl
import stat
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from file_lock import exclusive_file_lock


PORT = 17803
TAILSCALE_NETWORK = ipaddress.IPv4Network('100.64.0.0/10')
RADMIN_NETWORK = ipaddress.IPv4Network('26.0.0.0/8')
# Virtual LANs (Tailscale, Radmin VPN, Hamachi) and private networks (ZeroTier, home LAN).
VPN_NETWORKS = (TAILSCALE_NETWORK, RADMIN_NETWORK, ipaddress.IPv4Network('25.0.0.0/8'),
                ipaddress.IPv4Network('10.0.0.0/8'), ipaddress.IPv4Network('172.16.0.0/12'),
                ipaddress.IPv4Network('192.168.0.0/16'))


@dataclass(frozen=True)
class HostIdentity:
    dns_name: str
    ipv4: str


@dataclass(frozen=True)
class PreparedHost:
    identity: HostIdentity
    key: Path
    cert: Path
    client_dir: Path


def validate_identity(identity: HostIdentity) -> HostIdentity:
    dns = identity.dns_name  # Tailscale's name; empty for other networks (the IP is used)
    if not isinstance(dns, str) or (dns and (not 3 <= len(dns) <= 253 or '.' not in dns)):
        raise ValueError('The host DNS name is invalid')
    if dns and any(not re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?', label)
                   for label in dns.split('.')):
        raise ValueError('The host DNS name is invalid')
    try:
        address = ipaddress.IPv4Address(identity.ipv4)
    except (ValueError, TypeError) as exc:
        raise ValueError('The host IPv4 is invalid') from exc
    if not any(address in network for network in VPN_NETWORKS) or str(address) != identity.ipv4:
        raise ValueError('The host IPv4 must be a virtual LAN or private address '
                         '(Radmin 26.x, Tailscale 100.64-127.x, 10.x, 172.16-31.x, 192.168.x)')
    return identity


def host_name(identity: HostIdentity) -> str:
    """What the other players connect to and the certificate names: DNS name, else the IP."""
    return identity.dns_name or identity.ipv4


def local_ipv4() -> list[str]:
    """This PC's IPv4 addresses (every adapter Windows lists for the host name)."""
    import socket
    try:
        infos = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
    except OSError:
        return []
    return sorted({info[4][0] for info in infos})


def identity_from_status(status) -> HostIdentity:
    if (not isinstance(status, dict) or status.get('BackendState') != 'Running'
            or status.get('TUN') is not True or status.get('AuthURL')):
        raise ValueError('Open Tailscale, sign in and connect before starting the host')
    own = status.get('Self')
    if not isinstance(own, dict) or own.get('Online') is not True:
        raise ValueError('This PC is not online in Tailscale')
    dns = own.get('DNSName')
    if not isinstance(dns, str):
        raise ValueError('This PC has no Tailscale DNS name')
    dns = dns.removesuffix('.').lower()
    addresses = own.get('TailscaleIPs')
    if not isinstance(addresses, list) or not 1 <= len(addresses) <= 32:
        raise ValueError('This PC has no Tailscale IP address')
    try:
        parsed = [ipaddress.ip_address(value) for value in addresses
                  if isinstance(value, str)]
    except ValueError as exc:
        raise ValueError('Tailscale returned an invalid address') from exc
    ipv4 = [str(address) for address in parsed if address.version == 4]
    if len(parsed) != len(addresses) or len(ipv4) != 1:
        raise ValueError('This PC must have exactly one Tailscale IPv4 address')
    return validate_identity(HostIdentity(dns, ipv4[0]))


def run_checked(argv, *, timeout=30) -> bytes:
    try:
        result = subprocess.run(argv, check=False, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=timeout, shell=False,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ValueError('A required local tool could not complete') from exc
    if result.returncode:
        # Tool diagnostics are deliberately not echoed: key material stays private.
        raise ValueError('A required local tool returned an error')
    return result.stdout


def find_tailscale() -> str:
    candidates = [shutil.which('tailscale'),
                  str(Path(os.environ.get('ProgramFiles', 'C:/Program Files'))
                      / 'Tailscale' / 'tailscale.exe')]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(candidate)
    raise ValueError('Install Tailscale and sign in, then run this host again')


def discover_identity(executable=None, host_ip=None) -> HostIdentity:
    """--host-ip if given, else Radmin VPN's address, else Tailscale's."""
    if host_ip:
        return validate_identity(HostIdentity('', host_ip))
    radmin = [ip for ip in local_ipv4() if ipaddress.IPv4Address(ip) in RADMIN_NETWORK]
    if len(radmin) == 1:
        return validate_identity(HostIdentity('', radmin[0]))
    if executable is None:
        try:
            executable = find_tailscale()
        except ValueError:
            raise ValueError("No virtual LAN found: open Radmin VPN (or Tailscale) and join the "
                             "network, or give this PC's address with --host-ip") from None
    result = run_checked([executable, 'status', '--json'], timeout=10)
    if len(result) > 4 * 1024 * 1024:
        raise ValueError('Tailscale status is too large')
    try:
        status = json.loads(result)
    except (ValueError, UnicodeError) as exc:
        raise ValueError('Tailscale status could not be read') from exc
    return identity_from_status(status)


def find_openssl(root=None) -> str:
    root = Path(root) if root is not None else Path(__file__).resolve().parents[1]
    candidates = [root / 'port-source/out/openssl.exe',
                  shutil.which('openssl'), 'C:/msys64/clang64/bin/openssl.exe',
                  'C:/Program Files/Git/usr/bin/openssl.exe', 'C:/msys64/usr/bin/openssl.exe']
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(candidate)
    raise ValueError('OpenSSL is required to prepare the host certificate')


def _plain_path(path: Path, *, directory=False):
    """Do not follow a substituted key directory, junction or symlink."""
    info = path.lstat()
    if (stat.S_ISLNK(info.st_mode)
            or getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
            or (directory and not stat.S_ISDIR(info.st_mode))
            or (not directory and not stat.S_ISREG(info.st_mode))):
        raise ValueError('Host files must be regular files in their original directories')


def _windows_account_sid() -> str:
    # Account names may use the local Windows codepage; the SID is ASCII.
    output = run_checked(['whoami.exe', '/user', '/fo', 'csv', '/nh']).decode('utf-8', errors='replace')
    rows = list(csv.reader(output.splitlines()))
    if len(rows) != 1 or len(rows[0]) != 2 or not re.fullmatch(r'S-1-[0-9-]+', rows[0][1]):
        raise ValueError('Could not identify the current Windows account')
    return rows[0][1]


def _private_directory(path: Path):
    if os.name != 'nt':
        path.chmod(0o700)
        return
    sid = _windows_account_sid()
    run_checked(['icacls.exe', str(path), '/inheritance:r', '/grant:r',
                 '*' + sid + ':(OI)(CI)(F)', '*S-1-5-18:(OI)(CI)(F)'])


def _validate_key_permissions(host: Path, key: Path):
    """Reject restored/broadened private-key permissions, without changing them."""
    if os.name != 'nt':
        if any(path.stat().st_mode & 0o077 or path.stat().st_uid != os.geteuid()
               for path in (host, key)):
            raise ValueError('Existing host key permissions are not private; files were preserved')
        return
    sid = _windows_account_sid()
    rows = [_windows_permissions(path) for path in (host, key)]
    allowed = {sid, 'S-1-5-18'}
    if (not isinstance(rows, list) or len(rows) != 2
            or any(not isinstance(row, dict) or row.get('owner') != sid
                   or not isinstance(row.get('allow'), list) or not row['allow']
                   or any(principal not in allowed for principal in row['allow']) for row in rows)
            or rows[0].get('protected') is not True):
        raise ValueError('Existing host key permissions are not private; files were preserved')


def _windows_permissions(path: Path) -> dict:
    """Read owner and ordinary allow ACEs with Win32, without changing the DACL."""
    import ctypes
    from ctypes import wintypes
    pointer = ctypes.c_void_p
    advapi = ctypes.WinDLL('advapi32', use_last_error=True)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    advapi.GetNamedSecurityInfoW.argtypes = [wintypes.LPWSTR, wintypes.DWORD, wintypes.DWORD,
                                           ctypes.POINTER(pointer), pointer, ctypes.POINTER(pointer),
                                           pointer, ctypes.POINTER(pointer)]
    advapi.GetNamedSecurityInfoW.restype = wintypes.DWORD
    advapi.GetSecurityDescriptorControl.argtypes = [pointer, ctypes.POINTER(wintypes.WORD),
                                                   ctypes.POINTER(wintypes.DWORD)]
    advapi.GetSecurityDescriptorControl.restype = wintypes.BOOL
    advapi.GetAce.argtypes = [pointer, wintypes.DWORD, ctypes.POINTER(pointer)]
    advapi.GetAce.restype = wintypes.BOOL
    advapi.ConvertSidToStringSidW.argtypes = [pointer, ctypes.POINTER(pointer)]
    advapi.ConvertSidToStringSidW.restype = wintypes.BOOL
    kernel.LocalFree.argtypes = [pointer]
    kernel.LocalFree.restype = pointer

    def sid_text(value):
        text = pointer()
        if not value or not advapi.ConvertSidToStringSidW(value, ctypes.byref(text)):
            raise ValueError('Could not verify existing host key permissions')
        try:
            return ctypes.wstring_at(text.value)
        finally:
            kernel.LocalFree(text)

    owner, dacl, descriptor = pointer(), pointer(), pointer()
    # SE_FILE_OBJECT=1, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION=5.
    result = advapi.GetNamedSecurityInfoW(str(path), 1, 5, ctypes.byref(owner), None,
                                         ctypes.byref(dacl), None, ctypes.byref(descriptor))
    if result:
        raise ValueError('Could not verify existing host key permissions')
    try:
        control, revision = wintypes.WORD(), wintypes.DWORD()
        if not dacl or not advapi.GetSecurityDescriptorControl(descriptor, ctypes.byref(control),
                                                               ctypes.byref(revision)):
            raise ValueError('Existing host key permissions are not private; files were preserved')
        # ACL header: revision/sbz1 (2), size (2), ACE count (2), sbz2 (2).
        count = ctypes.c_uint16.from_address(dacl.value + 4).value
        if count > 1024:
            raise ValueError('Could not verify existing host key permissions')
        allows = []
        for index in range(count):
            ace = pointer()
            if not advapi.GetAce(dacl, index, ctypes.byref(ace)):
                raise ValueError('Could not verify existing host key permissions')
            kind = ctypes.c_ubyte.from_address(ace.value).value
            if kind == 0:  # ACCESS_ALLOWED_ACE: header(4), mask(4), SID.
                allows.append(sid_text(ace.value + 8))
            elif kind != 1:  # Ordinary deny ACEs can only remove access.
                raise ValueError('Unsupported host key permissions; files were preserved')
        return {'owner': sid_text(owner), 'protected': bool(control.value & 0x1000), 'allow': allows}
    finally:
        kernel.LocalFree(descriptor)


def _validate_certificate(cert: Path, key: Path, identity: HostIdentity, openssl: str):
    for file, limit in ((cert, 65536), (key, 16384)):
        _plain_path(file)
        if not 1 <= file.stat().st_size <= limit:
            raise ValueError('Existing host certificate or key is invalid; files were preserved')
    try:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        # An externally replaced encrypted key must fail, never prompt for a password.
        context.load_cert_chain(str(cert), str(key), password=b'')
        run_checked([openssl, 'x509', '-in', str(cert), '-noout', '-checkend', '86400'])
        run_checked([openssl, 'verify', '-CAfile', str(cert), str(cert)])
        output = run_checked([openssl, 'x509', '-in', str(cert), '-noout',
                              '-ext', 'subjectAltName']).decode('ascii')
        names = output.partition('\n')[2].strip()
        expected = {f'IP Address:{identity.ipv4}'} | ({f'DNS:{identity.dns_name}'} if identity.dns_name else set())
        actual = [value.strip() for value in names.split(',')]
        if len(actual) != len(expected) or set(actual) != expected:
            raise ValueError('Certificate identity does not match this host')
    except (ValueError, OSError, UnicodeError) as exc:
        raise ValueError('Existing certificate/key is invalid, expired or belongs to another host; '
                         'files were preserved') from exc


def _bundle_files(cert: Path, identity: HostIdentity) -> dict[str, bytes]:
    certificate = cert.read_bytes()
    der = ssl.PEM_cert_to_DER_cert(certificate.decode('ascii'))
    metadata = {'version': 1, 'server': f'{identity.ipv4}:{PORT}',
                'certificate': 'server-cert.pem',
                'certificate_sha256': hashlib.sha256(der).hexdigest()}
    if identity.dns_name:
        metadata['dns_name'] = identity.dns_name
    command = ('@echo off\r\n'
               f'call "%~dp0..\\Private Multiplayer.cmd" --server "{host_name(identity)}:{PORT}" '
               '--ca "%~dp0server-cert.pem"\r\n')
    return {'server-cert.pem': certificate,
            'metadata.json': (json.dumps(metadata, indent=2) + '\n').encode('utf-8'),
            'Connect.cmd': command.encode('ascii')}


def prepare_host(root: Path, identity: HostIdentity, openssl: str) -> PreparedHost:
    identity = validate_identity(identity)
    root = Path(root).resolve(strict=True)
    host = root / 'room-host'
    client = root / 'host-connection'
    key, cert = host / 'server-key.pem', host / 'server-cert.pem'
    lock = root / '.room-host-preparation.lock'
    with exclusive_file_lock(lock):
        if host.exists() or host.is_symlink():
            _plain_path(host, directory=True)
            if not key.exists() or not cert.exists():
                raise ValueError('Host folder is incomplete; existing files were preserved')
        else:
            # This PC's public bundle and key belong together. An imported active
            # connection belongs to another host and does not own this PC's key.
            if client.exists() or client.is_symlink():
                raise ValueError('Own host connection exists without its host key; files were preserved')
            with tempfile.TemporaryDirectory(prefix='.host-stage-', dir=root) as temporary:
                staged = Path(temporary) / 'room-host'
                staged.mkdir()
                _private_directory(staged)
                config = Path(temporary) / 'openssl.cnf'
                config.write_text('[req]\ndistinguished_name=dn\nprompt=no\n[dn]\nCN=private-host\n',
                                  encoding='ascii')
                run_checked([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-sha256',
                             '-config', str(config), '-batch', '-nodes', '-days', '365',
                             '-keyout', str(staged / key.name),
                             '-out', str(staged / cert.name), '-subj', '/CN=' + host_name(identity),
                             '-addext', 'subjectAltName=' + (f'DNS:{identity.dns_name},' if identity.dns_name else '')
                             + f'IP:{identity.ipv4}',
                             '-addext', 'basicConstraints=critical,CA:TRUE',
                             '-addext', 'keyUsage=critical,digitalSignature,keyEncipherment,keyCertSign',
                             '-addext', 'extendedKeyUsage=serverAuth'])
                if os.name != 'nt':
                    (staged / key.name).chmod(0o600)
                _validate_certificate(staged / cert.name, staged / key.name, identity, openssl)
                staged.rename(host)
        _validate_key_permissions(host, key)
        _validate_certificate(cert, key, identity, openssl)
        files = _bundle_files(cert, identity)
        if client.exists() or client.is_symlink():
            _plain_path(client, directory=True)
            if {path.name for path in client.iterdir()} != set(files):
                raise ValueError('Connection folder differs from the host configuration; files were preserved')
            for name, data in files.items():
                path = client / name
                _plain_path(path)
                if path.stat().st_size != len(data) or path.read_bytes() != data:
                    raise ValueError('Connection bundle does not match the host; files were preserved')
        else:
            with tempfile.TemporaryDirectory(prefix='.connection-stage-', dir=root) as temporary:
                staged = Path(temporary) / 'connection'
                staged.mkdir()
                for name, data in files.items():
                    (staged / name).write_bytes(data)
                staged.rename(client)
        return PreparedHost(identity, key, cert, client)


def export_connection(prepared: PreparedHost, destination: Path) -> Path:
    """Export public trust data only; preserve a different existing export."""
    destination = Path(destination)
    all_files = _bundle_files(prepared.cert, validate_identity(prepared.identity))
    files = {name: all_files[name] for name in ('server-cert.pem', 'metadata.json')}
    if destination.exists() or destination.is_symlink():
        _plain_path(destination)
        try:
            if destination.stat().st_size > 131072:
                raise ValueError('Export is too large')
            with zipfile.ZipFile(destination) as archive:
                if (len(archive.infolist()) != len(files)
                        or set(archive.namelist()) != set(files)
                        or any(info.file_size != len(files[info.filename]) for info in archive.infolist())
                        or any(archive.read(name) != data for name, data in files.items())):
                    raise ValueError('Export differs from this host')
            return destination
        except (ValueError, OSError, zipfile.BadZipFile, RuntimeError) as exc:
            raise ValueError('Existing public export differs from this host; file was preserved') from exc
    contents = io.BytesIO()
    with zipfile.ZipFile(contents, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            archive.writestr(name, data)
    # Exclusive creation cannot overwrite a file that appeared after the check.
    with destination.open('xb') as output:
        output.write(contents.getvalue())
        output.flush()
        os.fsync(output.fileno())
    return destination


def gateway_command(root: Path, prepared: PreparedHost) -> list[str]:
    identity = validate_identity(prepared.identity)
    return [sys.executable, '-u', str(root / 'server' / 'room_gateway.py'),
            '--host', identity.ipv4, '--port', str(PORT), '--cert', str(prepared.cert),
            '--key', str(prepared.key), '--data-root', str(root / 'room-host' / 'rooms')]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare-only', action='store_true', help='Prepare files without starting the gateway')
    parser.add_argument('--openssl', help='Path to an existing OpenSSL executable')
    parser.add_argument('--host-ip', help="This PC's address on the virtual LAN (default: Radmin VPN, then Tailscale)")
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    try:
        identity = discover_identity(host_ip=args.host_ip)
        prepared = prepare_host(root, identity, args.openssl or find_openssl(root))
        public_archive = export_connection(prepared, root / 'Host-connection.zip')
        print(json.dumps({'event': 'prepared', 'server': f'{identity.ipv4}:{PORT}',
                          'connection_directory': str(prepared.client_dir),
                          'connection_archive': str(public_archive)}), flush=True)
        if not args.prepare_only:
            if str(root) not in sys.path:
                sys.path.insert(0, str(root))
            from connection_bundle import activate_connection, read_bundle_directory
            with activate_connection(root, read_bundle_directory(prepared.client_dir)):
                pass
            return subprocess.call(gateway_command(root, prepared), shell=False)
        return 0
    except (ValueError, OSError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 0


if __name__ == '__main__':
    raise SystemExit(main())
