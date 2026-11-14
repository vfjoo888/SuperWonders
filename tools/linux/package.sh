#!/bin/bash
# tools/linux/package.sh (inside the build container, tools/linux/appimage.sh): the SuperWonders AppImage, after Savage
# Wind's tools/linux/package.sh (github.com/Revivedx/SavageWindRecomp, MIT).
#   build/dist/SuperWonders-v<version>-Linux-x64.AppImage    the release (updates itself: launcher/update_appimage.c)
#   build/dist/SuperWonders-v<version>-Linux-x64.tar.gz      the same files as a portable folder (secondary)
# No game code inside: the launcher builds it on the player's machine from their own files (tools/setup_game.py). So the
# AppImage carries what that needs, in usr/bin/game-build/: the recompiler and tools, the shader translator, a Python, and
# clang 21 with a sysroot of this container's glibc and libstdc++ (the game code library then uses the system's at run
# time, as the program does). Its data is never inside the image: $XDG_DATA_HOME/superwonders (platform_linux.cpp).
#
# TRANSLATOR=<dir>: a published linux-x64 spirv_harness (dotnet publish tools/maxwell/spirv_harness -r linux-x64
# --self-contained: it cross-publishes from any OS). REPO=<owner/name>: what update.ini names (default: none, off).
set -euo pipefail
cd "$(dirname "$0")/../.."
L=$PWD/build/linux/x86_64
B=$L/game
T=$L/toolchain
PYDIR=$L/python
OUTDIR=$PWD/build/dist
VERSION=$(tr -d '\r' < VERSION)
NAME=SuperWonders-v$VERSION-Linux-x64
JOBS=$(nproc)
PY=${PYTHON:-python3.12}  # (the container's; on a newer host, PYTHON=python3)
GCCV=$(ls /usr/lib/gcc/x86_64-linux-gnu | sort -n | tail -1)  # (11 in the container; whatever the host has)
: "${TRANSLATOR:?TRANSLATOR=<a published linux-x64 spirv_harness>}"

fetch() {  # url sha256 file: downloads pinned by their SHA-256
    mkdir -p build/linux/src
    [ -f "build/linux/src/$3" ] || { curl -fsSL -o "build/linux/src/$3.part" "$1" && mv "build/linux/src/$3.part" "build/linux/src/$3"; }
    echo "$2  build/linux/src/$3" | sha256sum -c --quiet
}

# ---- the program (no game code: NX_GAME_LIBRARY; SDL3 linked in: 22.04 has none)
cmake -S . -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNX_GAME=wonder -DNX_GAME_LIBRARY=ON -DNX_SDL_BUNDLED=ON \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DNX_GEN_DIR="$L/no-gen" >/dev/null
ninja -C "$B" -j"$JOBS" wonder
if nm "$B/wonder" | grep -q ' main_[0-9a-f]\{8\}'; then echo "$B/wonder has game code linked in"; exit 1; fi

# ---- the compiler: clang 21 and lld, with the libraries they need that a base system may not have
rm -rf "$T" && mkdir -p "$T/bin" "$T/lib"
cp -L /usr/lib/llvm-21/bin/clang-21 "$T/bin/clang"
printf '#!/bin/sh\nexec "$(dirname "$0")/clang" --driver-mode=g++ "$@"\n' > "$T/bin/clang++"  # (C++, without a second copy)
chmod +x "$T/bin/clang++"
cp -L /usr/lib/llvm-21/bin/lld "$T/bin/ld.lld"
mkdir -p "$T/lib/clang" && cp -RL /usr/lib/llvm-21/lib/clang/21 "$T/lib/clang/"  # (its own headers: <bin>/../lib/clang/21)
needed() { readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }
BASE='^(libc|libm|libdl|libpthread|librt|libstdc\+\+|libgcc_s)\.so|^ld-linux'
todo=("$T/bin/clang" "$T/bin/ld.lld")
while [ ${#todo[@]} -gt 0 ]; do
    f=${todo[0]}; todo=("${todo[@]:1}")
    for n in $(needed "$f"); do
        [[ $n =~ $BASE ]] && continue
        [ -e "$T/lib/$n" ] && continue
        src=$(ldconfig -p | sed -n "s/^\s*$n (libc6,x86-64) => //p" | head -1)
        [ -n "$src" ] || src=$(find /usr/lib/llvm-21/lib -name "$n" | head -1)
        [ -n "$src" ] || { echo "toolchain: $(basename "$f") needs $n, not found"; exit 1; }
        cp -L "$src" "$T/lib/$n" && patchelf --set-rpath '$ORIGIN' "$T/lib/$n"
        todo+=("$T/lib/$n")
    done
done
patchelf --set-rpath '$ORIGIN/../lib' "$T/bin/clang" "$T/bin/ld.lld"
# the sysroot: glibc's and libstdc++'s headers and what linking a shared library against them needs (crt files,
# libc.so's linker script and its parts, libstdc++.so, libgcc). Only for building: at run time the system's are used.
S=$T/sysroot
for pkg in libc6-dev linux-libc-dev libstdc++-$GCCV-dev libgcc-$GCCV-dev libc6 libstdc++6 libgcc-s1; do
    dpkg -L "$pkg" | while read -r f; do
        [ -f "$f" ] || [ -L "$f" ] || continue
        case $f in /usr/share/*|/etc/*|/usr/lib/x86_64-linux-gnu/gconv/*|/sbin/*|/usr/bin/*|/usr/sbin/*) continue ;; esac
        mkdir -p "$S$(dirname "$f")" && cp -L "$f" "$S$f" 2>/dev/null || true
    done
done
# (libc.so and libm.so are linker scripts naming /lib/... and /usr/lib/...: --sysroot puts them under the sysroot)
echo "toolchain: $(du -sh "$T" | cut -f1) ($(ls "$T/lib" | grep -v clang | tr '\n' ' '))"

# ---- Python: a relocatable CPython (python-build-standalone, pinned) with the setup's modules
fetch https://github.com/astral-sh/python-build-standalone/releases/download/20261026/cpython-3.12.16+20261026-x86_64-unknown-linux-gnu-install_only.tar.gz \
      6a79e922e50006b601f29e688407659da1c2b86ebcd4e9c7254cf234be8b89de python-3.12.16.tar.gz
rm -rf "$PYDIR" && mkdir -p "$PYDIR" && tar -xzf build/linux/src/python-3.12.16.tar.gz -C "$PYDIR" --strip-components=1
PYTHONDONTWRITEBYTECODE=1 "$PYDIR/bin/python3" -m pip install -q --no-warn-script-location --no-compile \
    capstone==5.0.9 lz4==4.4.5 zstandard==0.25.0 fonttools==4.68.0
rm -rf "$PYDIR/lib/python3.12/test" "$PYDIR/lib/python3.12/idlelib" "$PYDIR/lib/python3.12/tkinter" "$PYDIR/share"
find "$PYDIR" -name __pycache__ -prune -exec rm -rf {} +
# (no symbolic links in the release: the updater's file list moves files, python3 -> python3.12 becomes a copy)
find "$PYDIR" -type l | while read -r l; do t=$(readlink -f "$l"); rm "$l"; cp "$t" "$l"; done
find "$T" -type l | while read -r l; do t=$(readlink -f "$l"); rm "$l"; cp "$t" "$l"; done

# ---- the release folder and the portable archive (tools/pack_release.py)
"$PY" tools/pack_release.py --build "$B" --out "$L/release" --version "$VERSION" --repo "${REPO:-}" \
    --translator "$TRANSLATOR" --linux-toolchain "$T" --linux-python "$PYDIR"
mkdir -p "$OUTDIR" && mv "$L/release/$NAME.tar.gz" "$OUTDIR/"

# ---- the AppImage: the release folder as usr/bin (appimagetool runs extracted: no FUSE in the container)
APPDIR=$L/AppDir
rm -rf "$APPDIR" && mkdir -p "$APPDIR/usr/bin" && cp -a "$L/release/SuperWonders/." "$APPDIR/usr/bin/"
cp "$L/release/SuperWonders/recomplauncher.json" "$APPDIR/recomplauncher.json"
cp tools/linux/AppRun "$APPDIR/" && chmod +x "$APPDIR/AppRun"
cp tools/linux/superwonders.desktop "$APPDIR/"
cp assets/wonder_app.svg "$APPDIR/superwonders.svg" && ln -sf superwonders.svg "$APPDIR/.DirIcon"
"$PY" tools/pack_release.py --check-only "$APPDIR"  # (the AppDir too: nothing of this PC or of the game)
fetch https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
      ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 appimagetool-x86_64.AppImage
fetch https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 \
      2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d runtime-x86_64
chmod +x build/linux/src/appimagetool-x86_64.AppImage
rm -f "$OUTDIR/$NAME.AppImage"
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 build/linux/src/appimagetool-x86_64.AppImage --no-appstream \
    --runtime-file build/linux/src/runtime-x86_64 "$APPDIR" "$OUTDIR/$NAME.AppImage"
ls -la "$OUTDIR/$NAME.AppImage" "$OUTDIR/$NAME.tar.gz"
