"""Real loopback peer: split headers/bodies, coalesced frames, bad length, EOF."""
from pathlib import Path
import socket
import struct
import subprocess
import sys
import threading
import time

HEADER = struct.Struct('>4sBBHII')

def exact(peer, size):
    result = bytearray()
    while len(result) < size:
        chunk = peer.recv(size - len(result))
        if not chunk:
            raise EOFError('peer closed')
        result.extend(chunk)
    return bytes(result)

def packet(kind, channel, data=b'', flags=0):
    return HEADER.pack(b'KZRP', 1, kind, channel, len(data), flags) + data

def serve(server, errors):
    session = None
    try:
        for index in range(5):
            peer, address = server.accept()
            with peer:
                peer.settimeout(6)
                magic, version, kind, channel, size, flags = HEADER.unpack(exact(peer, 16))
                assert (magic, version, kind, size, flags) == (b'KZRP', 1, 1, 12, 0)
                nonce, local_port, reserved = struct.unpack('>QHH', exact(peer, size))
                assert nonce and local_port == 100 + index and reserved == 0
                assert session is None or session == nonce
                session = nonce
                ack = packet(2, channel)
                if index == 4:
                    peer.sendall(ack + packet(3, channel, b'early', 0x40))
                    while peer.recv(4096):
                        pass
                    continue
                for offset in range(0, len(ack), 3):
                    peer.sendall(ack[offset:offset+3])
                    time.sleep(.001)
                if index == 0:
                    header = HEADER.unpack(exact(peer, 16))
                    assert header == (b'KZRP', 1, 3, channel, 4, 0x40)
                    assert exact(peer, 4) == b'ping'
                    response = packet(3, channel, b'pong', 0x40) + packet(3, channel, b'two', 0x48)
                    for offset in range(0, len(response), 5):
                        peer.sendall(response[offset:offset+5])
                        time.sleep(.001)
                    while peer.recv(4096):
                        pass
                elif index == 1:
                    time.sleep(.05)
                    peer.sendall(HEADER.pack(b'KZRP', 1, 3, channel, 65536, 0x40))
                    while peer.recv(4096):
                        pass
                elif index == 2:
                    time.sleep(.05)
                else:
                    while peer.recv(4096):
                        pass
    except Exception as error:
        print('RUDP test peer error:', repr(error), file=sys.stderr, flush=True)
        errors.append(error)

if __name__ == '__main__':
    errors = []
    with socket.socket() as server:
        server.bind(('127.0.0.1', int(sys.argv[2]) if len(sys.argv) > 2 else 17801))
        server.listen(4)
        server.settimeout(12)
        thread = threading.Thread(target=serve, args=(server, errors), daemon=True)
        thread.start()
        result = subprocess.run([str(Path(sys.argv[1]).resolve())], timeout=20)
        if result.returncode == 0:
            thread.join(8)
            assert not thread.is_alive(), 'server did not complete'
            if errors:
                raise errors[0]
    raise SystemExit(result.returncode)
