"""Link the eboot with the game's bundled system modules into one probe image.

Successor of link_libc.py for several modules (default: libc.prx, then
libSceFios2.prx). Every module is placed after the previous one; imports of
all modules share one import table, so the C runtime can override any
function (host contracts first) and otherwise binds the native export.

Exports are matched by (NID, library identity, module identity). Imports of
libSceLibcInternal are served by libc.prx's same-NID exports: the internal
library is not shipped with the game and exposes the same functions.

Dynamic modules (from the game root, e.g. Killzone's all_programs.prx) are
placed and relocated the same way but not started at boot: the game loads them
with sceKernelLoadStartModule and finds their exports with sceKernelDlsym, so
their export table (NID -> address) travels with the image.
Output: out/boot-linked.bin (format G3PROBE6) and out/link.json.
"""
import collections
import hashlib
import json
from pathlib import Path
import struct
from prepare import parse_self, read_self, span, unpack
from link_libc import encode_id

DEFAULT_MODULES = ('libc.prx', 'libSceFios2.prx')
FS_LOAD = bytes.fromhex('64488b042500000000')  # mov rax, fs:[0]


def canonical(identity, fallback=None):
    """NID#library for an import identity (NID, (library, version), module)."""
    return f'{identity[0]}#{identity[1][0]}' if identity else fallback


def module(path):
    source = read_self(path)
    elf, header, ph, _, missing = parse_self(source, Path(path).name)
    dp = next(p for p in ph if p['type'] == 2)
    dynamic = []
    for pos in range(dp['offset'], dp['offset'] + dp['filesz'], 16):
        tag, value = unpack('<QQ', elf, pos)
        if not tag:
            break
        dynamic.append((tag, value))
    tags = dict(dynamic)
    lp = next(p for p in ph if p['type'] == 0x61000000)
    blob = span(elf, lp['offset'], lp['filesz'])
    strings = span(blob, tags[0x61000035], tags[0x61000037])

    def string(offset):
        span(strings, offset, 1)
        return strings[offset:strings.index(0, offset)].decode('ascii')
    libraries, modules = {}, {}
    for tag, value in dynamic:
        if tag in (0x61000013, 0x61000015, 0x6100000d, 0x6100000f):
            table = libraries if tag in (0x61000013, 0x61000015) else modules
            key = encode_id(value >> 48)
            identity = (string(value & 0xffffffff), (value >> 32) & 65535)
            if key in table and table[key] != identity:
                raise ValueError('conflicting module/library IDs')
            table[key] = identity

    def identity(name):
        parts = name.split('#')
        if len(parts) != 3:
            return None  # plain names such as module_start are not linkable
        n, lib, mod = parts
        return (n, libraries[lib], modules[mod])
    symbols = []
    table = span(blob, tags[0x61000039], tags[0x6100003f])
    for pos in range(0, len(table), 24):
        n, info, other, section, value, size = unpack('<IBBHQQ', table, pos)
        name = string(n)
        symbols.append(dict(name=name, type=info & 15, binding=info >> 4, section=section,
                            value=value, size=size, identity=identity(name) if name else None))
    relocations = []
    for ot, st in ((0x61000029, 0x6100002d), (0x6100002f, 0x61000031)):
        table = span(blob, tags[ot], tags[st])
        for pos in range(0, len(table), 24):
            target, info, addend = unpack('<QQq', table, pos)
            relocations.append((target, info & 0xffffffff, info >> 32, addend))
    return dict(elf=elf, header=header, ph=ph, tags=tags, symbols=symbols, relocs=relocations,
                identity=identity, libraries=libraries, modules=modules,
                sha256=hashlib.sha256(source).hexdigest(), missing=missing)


def patch_fs_loads(image, ph, base):
    """Rewrite initial-exec `mov rax, fs:[0]` to GS: glibc owns FS on Linux."""
    patched = 0
    for p in ph:
        if p['type'] != 1 or not p['flags'] & 1:
            continue
        start, end = base + p['vaddr'], base + p['vaddr'] + p['filesz']
        at = image.find(FS_LOAD, start, end)
        while at >= 0:
            image[at] = 0x65
            patched += 1
            at = image.find(FS_LOAD, at + len(FS_LOAD), end)
    return patched


def link(game, out, module_names=DEFAULT_MODULES, dynamic_names=()):
    main = module(game / 'eboot.bin')
    raw = (out / 'boot.bin').read_bytes()
    magic, size, entry, ns, nr, ni, flags = unpack('<8s6Q', raw, 0)
    if magic != b'G3PROBE2':
        raise ValueError('linker expects freshly prepared G3PROBE2')
    pos = 56
    segments = [unpack('<3Q', raw, pos + i * 24) for i in range(ns)]
    pos += ns * 24
    names = [span(raw, pos + i * 128, 128).split(b'\0', 1)[0].decode() for i in range(ni)]
    pos += ni * 128
    relocs = [unpack('<QQqq', raw, pos + i * 32) for i in range(nr)]
    pos += nr * 32
    # prepare.py kinds 3/4: the eboot's DTPMOD64/DTPOFF64 against another module's TLS variable.
    main_tls_relocs = [r for r in relocs if r[1] in (3, 4)]
    relocs = [r for r in relocs if r[1] not in (3, 4)]
    image = bytearray(span(raw, pos, size))
    if pos + size != len(raw):
        raise ValueError('unexpected boot trailer')

    identities = {main['identity'](name): i for i, name in enumerate(names)}
    # Host tables key imports as NID#library: the eboot's own letters (#c#C) differ per game.
    names = [canonical(main['identity'](name), name) for name in names]

    def imported(symbol):
        key = symbol['identity']
        if key not in identities:
            identities[key] = len(names)
            names.append(canonical(key))
        return identities[key]

    exports, by_nid, tls_exports = {}, collections.defaultdict(list), {}
    table = []
    base = (size + 65535) & ~65535
    fs_patched = patch_fs_loads(image, main['ph'], 0)
    tls_module = 2
    dynamic = []
    for filename in list(module_names) + list(dynamic_names):
        is_dynamic = filename in dynamic_names
        m = module(game / filename if is_dynamic else game / 'sce_module' / filename)
        if is_dynamic:
            # kzsf (#72): some PKG extractors leave system modules (dlcldr.prx) in the game root.
            # Ones with TLS or initializers are not the game's own (all_programs.prx): skipped.
            tls_header = next((p for p in m['ph'] if p['type'] == 7), None)
            init_array = m['tags'].get(27, 0) or m['tags'].get(33, 0)
            if (tls_header and tls_header['memsz']) or m['tags'].get(12, 0) or init_array:
                print(f'{filename}: dynamic module with TLS or initializers, skipped', flush=True)
                continue
        loads = [p for p in m['ph'] if p['type'] in (1, 0x61000010)]
        modsize = max(p['vaddr'] + p['memsz'] for p in loads)
        if base + modsize > 512 * 1024 * 1024:
            raise ValueError('linked image exceeds probe limit')
        image.extend(bytes(base + modsize - len(image)))

        def mapped(address, length=8, loads=loads):
            return any(p['vaddr'] <= address and address + length <= p['vaddr'] + p['memsz'] for p in loads)
        for p in loads:
            if p['filesz'] > p['memsz']:
                raise ValueError(f'{filename}: invalid segment size')
            image[base + p['vaddr']:base + p['vaddr'] + p['filesz']] = span(m['elf'], p['offset'], p['filesz'])
            segments.append((base + p['vaddr'], p['memsz'], p['flags']))
        tls = next((p for p in m['ph'] if p['type'] == 7), None)
        module_id = 0
        if is_dynamic:
            # Started by the game itself: no TLS block and no initializer to run at boot.
            init_array = m['tags'].get(27, 0) or m['tags'].get(33, 0)
            if (tls and tls['memsz']) or m['tags'].get(12, 0) or init_array:
                raise ValueError(f'{filename}: dynamic module with TLS or initializers is not supported')
            tls = None
        if tls:
            if tls['filesz'] > tls['memsz'] or not mapped(tls['vaddr'], tls['memsz']):
                raise ValueError(f'{filename}: unsupported TLS layout')
            module_id = tls_module
            tls_module += 1
        tls_relocations = 0
        for target, kind, symid, addend in m['relocs']:
            if not mapped(target):
                raise ValueError(f'{filename}: relocation outside image {target:#x}')
            target += base
            if kind == 8:
                relocs.append((target, 0, base + addend, 0))
            elif kind == 16:
                symbol = m['symbols'][symid]
                if symbol['name'] or symbol['type'] != 3 or addend or not module_id:
                    raise ValueError(f'{filename}: unsupported external TLS relocation')
                struct.pack_into('<Q', image, target, module_id)
                tls_relocations += 1
            elif kind in (1, 6, 7):
                symbol = m['symbols'][symid]
                if symbol['section']:
                    if symbol['section'] == 0xfff1:
                        struct.pack_into('<Q', image, target, symbol['value'] + addend)
                    else:
                        relocs.append((target, 0, base + symbol['value'] + addend, 0))
                elif not symbol['identity'] and symbol['binding'] == 2:
                    struct.pack_into('<Q', image, target, 0)  # weak undefined (module_start/stop)
                else:
                    rk = 2 if symbol['type'] == 1 else 1
                    if (rk == 1 and addend) or not 0 <= addend < 4096:
                        raise ValueError(f'{filename}: unsupported import addend')
                    relocs.append((target, rk, imported(symbol), addend))
            else:
                raise ValueError(f'{filename}: unsupported relocation type {kind}')
        if is_dynamic:
            symbols = []
            for s in m['symbols']:
                if s['section'] and s['binding'] in (1, 2) and s['identity'] and s['type'] in (1, 2):
                    if not mapped(s['value'], max(1, s['size'])):
                        raise ValueError(f'{filename}: export outside load segments')
                    symbols.append((s['identity'][0], base + s['value']))
            if len({nid for nid, _ in symbols}) != len(symbols):
                raise ValueError(f'{filename}: ambiguous export')
            dynamic.append(dict(file=filename, base=base, size=modsize, exports=sorted(symbols),
                                sha256=m['sha256']))
            fs_patched += patch_fs_loads(image, m['ph'], base)
            base = (base + modsize + 65535) & ~65535
            continue
        count = 0
        for s in m['symbols']:
            if s['section'] and s['binding'] in (1, 2) and s['identity'] and s['type'] in (1, 2):
                if not mapped(s['value'], max(1, s['size'])):
                    raise ValueError(f'{filename}: export outside load segments')
                if s['identity'] in exports:
                    raise ValueError(f'{filename}: ambiguous export')
                entry_value = (base + s['value'], 2 if s['type'] == 1 else 1, filename)
                exports[s['identity']] = entry_value
                by_nid[(s['identity'][0], s['identity'][1][0])].append(entry_value)
                count += 1
            elif s['section'] and s['binding'] in (1, 2) and s['identity'] and s['type'] == 6 and module_id:
                if s['value'] + max(1, s['size']) > tls['memsz']:
                    raise ValueError(f'{filename}: TLS export outside its TLS block')
                tls_exports[s['identity']] = (module_id, s['value'])
        fs_patched += patch_fs_loads(image, m['ph'], base)
        table.append(dict(file=filename, base=base, size=modsize, init=base + m['tags'].get(12, 0),
                          tls_address=base + tls['vaddr'] if tls else 0, tls_memsz=tls['memsz'] if tls else 0,
                          tls_filesz=tls['filesz'] if tls else 0, tls_module=module_id,
                          tls_relocations=tls_relocations, exports=count, sha256=m['sha256']))
        base = (base + modsize + 65535) & ~65535

    by_index = {index: identity for identity, index in identities.items()}
    for target, kind, index, addend in main_tls_relocs:
        found = tls_exports.get(by_index.get(index))
        if not found:
            raise ValueError(f'eboot TLS relocation against unknown TLS export {names[index]}')
        struct.pack_into('<Q', image, target, found[0] if kind == 3 else found[1] + addend)

    bindings, unresolved = [], []
    for identity, index in identities.items():
        found = exports.get(identity)
        if not found and identity and identity[1][0] == 'libSceLibcInternal':
            candidates = by_nid.get((identity[0], 'libc'), [])
            found = candidates[0] if len(candidates) == 1 else None
        if found:
            bindings.append((index, found[0], found[1]))
        else:
            unresolved.append(names[index])

    main_tls = next((p for p in main['ph'] if p['type'] == 7), None)
    main_tls_values = (main_tls['vaddr'], main_tls['filesz'], main_tls['memsz'], main_tls['align']) if main_tls else (0, 0, 0, 0)
    if main_tls and (main_tls['filesz'] > main_tls['memsz'] or main_tls['memsz'] > 1024 * 1024
                     or main_tls['vaddr'] + main_tls['filesz'] > size):
        raise ValueError('unsupported eboot TLS layout')
    procparam = next(p for p in main['ph'] if p['type'] == 0x61000001)
    with (out / 'boot-linked.bin').open('wb') as f:
        f.write(struct.pack('<8s6Q', b'G3PROBE6', len(image), entry, len(segments), len(relocs), len(names), flags))
        f.write(struct.pack('<Q', procparam['vaddr']))
        f.write(struct.pack('<4Q', *main_tls_values))
        f.write(struct.pack('<Q', len(table)))
        for t in table:
            f.write(struct.pack('<7Q', t['base'], t['size'], t['init'], t['tls_address'], t['tls_memsz'],
                                t['tls_filesz'], t['tls_module']))
        f.write(struct.pack('<Q', len(bindings)))
        for binding in bindings:
            f.write(struct.pack('<3Q', *binding))
        # G3PROBE6: dynamic modules: guest path, base, size, then (NID, address) exports.
        f.write(struct.pack('<Q', len(dynamic)))
        for d in dynamic:
            path = f"/app0/{d['file']}".encode()
            if len(path) >= 64:
                raise ValueError('dynamic module path too long')
            f.write(path.ljust(64, b'\0'))
            f.write(struct.pack('<3Q', d['base'], d['size'], len(d['exports'])))
            for nid, address in d['exports']:
                if len(nid.encode()) >= 16:
                    raise ValueError('export NID too long')
                f.write(nid.encode().ljust(16, b'\0') + struct.pack('<Q', address))
        for segment in segments:
            f.write(struct.pack('<3Q', *segment))
        for name in names:
            if len(name.encode()) >= 128:
                raise ValueError('name too long')
            f.write(name.encode().ljust(128, b'\0'))
        for relocation in relocs:
            f.write(struct.pack('<QQqq', *relocation))
        f.write(image)
    report = dict(modules=[{k: (hex(v) if k in ('base', 'init', 'tls_address') else v) for k, v in t.items()} for t in table],
                  dynamic_modules=[dict(file=d['file'], base=hex(d['base']), size=d['size'], exports=len(d['exports']),
                                        sha256=d['sha256']) for d in dynamic],
                  bindings=len(bindings), imports=len(names), fs_loads_patched=fs_patched,
                  main_tls=dict(zip(('vaddr', 'filesz', 'memsz', 'align'), main_tls_values)),
                  unresolved_imports=unresolved)
    (out / 'link.json').write_text(json.dumps(report, indent=2) + '\n')
    summary = ', '.join([f"{t['file']}@{t['base']:#x}" for t in table] +
                        [f"{d['file']}@{d['base']:#x} (dynamic, {len(d['exports'])} exports)" for d in dynamic])
    print(f'Linked modules: {summary}; {len(bindings)} native bindings, {len(unresolved)} imports left to the host runtime, '
          f'fs->gs patched={fs_patched}')


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'out')
    p.add_argument('--modules', nargs='*', default=list(DEFAULT_MODULES))
    p.add_argument('--dynamic', nargs='*', default=[],
                   help='modules in the game root that the game loads itself (sceKernelLoadStartModule)')
    a = p.parse_args()
    link(a.game, a.out, a.modules, a.dynamic)
