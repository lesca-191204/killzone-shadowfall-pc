# SPDX-License-Identifier: GPL-2.0-or-later
"""Private multiplayer (beta) between PCs on a virtual LAN (Radmin VPN, Tailscale, ZeroTier...).

  Host:  python kz_online.py host --nickname Santi --port "<port folder>" --source-user "<user folder>"
         Starts the room server on this PC's virtual-LAN address (Radmin VPN 26.x by itself, else
         Tailscale, or --host-ip), writes Host-connection.zip (send it to the others: it has no
         secrets), creates a room, prints its CODE and starts the game.
  Join:  python kz_online.py join --connection Host-connection.zip --code CODE --nickname Friend ...
         Joins that room and starts the game.

In the game, one player creates a Warzone (Multiplayer > Create your Warzone) and the others
join it (Multiplayer > Browse Warzones). Close the game to leave; the room ends with the host.

The room server, bridge and profiles are the KZSF private multiplayer prototype's (server/,
GPL-2.0-or-later). The game runs with the port's launcher (--play) and its own user folder
(user-online beside the port folder): saves apart from the campaign, options and shader cache
from --source-user.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / 'server'))
import room_host  # noqa: E402
from connection_bundle import activate_connection, read_bundle_zip  # noqa: E402
from local_profile import LocalProfile  # noqa: E402
from player_identity import player_number, validate_nickname  # noqa: E402

PATCHES = 'Private Multiplayer - HUD Visibility;Private Multiplayer - Full Unlocks'


def wait_port(host, port, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            socket.create_connection((host, port), timeout=1).close()
            return True
        except OSError:
            time.sleep(0.5)
    return False


def user_dir(port, source):
    """user-online beside the port: the source's launcher options and settings, its own saves."""
    user = port.parent / 'user-online'
    user.mkdir(exist_ok=True)
    (user / 'launcher.json').write_bytes((source / 'launcher.json').read_bytes())
    if not (user / 'settings.ini').exists() and (source / 'settings.ini').exists():
        (user / 'settings.ini').write_bytes((source / 'settings.ini').read_bytes())
    return user


def open_room(server, ca, nickname, code=None):
    """The room bridge (local ports 17800-17802 to the host's TLS gateway): (process, code, slot, name)."""
    command = [sys.executable, '-u', '-B', str(HERE / 'server' / 'room_bridge.py'), '--server', server,
               '--ca', str(ca), '--nickname=' + nickname]
    command += ['--join', code.strip().upper()] if code else ['--create']
    bridge = subprocess.Popen(command, cwd=str(HERE), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              text=True, encoding='utf-8')
    line = bridge.stdout.readline(8193)
    try:
        message = json.loads(line)
    except ValueError:
        message = {}
    if message.get('event') != 'ready':
        bridge.kill()
        raise SystemExit('Could not open the room: ' + str(message.get('message') or line.strip() or
                         'check Radmin VPN, the host, the code and the firewall (TCP 17803)'))
    return bridge, message['code'], player_number(message.get('player')), message.get('nickname') or nickname


def play(port, source, slot, nickname):
    LocalProfile.reset_boosts(HERE / 'local-profile')
    user = user_dir(port, source)
    env = dict(os.environ,
               KZSF_USER_DIR=str(user),
               KZSF_PROBE_LINK='1', KZSF_PROBE_UDP='1', KZSF_PROBE_NP='1',
               KZSF_PRIVATE_HTTP='http://127.0.0.1:17800',
               # The native alias picks local saves (Player1 = slot 1, Player2 = any other); the
               # nickname is the player's identity in the room.
               KZSF_PRIVATE_PLAYER='Player1' if slot == 1 else 'Player2',
               KZSF_PRIVATE_NICKNAME=nickname,
               KZSF_NET_TRACE='0',
               GOW3_PATCHES=PATCHES)
    if (source / 'ps4').is_dir():
        env['GOW3_GPU_USER_DIR'] = str(source / 'ps4')
    print(f'Starting the game as {nickname} (logs in {user / "logs"})', flush=True)
    return subprocess.call([str(port / 'Killzone Shadow Fall.exe'), '--play'], cwd=str(port), env=env)


def stop(process):
    if process and process.poll() is None:
        try:
            process.stdin.write('stop\n')
            process.stdin.flush()
            process.wait(timeout=5)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            process.terminate()


def host(args, port, source):
    identity = room_host.discover_identity(host_ip=args.host_ip)
    prepared = room_host.prepare_host(HERE, identity, room_host.find_openssl(HERE))
    archive = HERE / 'Host-connection.zip'
    room_host.export_connection(prepared, archive)
    print(f'Room server on {identity.ipv4}:{room_host.PORT}', flush=True)
    print(f'Send this file to the other players: {archive}', flush=True)
    gateway = subprocess.Popen(room_host.gateway_command(HERE, prepared), cwd=str(HERE))
    bridge = None
    try:
        if not wait_port(identity.ipv4, room_host.PORT):
            raise SystemExit('The room server did not start (is TCP 17803 free?)')
        bridge, code, slot, nickname = open_room(f'{room_host.host_name(identity)}:{room_host.PORT}',
                                                 prepared.cert, args.nickname)
        print(f'\n    ROOM CODE: {code}\n', flush=True)
        input('Send the code, then press Enter to start the game... ')
        play(port, source, slot, nickname)
        input('Game closed. Press Enter to close the room (the others leave too)... ')
    finally:
        stop(bridge)
        gateway.terminate()


def join(args, port, source):
    with activate_connection(HERE, read_bundle_zip(args.connection)) as (server, ca):
        bridge, code, slot, nickname = open_room(server, ca, args.nickname, args.code)
    try:
        print(f'Joined room {code} as {nickname}', flush=True)
        play(port, source, slot, nickname)
    finally:
        stop(bridge)


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
    validate_nickname(args.nickname)
    port, source = args.port.resolve(), args.source_user.resolve()
    if not (port / 'Killzone Shadow Fall.exe').is_file() or not (source / 'launcher.json').is_file():
        sys.exit(f'Not found: {port / "Killzone Shadow Fall.exe"} or {source / "launcher.json"}')
    try:
        if args.mode == 'host':
            host(args, port, source)
        else:
            if not args.connection or not args.code:
                sys.exit('join needs --connection and --code')
            join(args, port, source)
    except ValueError as error:
        sys.exit(f'Error: {error}')
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
