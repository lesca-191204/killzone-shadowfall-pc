# SPDX-License-Identifier: GPL-2.0-or-later
"""Private multiplayer (beta) between PCs on a virtual LAN (Radmin VPN, Tailscale, ZeroTier...).

The launcher's Multiplayer page uses Online below. From a console:

  Host:  python kz_online.py host --nickname Santi --port "<port folder>" --source-user "<user folder>"
         Starts the room server on this PC's virtual-LAN address (Radmin VPN 26.x by itself, else
         Tailscale, or --host-ip), writes Host-connection.zip (send it to the others: it has no
         secrets), creates a room, prints its CODE and starts the game.
  Join:  python kz_online.py join --connection Host-connection.zip --code CODE --nickname Friend ...

In the game, one player creates a Warzone (Multiplayer > Create your Warzone) and the others
join it (Multiplayer > Browse Warzones). The room ends with the host.

The room server, bridge and profiles are the KZSF private multiplayer prototype's (server/,
GPL-2.0-or-later). Keys, rooms and profiles live in the data folder (the launcher's user/online).
"""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import threading
import time

HERE = Path(__file__).resolve().parent
for folder in (HERE, HERE / 'server'):
    if str(folder) not in sys.path:
        sys.path.insert(0, str(folder))
import room_host  # noqa: E402
from connection_bundle import activate_connection, read_bundle_zip  # noqa: E402
from local_profile import LocalProfile  # noqa: E402
from player_identity import player_number, validate_nickname  # noqa: E402

PATCHES = 'Private Multiplayer - HUD Visibility;Private Multiplayer - Full Unlocks'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def network():
    """This PC's virtual LAN: {'kind': 'radmin' | 'tailscale' | 'lan' | None, 'ip': str}."""
    addresses = room_host.local_ipv4()
    for kind, net in (('radmin', room_host.RADMIN_NETWORK), ('tailscale', room_host.TAILSCALE_NETWORK)):
        found = [ip for ip in addresses if ipaddress.IPv4Address(ip) in net]
        if found:
            return {'kind': kind, 'ip': found[0]}
    private = [ip for ip in addresses if any(ipaddress.IPv4Address(ip) in n for n in room_host.VPN_NETWORKS)]
    return {'kind': 'lan' if private else None, 'ip': private[0] if private else ''}


def wait_port(host, port, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            socket.create_connection((host, port), timeout=1).close()
            return True
        except OSError:
            time.sleep(0.5)
    return False


class Online:
    """One room at a time: hosting (gateway + own bridge) or joined (bridge). Not thread-safe:
    one caller (the launcher's page or the console) drives it."""

    def __init__(self, data, test_ports=False):
        self.data = Path(data)
        self.test_ports = test_ports  # tests on a PC already playing: the bridge on free local ports
        self.data.mkdir(parents=True, exist_ok=True)
        self.gateway = self.bridge = None
        self.room = None  # {'code', 'slot', 'nickname', 'host': bool, 'server', 'archive'}

    def _environment(self):
        env = dict(os.environ, KZSF_ONLINE_DIR=str(self.data), PYTHONIOENCODING='utf-8')
        for name in ('PYTHONHOME', 'PYTHONPATH'):
            env.pop(name, None)
        return env

    def _open_room(self, server, ca, nickname, code=None):
        command = [*room_host.python_command(HERE / 'server' / 'room_bridge.py'), '--server', server,
                   '--ca', str(ca), '--nickname=' + nickname]
        command += ['--join', code.strip().upper()] if code else ['--create']
        command += ['--test-ephemeral-ports'] if self.test_ports else []
        bridge = subprocess.Popen(command, cwd=str(HERE), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, encoding='utf-8',
                                  env=self._environment(), creationflags=NO_WINDOW)
        line = bridge.stdout.readline(8193)
        try:
            message = json.loads(line)
        except ValueError:
            message = {}
        if not isinstance(message, dict) or message.get('event') != 'ready':
            bridge.kill()
            raise ValueError(str(message.get('message') if isinstance(message, dict) else '') or
                             'Could not open the room: check the virtual LAN (Radmin VPN), the host, '
                             'the code and the firewall (TCP 17803)')
        self.bridge = bridge
        threading.Thread(target=self._drain, args=(bridge,), daemon=True).start()
        return message['code'], player_number(message.get('player')), message.get('nickname') or nickname

    @staticmethod
    def _drain(bridge):
        for _ in bridge.stdout:  # an error line, then the end of the bridge
            pass

    def host(self, nickname, host_ip=None):
        validate_nickname(nickname)
        self.close()
        identity = room_host.discover_identity(host_ip=host_ip)
        prepared = room_host.prepare_host(self.data, identity, room_host.find_openssl(HERE))
        archive = self.data / 'Host-connection.zip'
        room_host.export_connection(prepared, archive)
        self.gateway = subprocess.Popen(room_host.gateway_command(HERE, prepared), cwd=str(HERE),
                                        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL, env=self._environment(),
                                        creationflags=NO_WINDOW)
        try:
            if not wait_port(identity.ipv4, room_host.PORT):
                raise ValueError('The room server did not start (is TCP 17803 in use?)')
            server = f'{room_host.host_name(identity)}:{room_host.PORT}'
            code, slot, nickname = self._open_room(server, prepared.cert, nickname)
        except BaseException:
            self.close()
            raise
        self.room = {'code': code, 'slot': slot, 'nickname': nickname, 'host': True,
                     'server': server, 'archive': str(archive)}
        return self.room

    def join(self, connection, code, nickname):
        validate_nickname(nickname)
        if not code or not code.strip():
            raise ValueError('Enter the room code')
        self.close()
        with activate_connection(self.data, read_bundle_zip(connection)) as (server, ca):
            code, slot, nickname = self._open_room(server, ca, nickname, code)
        self.room = {'code': code, 'slot': slot, 'nickname': nickname, 'host': False, 'server': server}
        return self.room

    def alive(self):
        return bool(self.room and self.bridge and self.bridge.poll() is None and
                    (not self.room['host'] or (self.gateway and self.gateway.poll() is None)))

    def game_env(self, gpu_user_dir=None):
        """What the game needs to play in this room (on top of the launcher's environment)."""
        if not self.alive():
            raise ValueError('The room is closed')
        LocalProfile.reset_boosts(self.data / 'local-profile')
        env = {'KZSF_PROBE_LINK': '1', 'KZSF_PROBE_UDP': '1', 'KZSF_PROBE_NP': '1',
               'KZSF_PRIVATE_HTTP': 'http://127.0.0.1:17800',
               # The native alias picks local saves (Player1 = slot 1, Player2 = any other); the
               # nickname is the player's identity in the room.
               'KZSF_PRIVATE_PLAYER': 'Player1' if self.room['slot'] == 1 else 'Player2',
               'KZSF_PRIVATE_NICKNAME': self.room['nickname'], 'KZSF_NET_TRACE': '0',
               'GOW3_PATCHES': PATCHES}
        if gpu_user_dir:
            env['GOW3_GPU_USER_DIR'] = str(gpu_user_dir)
        return env

    def close(self):
        bridge, gateway, self.bridge, self.gateway, self.room = self.bridge, self.gateway, None, None, None
        if bridge and bridge.poll() is None:
            try:
                bridge.stdin.write('stop\n')
                bridge.stdin.flush()
                bridge.wait(timeout=5)
            except (OSError, ValueError, subprocess.TimeoutExpired):
                bridge.terminate()
        if gateway and gateway.poll() is None:
            gateway.terminate()


def online_user(port, source):
    """user-online beside the port (console use): the source's options and settings, its own saves."""
    user = port.parent / 'user-online'
    user.mkdir(exist_ok=True)
    shutil.copy2(source / 'launcher.json', user / 'launcher.json')
    if not (user / 'settings.ini').exists() and (source / 'settings.ini').exists():
        shutil.copy2(source / 'settings.ini', user / 'settings.ini')
    return user


def play(online, port, source):
    user = online_user(port, source)
    env = dict(os.environ, KZSF_USER_DIR=str(user), **online.game_env(source / 'ps4' if (source / 'ps4').is_dir() else None))
    print(f'Starting the game as {online.room["nickname"]} (logs in {user / "logs"})', flush=True)
    return subprocess.call([str(port / 'Killzone Shadow Fall.exe'), '--play'], cwd=str(port), env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=('host', 'join'))
    parser.add_argument('--nickname', required=True, help='3-16 letters, digits, _ or -')
    parser.add_argument('--port', type=Path, required=True, help='folder with Killzone Shadow Fall.exe')
    parser.add_argument('--source-user', type=Path, required=True, help='user folder to copy the options from')
    parser.add_argument('--host-ip', help="host: this PC's virtual-LAN address (default: Radmin VPN, then Tailscale)")
    parser.add_argument('--connection', type=Path, help="join: the host's Host-connection.zip")
    parser.add_argument('--code', help='join: the room code')
    args = parser.parse_args()
    port, source = args.port.resolve(), args.source_user.resolve()
    if not (port / 'Killzone Shadow Fall.exe').is_file() or not (source / 'launcher.json').is_file():
        sys.exit(f'Not found: {port / "Killzone Shadow Fall.exe"} or {source / "launcher.json"}')
    online = Online(HERE)
    try:
        if args.mode == 'host':
            room = online.host(args.nickname, args.host_ip)
            print(f'Room server on {room["server"]}', flush=True)
            print(f'Send this file to the other players: {room["archive"]}', flush=True)
            print(f'\n    ROOM CODE: {room["code"]}\n', flush=True)
            input('Send the code, then press Enter to start the game... ')
            play(online, port, source)
            input('Game closed. Press Enter to close the room (the others leave too)... ')
        else:
            if not args.connection or not args.code:
                sys.exit('join needs --connection and --code')
            room = online.join(args.connection, args.code, args.nickname)
            print(f'Joined room {room["code"]} as {room["nickname"]}', flush=True)
            play(online, port, source)
    except ValueError as error:
        sys.exit(f'Error: {error}')
    except KeyboardInterrupt:
        pass
    finally:
        online.close()


if __name__ == '__main__':
    main()
