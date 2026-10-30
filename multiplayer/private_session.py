"""Connection-file checks of the private multiplayer prototype's launcher (connection_bundle.py)."""
import hashlib
import json
from pathlib import Path
import re
import sys
import ssl
from server.player_identity import validate_nickname


def connection_defaults(root):
    folder = Path(root) / 'connection'
    if not folder.exists():
        return None, None
    metadata = folder / 'metadata.json'
    cert = folder / 'server-cert.pem'
    if metadata.stat().st_size > 4096 or cert.stat().st_size > 65536:
        raise ValueError('The connection files are damaged.')
    data = json.loads(metadata.read_text(encoding='utf-8'))
    if (not isinstance(data, dict) or data.get('version') != 1
            or data.get('certificate') != cert.name
            or not isinstance(data.get('server'), str)):
        raise ValueError('Invalid connection settings.')
    bridge_command(Path(root), data['server'], '', cert)
    der = ssl.PEM_cert_to_DER_cert(cert.read_text(encoding='ascii'))
    if hashlib.sha256(der).hexdigest() != data.get('certificate_sha256'):
        raise ValueError('The certificate does not match the connection settings.')
    server = data['server']
    if 'dns_name' in data:
        dns = data['dns_name']
        if (not isinstance(dns, str) or not 3 <= len(dns) <= 253 or '.' not in dns
                or any(not re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?', label)
                       for label in dns.split('.'))):
            raise ValueError('Invalid connection server DNS name.')
        port = server.rpartition(':')[2]
        if not re.fullmatch(r'[0-9]{1,5}', port) or not 1 <= int(port) <= 65535:
            raise ValueError('Invalid connection server port.')
        # Shared Tailscale devices can have remapped IPs in another account.
        server = f'{dns}:{port}'
    return server, cert


def bridge_command(root, server, code, ca_file, nickname=''):
    server = server.strip()
    if not server or len(server) > 512 or any(c.isspace() for c in server):
        raise ValueError('Give the server address as host:port.')
    code = code.strip().upper()
    args = [sys.executable, '-u', str(root / 'server' / 'room_bridge.py'), '--server', server]
    args += ['--join', code] if code else ['--create']
    if ca_file:
        args += ['--ca', str(ca_file)]
    if validate_nickname(nickname, allow_empty=True):
        args += ['--nickname=' + nickname]
    return args

