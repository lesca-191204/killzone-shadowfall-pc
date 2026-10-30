"""Mount installed free-map resources together with the updated base cache."""
from contextlib import contextmanager
import os
from pathlib import Path
import re
import shutil
import tempfile

from mods import LINKED, expand, make_link


def _reject_link(path):
    if path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction()):
        raise ValueError(f'DLC content links are unsupported: {path}')


def _files(game):
    files = []
    for addon in sorted(game.iterdir()):
        if not re.fullmatch(r'FREECONTENT[0-9]{5}', addon.name):
            continue
        _reject_link(addon)
        cache = addon / 'localcachepink'
        _reject_link(cache)
        if not cache.is_dir():
            continue
        for directory, folders, names in os.walk(cache, followlinks=False):
            directory = Path(directory)
            for name in folders + names:
                _reject_link(directory / name)
            for name in sorted(names):
                source = directory / name
                if not source.is_file():
                    raise ValueError(f'Not a regular DLC file: {source}')
                files.append((source.relative_to(cache), source))
    return files


@contextmanager
def cache_mounts(game, directory):
    game = Path(game).resolve(strict=True)
    directory = Path(directory).resolve()
    if directory == game or directory.is_relative_to(game):
        raise ValueError('The temporary DLC cache must be outside Game.')
    additions = _files(game)
    if not additions:
        yield []
        return
    directory.mkdir(parents=True, exist_ok=True)
    # Keep hard links on the game volume; a cross-volume fallback would copy gigabytes.
    if game.stat().st_dev != directory.stat().st_dev:
        raise ValueError('Game and the private launcher must be on the same volume for DLC.')
    cache = game / 'localcachepink'
    _reject_link(cache)
    # A short path also keeps the game's long asset names within Windows path limits.
    view = Path(tempfile.mkdtemp(prefix='dlc-', dir=directory))
    try:
        if cache.is_dir():
            for entry in cache.iterdir():
                make_link(view / entry.name, entry)
        added = 0
        for relative, source in additions:
            parent = view
            for part in relative.parts[:-1]:
                parent = parent / part
                expand(parent)
            target = parent / relative.name
            if target.exists():
                if not target.is_file():
                    raise ValueError(f'DLC file conflicts with a directory: {relative}')
                # The base update wins; its .core.patch files remain beside DLC .core files.
                continue
            make_link(target, source)
            added += 1
        print(f'DLC cache: {added} installed files added; base files preserved.', flush=True)
        yield ['--mount', f'/app0/localcachepink={view}',
               '--mount', f'/hostapp/localcachepink={view}']
    finally:
        # view is our new directory outside Game; rmtree does not traverse directory links.
        shutil.rmtree(view)
        for name in list(LINKED):
            if Path(name).is_relative_to(view):
                LINKED.pop(name)
