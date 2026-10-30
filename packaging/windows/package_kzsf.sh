#!/usr/bin/env bash
# Builds dist/Killzone Shadow Fall/ and dist/KillzoneShadowFall-v<version>-windows.zip: the launcher
# (Killzone Shadow Fall.exe, frozen with PyInstaller so players need no Python), the game
# executable with the MSYS2 CLANG64 DLLs it needs, the preparation scripts and run.py.
# Run from an MSYS2 CLANG64 shell after `bash build.sh`. Freezing uses a Windows Python 3.10+
# (python.org; WINPYTHON overrides) and a private venv in out/pyenv.
# No game files ship: the player points the launcher at their own dump.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source ./msys2-env.sh
[[ -f out/gow3-probe.exe ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
[[ -f launcher/kzsf.ico ]] || { echo 'launcher/kzsf.ico is missing: packaging/windows/make_kzsf_icon.py <icon0.png> launcher/kzsf.ico' >&2; exit 1; }
version=$(sed -n "s/^VERSION = '\(.*\)'/\1/p" launcher/kzsf_launcher.py)
name='Killzone Shadow Fall'

python=${WINPYTHON:-}
if [[ -z $python ]]; then
    for candidate in /c/Python3*/python.exe "${LOCALAPPDATA:-/c/Users/$USER/AppData/Local}"/Programs/Python/Python3*/python.exe; do
        [[ -x $candidate ]] && python=$candidate
    done
fi
[[ -n $python ]] || { echo 'Need a Windows Python 3 (python.org) or WINPYTHON=path\to\python.exe' >&2; exit 1; }
export USERPROFILE=${USERPROFILE:-$(cygpath -w "/c/Users/$(id -un)")}
if [[ ! -x out/pyenv/Scripts/python.exe ]]; then
    "$python" -m venv out/pyenv
fi
out/pyenv/Scripts/python.exe -m pip install -q --disable-pip-version-check pyinstaller pillow iced-x86==1.21.0 pywebview==6.2.1
# The scripts run inside the launcher executable (--script): the standard modules they import come along.
hidden=()
for module in argparse base64 collections hashlib json re shutil socket struct tempfile threading uuid \
              xml.etree.ElementTree urllib.request ctypes.wintypes iced_x86 \
              asyncio ssl http.server http.client http.cookies ipaddress csv zipfile zlib gzip secrets \
              dataclasses copy datetime queue urllib.parse email.parser contextlib; do
    hidden+=(--hidden-import "$module")
done
out/pyenv/Scripts/python.exe -m PyInstaller --noconfirm --clean --log-level WARN --windowed --copy-metadata pillow \
    --name "$name" --icon "$(cygpath -w "$PWD/launcher/kzsf.ico")" --distpath out/kzsf-pyi-dist \
    --workpath out/kzsf-pyi-work --specpath out/kzsf-pyi-work --paths "$(cygpath -w "$PWD/scripts")" "${hidden[@]}" \
    --add-data "$(cygpath -w "$PWD/launcher/web");web" --hidden-import kzsf_web \
    "$(cygpath -w "$PWD/launcher/kzsf_launcher.py")"

dest="out/kzsf-stage/$name"
rm -rf -- out/kzsf-stage
mkdir -p "$dest/bin" "$dest/launcher" "$dest/patches"
cp -r "out/kzsf-pyi-dist/$name/." "$dest/"
cp launcher/kzsf.ico "$dest/launcher/"
# The game executable without debug information (out/ keeps the symbols for crash reports).
llvm-strip --strip-debug -o "$dest/bin/KillzoneShadowFall.exe" out/gow3-probe.exe
# Every DLL it loads from the CLANG64 tree (SDL3, FFmpeg, Vulkan loader, ...).
ldd "$dest/bin/KillzoneShadowFall.exe" | awk '/\/clang64\/bin\// {print $3}' | sort -u | while read -r dll; do
    cp -u "$dll" "$dest/bin/"
done
# NVIDIA DLSS (packaging/windows/build_dlss_kzsf.sh): the MSVC bridge and NVIDIA's runtime, when built.
if [[ -f out/gow3_dlss.dll && -f out/nvngx_dlss.dll ]]; then
    cp out/gow3_dlss.dll out/nvngx_dlss.dll "$dest/bin/"
    # DLSS Frame Generation (GeForce RTX 40/50), next to nvngx_dlss.dll where NGX looks for it.
    if [[ -f out/nvngx_dlssg.dll ]]; then
        cp out/nvngx_dlssg.dll "$dest/bin/"
    fi
    mkdir -p "$dest/licenses"
    cp out/NVIDIA-DLSS-LICENSE.txt "$dest/licenses/"
    cp gpu/dlss_bridge/LICENSE.txt "$dest/licenses/gow3_dlss-LICENSE.txt"
else
    echo 'Note: DLSS not packaged (run packaging/windows/build_dlss_kzsf.sh first)' >&2
fi
cp -r scripts "$dest/"
# Private multiplayer (beta): the room scripts (run inside the launcher executable, --script), without
# their tests or any runtime data, and OpenSSL for the host's certificate.
mkdir -p "$dest/multiplayer/server/fixtures" "$dest/multiplayer/openssl"
cp multiplayer/*.py multiplayer/README.md "$dest/multiplayer/"
cp multiplayer/server/fixtures/* "$dest/multiplayer/server/fixtures/"
for f in multiplayer/server/*.py; do
    [[ $(basename "$f") == test_* ]] || cp "$f" "$dest/multiplayer/server/"
done
cp /clang64/bin/openssl.exe /clang64/bin/libcrypto-3-x64.dll /clang64/bin/libssl-3-x64.dll "$dest/multiplayer/openssl/"
cp run.py LICENSE "$dest/"
# The render resolution patches (tools/make_kzsf_res_patches.py): our own file, no game data.
cp patches/Killzone_Shadow_Fall.xml "$dest/patches/"
cp packaging/windows/LEEME-README.txt "$dest/LEEME - README.txt"
# The classic (Tk) launcher window, for PCs where the new one does not work.
cp "packaging/windows/Classic launcher.bat" "$dest/"
# FSR 4: the fixed and faster passes (tools/fsr4_optimize.sh: no data race in pass 11 at 1080p,
# post passes ~3.5x faster), built here from AMD's MIT-licensed model (FireBurn/Q2RTX build) with
# spirv-cross and glslang; players cannot build them. The launcher downloads the original files
# next to them (launcher/kzsf_fsr4.py); vk_fsr4.cpp prefers opt/.
bash tools/fetch_fsr4_assets.sh
opt_count=$(ls fsr4_shaders/opt/*.spv 2>/dev/null | wc -l)
if [[ $opt_count != 24 ]]; then
    echo "fsr4_shaders/opt holds $opt_count passes, 24 expected (spirv-cross and glslangValidator needed)" >&2
    exit 1
fi
mkdir -p "$dest/fsr4_shaders/opt"
cp fsr4_shaders/opt/*.spv "$dest/fsr4_shaders/opt/"
cp fsr4_shaders/LICENSE-FSR4-v07.txt "$dest/fsr4_shaders/"
find "$dest" -name __pycache__ -prune -exec rm -r {} +
# pythonnet's API documentation (the launcher window's pywebview): not needed, and an .xml.
rm -f "$dest/_internal/pythonnet/runtime/Python.Runtime.xml"
# No game files and nothing a player made (saves, caches, settings, logs) may ship.
forbidden=$(find "$dest" \( -iname eboot.bin -o -iname sce_module -o -iname sce_sys -o -iname savedata \
    -o -iname user -o -iname cache -o -iname '*.log' -o -iname '*.ini' -o -iname launcher.json \
    -o \( -path "$dest/fsr4_shaders/*" -not -path "$dest/fsr4_shaders/opt/*" -not -name LICENSE-FSR4-v07.txt -not -name opt \) \
    -o \( -iname '*.spv' -not -path "$dest/fsr4_shaders/opt/*" \) -o -iname '*.pkg' -o -iname '*.sprx' -o -iname '*.prx' -o \( -iname '*.xml' -not -path "$dest/patches/Killzone_Shadow_Fall.xml" -not -path "$dest/multiplayer/server/fixtures/*" \) \) -print)
if [[ -n $forbidden ]]; then
    echo "Package holds files that must not ship:" >&2
    echo "$forbidden" >&2
    exit 1
fi
zip="dist/KillzoneShadowFall-v$version-windows.zip"
mkdir -p dist
rm -f "$zip"
# Python's zipfile, not Compress-Archive: Windows PowerShell 5.1 stores "\" in the entry names,
# which other unzip tools turn into files named after the folders (#87).
out/pyenv/Scripts/python.exe - "$(cygpath -w "$PWD/out/kzsf-stage")" "$name" "$(cygpath -w "$PWD/$zip")" <<'PY'
import os, sys, zipfile
root, name, target = sys.argv[1:]
with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for folder, dirs, files in os.walk(os.path.join(root, name)):
        dirs.sort()
        for file in sorted(files):
            path = os.path.join(folder, file)
            z.write(path, os.path.relpath(path, root).replace(os.sep, '/'))
PY

# Refresh dist/<name> (a playable copy), keeping what the player made there (user/) and the
# downloaded FSR 4 shaders (launcher/kzsf_fsr4.py).
play="dist/$name"
running=$(powershell -NoProfile -Command \
    "@(Get-Process | Where-Object { \$_.Path -like '$(cygpath -w "$PWD/$play")\\*' }).Count" | tr -d '\r')
if [[ ${running:-0} != 0 ]]; then
    echo "$play is in use ($running processes): not refreshed; the zip is ready." >&2
else
    mkdir -p "$play"
    find "$play" -mindepth 1 -maxdepth 1 ! -name user ! -name fsr4_shaders -exec rm -rf {} +
    cp -r "$dest/." "$play/"
fi
du -sh "$dest" "$zip"
