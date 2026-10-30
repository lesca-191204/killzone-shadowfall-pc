# SPDX-License-Identifier: GPL-2.0-or-later
"""Private multiplayer (beta), test on ONE PC: the private services on loopback and two copies of
the game (Player1 and Player2), each with its own saves, settings and logs.

  python local_test.py --port "<port folder>" --source-user "<user folder with launcher.json>"

The services are the KZSF private multiplayer prototype's (server/, GPL-2.0-or-later); the game
copies are started with the port's launcher (--play). Player1 reuses the source user's shader
cache (only it writes there); Player2 builds its own. Close both games, then this window, to end.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
SERVER = HERE / 'server'
PATCHES = 'Private Multiplayer - HUD Visibility;Private Multiplayer - Full Unlocks'
SERVICES = ('private_service.py', 'turbine_relay.py', 'ping_service.py')


# Two copies side by side: 1280x720 windows showing the game's own 1080p picture. No upscaler,
# quality mode or frame generation: those size the scene for the source's output (1080p and up),
# which drew the game in a corner of the smaller window and zoomed in.
NATIVE_OPTIONS = {'window_size': '1280x720', 'res_method': 'patch', 'upscale_quality': 'native',
                  'frame_gen': 'off', 'ultrawide': 'off'}
NATIVE_SETTINGS = {'display_mode': 'windowed', 'render_resolution': 'native', 'upscale_quality': 'native'}
DROPPED_SETTINGS = ('upscaler=', 'output_res=', 'window_size=')


def player_dir(root, source, number):
    """user-mp<number>: the source's launcher options and game settings, windowed and native."""
    user = root / f'user-mp{number}'
    user.mkdir(parents=True, exist_ok=True)
    options = json.loads((source / 'launcher.json').read_text(encoding='utf-8'))
    options.update(NATIVE_OPTIONS)
    (user / 'launcher.json').write_text(json.dumps(options, indent=2), encoding='utf-8')
    if (source / 'settings.ini').exists():
        lines = (source / 'settings.ini').read_text(encoding='utf-8').splitlines()
        lines = [line for line in lines if not line.startswith(DROPPED_SETTINGS) and
                 line.partition('=')[0] not in NATIVE_SETTINGS]
        lines += [f'{key}={value}' for key, value in NATIVE_SETTINGS.items()]
        (user / 'settings.ini').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return user


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', type=Path, required=True, help='folder with Killzone Shadow Fall.exe')
    parser.add_argument('--source-user', type=Path, required=True, help='user folder to copy the options from')
    parser.add_argument('--players', type=int, choices=(1, 2), default=2)
    args = parser.parse_args()
    port, source = args.port.resolve(), args.source_user.resolve()
    launcher = port / 'Killzone Shadow Fall.exe'
    if not launcher.is_file() or not (source / 'launcher.json').is_file():
        sys.exit(f'Not found: {launcher} or {source / "launcher.json"}')
    data = port.parent / 'server-data'
    data.mkdir(exist_ok=True)
    services = []
    for name in SERVICES:
        log = open(data / (name[:-3] + '.log'), 'a', encoding='utf-8')
        services.append(subprocess.Popen([sys.executable, '-u', '-B', str(SERVER / name)], cwd=str(SERVER),
                                         stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
    time.sleep(2)
    if any(p.poll() is not None for p in services):
        for p in services: p.kill()
        sys.exit(f'A private service did not start: see the logs in {data}')
    print('Private services ready on 127.0.0.1:17800 (profiles), 17801 (match relay), 17802 (ping)', flush=True)
    games = []
    try:
        for number in range(1, args.players + 1):
            user = player_dir(port.parent, source, number)
            env = dict(os.environ,
                       KZSF_USER_DIR=str(user),
                       KZSF_PROBE_LINK='1', KZSF_PROBE_UDP='1', KZSF_PROBE_NP='1',
                       KZSF_PRIVATE_HTTP='http://127.0.0.1:17800',
                       KZSF_PRIVATE_PLAYER=f'Player{number}',
                       KZSF_NET_TRACE='1',
                       GOW3_PATCHES=PATCHES)
            if number == 1 and (source / 'ps4').is_dir():
                env['GOW3_GPU_USER_DIR'] = str(source / 'ps4')
            games.append(subprocess.Popen([str(launcher), '--play'], cwd=str(port), env=env))
            print(f'Player{number} started (options and logs in {user})', flush=True)
            time.sleep(20)  # one at a time: preparing the game is heavy
        for game in games:
            game.wait()
    except KeyboardInterrupt:
        pass
    finally:
        for p in services:
            p.terminate()
        print('Private services stopped.', flush=True)


if __name__ == '__main__':
    main()
