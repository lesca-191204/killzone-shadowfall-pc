# SPDX-License-Identifier: GPL-2.0-or-later
"""Update check and install for the Killzone Shadow Fall launcher.

The newest GitHub release (pre-releases included: the betas are published as such) is compared
with the launcher's VERSION. Installing downloads the release zip into user/update/, unpacks it,
and hands over to a small .cmd script that waits for the launcher to exit, copies the new files
over the port folder (user/ is never touched: saves, settings, caches and logs stay), and starts
the launcher again.
"""
import json
import re
import shutil
import subprocess
import urllib.request
import zipfile
from pathlib import Path

REPO = 'lesca-191204/killzone-shadowfall-pc'
RELEASES_API = f'https://api.github.com/repos/{REPO}/releases?per_page=10'
RELEASES_PAGE = f'https://github.com/{REPO}/releases'
ASSET = re.compile(r'^KillzoneShadowFall-v[\d.]+-windows\.zip$')
USER_AGENT = 'KillzoneShadowFall-launcher'
SYSTEM32 = r'%SystemRoot%\System32'


def parse_version(text):
    """(major, minor, patch) from 'v0.2.0' / '0.2.0', None when it is not a version."""
    m = re.match(r'^v?(\d+)\.(\d+)(?:\.(\d+))?$', str(text).strip())
    return (int(m.group(1)), int(m.group(2)), int(m.group(3) or 0)) if m else None


def newest_release(releases):
    """The highest-versioned published release that has a Windows zip, or None."""
    best = None
    for release in releases:
        version = parse_version(release.get('tag_name', ''))
        if release.get('draft') or version is None:
            continue
        asset = next((a for a in release.get('assets', []) if ASSET.match(a.get('name', ''))), None)
        if asset is None:
            continue
        if best is None or version > best['version_tuple']:
            best = {'version_tuple': version, 'version': '.'.join(map(str, version)),
                    'page': release.get('html_url') or RELEASES_PAGE, 'notes': release.get('body') or '',
                    'url': asset['browser_download_url'], 'size': int(asset.get('size') or 0),
                    'name': asset['name']}
    return best


def check(current, timeout=10):
    """The release newer than `current`, or None. Network errors raise OSError/ValueError."""
    request = urllib.request.Request(RELEASES_API, headers={'User-Agent': USER_AGENT,
                                                            'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        releases = json.load(response)
    best = newest_release(releases if isinstance(releases, list) else [])
    mine = parse_version(current) or (0, 0, 0)
    return best if best and best['version_tuple'] > mine else None


def download(release, folder, progress=None, timeout=30):
    """Downloads the release zip into `folder`; progress(done_bytes, total_bytes)."""
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / release['name']
    partial = target.with_suffix('.part')
    request = urllib.request.Request(release['url'], headers={'User-Agent': USER_AGENT})
    with urllib.request.urlopen(request, timeout=timeout) as response, open(partial, 'wb') as out:
        total = int(response.headers.get('Content-Length') or release.get('size') or 0)
        done = 0
        while chunk := response.read(1 << 20):
            out.write(chunk)
            done += len(chunk)
            if progress:
                progress(done, total)
    if release.get('size') and partial.stat().st_size != release['size']:
        partial.unlink(missing_ok=True)
        raise OSError('incomplete download')
    partial.replace(target)
    return target


def extract(archive, folder, launcher_name):
    """Unpacks the zip into `folder` and returns the directory that holds the launcher exe."""
    folder = Path(folder)
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    with zipfile.ZipFile(archive) as z:
        for member in z.namelist():  # no absolute paths or '..' may leave the folder
            path = (folder / member).resolve()
            if folder.resolve() not in path.parents and path != folder.resolve():
                raise ValueError(f'unsafe path in the update: {member}')
        z.extractall(folder)
    found = [p.parent for p in folder.rglob(launcher_name)]
    if len(found) != 1:
        raise ValueError('the update zip does not hold the launcher')
    if any(part.lower() == 'user' for part in found[0].relative_to(folder).parts):
        raise ValueError('unexpected update layout')
    return found[0]


def install_script(source, port_dir, launcher_pid, launcher_exe, work_dir, cleanup=()):
    """Writes the .cmd that applies the update once the launcher (PID) has exited; `cleanup`
    (the unpacked folder, the zip) is deleted afterwards."""
    script = Path(work_dir) / 'apply_update.cmd'
    log = Path(work_dir) / 'apply_update.log'
    failed = Path(work_dir) / 'apply_update_failed.txt'
    lines = [
        '@echo off',
        'chcp 65001 >nul',
        'setlocal',
        ':wait',
        # System32 tools by full path: a PATH with Git's usr/bin would pick another find.
        f'"{SYSTEM32}\\tasklist.exe" /NH /FI "PID eq {launcher_pid}" 2>nul | '
        f'"{SYSTEM32}\\findstr.exe" /C:" {launcher_pid} " >nul && '
        f'("{SYSTEM32}\\ping.exe" -n 2 127.0.0.1 >nul & goto wait)',  # timeout needs a console
        f'del "{failed}" 2>nul',
        # Copies everything except a user folder; nothing in the port folder is deleted.
        f'"{SYSTEM32}\\robocopy.exe" "{source}" "{port_dir}" /E /XD user /R:20 /W:1 /NP /NFL /NDL > "{log}"',
        f'if %ERRORLEVEL% GEQ 8 echo Update failed, see {log} > "{failed}"',
        f'cd /d "{port_dir}"',  # the restarted launcher must not hold user/update open
        f'start "" "{Path(port_dir) / launcher_exe}"',
    ]
    for path in cleanup:
        lines.append(f'rmdir /s /q "{path}" 2>nul' if Path(path).is_dir() else f'del "{path}" 2>nul')
    lines.append('endlocal')
    script.write_text('\r\n'.join(lines) + '\r\n', encoding='utf-8')
    return script


def failed_update(work_dir):
    """True when the last update script could not copy the files."""
    return (Path(work_dir) / 'apply_update_failed.txt').is_file()


def run_script(script):
    """Starts the update script on its own, without a console window. It leaves the launcher's job
    when allowed: a job that kills its processes when the launcher closes (some shortcuts and game
    launchers start it in one) took the script along before it copied anything (#87)."""
    flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0) | getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
    breakaway = getattr(subprocess, 'CREATE_BREAKAWAY_FROM_JOB', 0x01000000)
    for extra in (breakaway, 0):
        try:
            return subprocess.Popen(['cmd.exe', '/c', str(script)], cwd=str(Path(script).parent),
                                    creationflags=flags | extra, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)
        except OSError:
            if not extra:
                raise


def exit_for_update(delay=1.5):
    """Ends the launcher shortly after its window closed, whatever threads remain."""
    import os
    import threading
    threading.Timer(delay, lambda: os._exit(0)).start()
