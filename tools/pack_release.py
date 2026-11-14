#!/usr/bin/env python3
"""Packs a SuperWonders release: the program, ready made, and game-build/, what the player's PC needs to set the game up
from their own files (tools/setup_game.py). Nothing of the game is in it. Packing only: nothing is uploaded.

    SuperWonders/wonder.exe, libc++.dll, libunwind.dll     the program (a build with NX_GAME_LIBRARY=ON); wonder on Linux
    SuperWonders/VERSION                                   this release's version
    SuperWonders/update.ini                                repo=<owner/name> for SuperWonders Update ("" : off)
    SuperWonders/recomplauncher.json                       how RecompLauncher sets it up and starts it
    SuperWonders/superwonders-manifest.txt                 every file above and below: size and SHA-256 (the updater
                                                           checks an update against it, and removes only what it names)
    SuperWonders/game-build/setup_game.py, gamebuild.py    the setup
    SuperWonders/game-build/recomp/, tools/                the recompiler, nso2bin, the shader, font and menu tools
    SuperWonders/game-build/include/nx/, gamelib_entry.cpp what the generated code includes, the library's entry
    SuperWonders/game-build/translator/                    the shader translator (a self-contained spirv_harness)
    SuperWonders/game-build/python/                        a Python with capstone, lz4, zstandard and fontTools (Windows)
    SuperWonders/game-build/toolchain/                     a cut-down llvm-mingw: clang and lld for x86-64 (Windows)
    SuperWonders/game-build/kit-id.txt                     tools/gamebuild.py's id of this kit

Then, beside the folder, the archive a GitHub release carries, by a name launchers can match:
SuperWonders-v<version>-Windows-x64.zip / SuperWonders-v<version>-Linux-x64.tar.gz (and <archive>.sig with --sign).

usage: pack_release.py --build <build dir> --out <dir> [--version V] [--repo owner/name] [--sign key.pem]
                       [--toolchain <llvm-mingw>] [--python <CPython install>] [--site-packages <dir>]
                       [--translator <dir of a published spirv_harness>] [--no-archive]
Linux: --linux-toolchain <dir with bin/clang++, lib/, sysroot/> and --linux-python <a relocatable Python with the modules>
(tools/linux/package.sh makes both in the build container) put them in game-build/; left out, the player's own
clang++ and python3 are used (a developer's portable folder). The AppImage itself: tools/linux/package.sh.
"""
import argparse
import getpass
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = "wonder"
WIN = os.name == "nt"
TOP = "SuperWonders"
PY_MODULES = ("capstone", "lz4", "zstandard", "fontTools")


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def copytree(src, dst, ignore=None):
    shutil.copytree(src, dst, ignore=ignore, dirs_exist_ok=True, symlinks=True)


def sanitize_embedded_pdb_paths(folder):
    """Replace local source paths in .NET CodeView records with a neutral PDB name.

    The translator's DLLs can contain the build machine's absolute PDB path even when the PDB files themselves are
    omitted from the release. Keep the CodeView record length intact, but make the path non-identifying.
    """
    user = os.environ.get("USERNAME") or os.environ.get("USER") or getpass.getuser()
    roots = {os.path.expanduser("~"), f"Users/{user}", f"Users\\{user}"}
    roots.update(x.replace("\\", "/") for x in tuple(roots))
    roots.update(x.replace("/", "\\") for x in tuple(roots))
    encodings = ("utf-8", "utf-16-le")
    for base, dirs, files in os.walk(folder):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(base, name)
            if os.path.islink(path):
                continue
            with open(path, "rb") as f:
                data = f.read()
            original = data
            for root in roots:
                if not root or root in ("/", "\\"):
                    continue
                for encoding in encodings:
                    needle = root.encode(encoding)
                    suffix = ".pdb".encode(encoding)
                    placeholder = "source.pdb".encode(encoding)
                    start = 0
                    while True:
                        at = data.lower().find(needle.lower(), start)
                        if at < 0:
                            break
                        end = data.lower().find(suffix.lower(), at + len(needle))
                        if end < 0 or end - at > 4096:
                            start = at + len(needle)
                            continue
                        end += len(suffix)
                        width = end - at
                        if len(placeholder) <= width:
                            data = data[:at] + placeholder + b"\0" * (width - len(placeholder)) + data[end:]
                        start = end
            if data != original:
                with open(path, "wb") as f:
                    f.write(data)


def size(path):
    return sum(os.path.getsize(os.path.join(b, f)) for b, _, fs in os.walk(path) for f in fs)


def pack_toolchain(src, dst):
    """clang and lld for x86_64-w64-mingw32, the C++ library and the Windows headers and import libraries."""
    bins = ["clang.exe", "clang++.exe", "x86_64-w64-mingw32-clang++.exe", "ld.lld.exe", "libLLVM-*.dll",
            "libclang-cpp.dll", "libc++.dll", "libunwind.dll", "libwinpthread-1.dll", "clang-[0-9]*.exe",
            "x86_64-w64-windows-gnu.cfg", "x86_64-pc-windows-gnu.cfg", "mingw32-common.cfg"]
    for pat in bins:
        hits = glob.glob(os.path.join(src, "bin", pat))
        if not hits:
            raise SystemExit(f"toolchain: no bin/{pat} in {src}")
        for h in hits:
            copy(h, os.path.join(dst, "bin", os.path.basename(h)))
    copytree(os.path.join(src, "include"), os.path.join(dst, "include"))
    copytree(os.path.join(src, "x86_64-w64-mingw32"), os.path.join(dst, "x86_64-w64-mingw32"),
             ignore=shutil.ignore_patterns("*.pdb", "share", "bin"))
    for res in glob.glob(os.path.join(src, "lib", "clang", "*")):  # clang's own headers, compiler-rt for x86_64 only
        rel = os.path.relpath(res, src)
        copytree(os.path.join(res, "include"), os.path.join(dst, rel, "include"))
        for lib in glob.glob(os.path.join(res, "lib", "windows", "*x86_64*")):
            copy(lib, os.path.join(dst, rel, "lib", "windows", os.path.basename(lib)))
    if os.path.isfile(os.path.join(src, "LICENSE.TXT")):
        copy(os.path.join(src, "LICENSE.TXT"), os.path.join(dst, "LICENSE.TXT"))


def pack_python(src, site, dst):
    """A CPython install, without what the setup never uses, plus the modules it needs."""
    for f in os.listdir(src):
        p = os.path.join(src, f)
        if os.path.isfile(p) and (f.lower().endswith((".dll", ".exe")) or f.upper().startswith("LICENSE")):
            if f.lower() != "pythonw.exe":
                copy(p, os.path.join(dst, f))
    copytree(os.path.join(src, "DLLs"), os.path.join(dst, "DLLs"),
             ignore=shutil.ignore_patterns("_tkinter*", "tcl*", "tk*", "_test*", "*.pdb"))
    copytree(os.path.join(src, "Lib"), os.path.join(dst, "Lib"),
             ignore=shutil.ignore_patterns("test", "tests", "idlelib", "tkinter", "turtledemo", "ensurepip",
                                           "site-packages", "__pycache__", "lib2to3", "pydoc_data"))
    for pkg in PY_MODULES:
        hits = glob.glob(os.path.join(site, pkg)) + glob.glob(os.path.join(site, pkg.lower() + "-*.dist-info")) + \
            glob.glob(os.path.join(site, pkg + "-*.dist-info"))
        if not hits:
            raise SystemExit(f"python: {pkg} not in {site}")
        for h in hits:
            copytree(h, os.path.join(dst, "Lib", "site-packages", os.path.basename(h)),
                     ignore=shutil.ignore_patterns("__pycache__"))


def pack_kit(kit, a):
    py = sys.executable
    copy(os.path.join(REPO, "tools", "gamebuild.py"), os.path.join(kit, "gamebuild.py"))
    copy(os.path.join(REPO, "tools", "setup_game.py"), os.path.join(kit, "setup_game.py"))
    for f in glob.glob(os.path.join(REPO, "recomp", "*.py")):
        copy(f, os.path.join(kit, "recomp", os.path.basename(f)))
    copy(os.path.join(REPO, "tools", "nso2bin.py"), os.path.join(kit, "tools", "nso2bin.py"))
    copy(os.path.join(REPO, "tools", "fonts", "make_button_font.py"), os.path.join(kit, "tools", "fonts", "make_button_font.py"))
    copy(os.path.join(REPO, "tools", "wonder_menu_mod.py"), os.path.join(kit, "tools", "wonder_menu_mod.py"))
    open(os.path.join(kit, "tools", "__init__.py"), "w").close()
    for f in glob.glob(os.path.join(REPO, "tools", "maxwell", "*.py")):
        copy(f, os.path.join(kit, "tools", "maxwell", os.path.basename(f)))
    for h in ("cpu.h", "module.h", "gamelib.h"):
        copy(os.path.join(REPO, "runtime", "include", "nx", h), os.path.join(kit, "include", "nx", h))
    copy(os.path.join(REPO, "runtime", "src", "gamelib_entry.cpp"), os.path.join(kit, "gamelib_entry.cpp"))
    if not WIN:  # (first: the kit id names the compiler the kit builds with)
        if a.linux_toolchain:
            copytree(a.linux_toolchain, os.path.join(kit, "toolchain"), ignore=None)
        if a.linux_python:
            copytree(a.linux_python, os.path.join(kit, "python"), ignore=shutil.ignore_patterns("__pycache__"))
            py = os.path.join(kit, "python", "bin", "python3")
        # RecompLauncher can run this from an extracted tarball. AppImages use AppRun's
        # --recomp-prepare dispatch instead, because their bundled files live in SquashFS.
        setup_runner = os.path.join(kit, "recomp-setup")
        with open(setup_runner, "w", newline="\n") as f:
            f.write("#!/bin/sh\n"
                    "HERE=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\n"
                    "[ \"${1:-}\" = \"--recomp-prepare\" ] && shift\n"
                    "exec \"$HERE/python/bin/python3\" -I -B \"$HERE/setup_game.py\" setup \"$@\"\n")
        os.chmod(setup_runner, 0o755)
    if a.translator:
        translator = os.path.join(kit, "translator")
        copytree(a.translator, translator, ignore=shutil.ignore_patterns("*.pdb"))
        sanitize_embedded_pdb_paths(translator)
        if not WIN:  # (an archive made on Windows has no executable bit: the setup must be able to start these)
            for exe in ("spirv_harness", "createdump"):
                p = os.path.join(kit, "translator", exe)
                if os.path.isfile(p):
                    os.chmod(p, 0o755)
    else:
        print("note: no --translator: this release can't translate shaders (setup stops at that step)")
    if WIN:
        if not a.toolchain or not a.python:
            raise SystemExit("Windows: --toolchain (llvm-mingw) and --python (a CPython install) are needed")
        pack_toolchain(a.toolchain, os.path.join(kit, "toolchain"))
        site = a.site_packages or next(p for p in sys.path if p.endswith("site-packages"))
        pack_python(a.python, site, os.path.join(kit, "python"))
        py = os.path.join(kit, "python", "python.exe")
    kit_id = subprocess.run([py, "-I", "-B", os.path.join(kit, "gamebuild.py"), "id"], capture_output=True, text=True,
                            check=True).stdout.strip()
    with open(os.path.join(kit, "kit-id.txt"), "w", newline="\n") as f:
        f.write(kit_id + "\n")
    return kit_id


def recomplauncher_json(version):
    """RecompLauncher's setup profile (docs/DEVELOPERS.md there): it starts our setup headless with the player's
    folder, then starts the program to play. The program's own launcher stays out of the way (--play)."""
    exe = f"{GAME}.exe" if WIN else GAME
    if WIN:
        prepare_program = ["game-build/python/python.exe"]
        prepare_args = ["-u", "game-build/setup_game.py", "setup", "--exefs", "{game}/exefs",
                        "--romfs", "{game}/romfs", "--data-root", "{install}/data"]
        start_program = [exe]
    else:
        appimage = f"SuperWonders-v{version}-Linux-x64.AppImage"
        # RecompLauncher prefers the AppImage when present; an extracted tarball falls back
        # to the included setup runner and native executable.
        prepare_program = [appimage, "game-build/recomp-setup"]
        prepare_args = ["--recomp-prepare", "--exefs", "{game}/exefs", "--romfs", "{game}/romfs",
                        "--data-root", "{install}/data"]
        start_program = [appimage, exe]
    return {
        "$schema": "https://recomplauncher.com/schema/recomplauncher.json",
        "format": 1,
        "name": "SuperWonders",
        "repo": "vfjoo888/SuperWonders",
        "game": {
            "kind": "folder",
            "label": "Your Super Mario Bros. Wonder game files, extracted, version 1.0.0: a folder holding exefs and romfs.",
            "check": {"file": "exefs/main"},
        },
        "prepare": {
            "program": prepare_program,
            "args": prepare_args,
            "label": "Builds the game code and the shaders from your game files (about 15 minutes, once)",
            "timeout": 7200,
        },
        "start": {"program": start_program, "args": ["--play"], "env": {"NX_DATA_ROOT": "{install}/data"}},
        "config": [{"id": "main", "format": "ini", "file": [{"path": "{install}/data/pc-settings.ini"}],
                    "new": {"vsync": "1", "frame_cap": "60", "upscaler": "1", "antialiasing": "0",
                            "anisotropy": "0", "game_language": "0"}}],
        "settings": [
            {"id": "fullscreen", "label": "Full screen", "choices": [
                {"label": "Off", "set": {"fullscreen": "0"}}, {"label": "On", "set": {"fullscreen": "1"}}],
             "defaults": {"fullscreen": "0"}},
            {"id": "res", "label": "Internal resolution", "help": "What the game draws at. Original is the console's.",
             "choices": [{"label": "Original", "set": {"internal_resolution": "0"}},
                         {"label": "1080p", "set": {"internal_resolution": "3"}},
                         {"label": "1440p", "set": {"internal_resolution": "4"}},
                         {"label": "4K", "set": {"internal_resolution": "5"}}],
             "defaults": {"internal_resolution": "0"}},
            {"id": "vsync", "label": "V-Sync", "help": "Fast shows the newest frame at each refresh without tearing.",
             "choices": [{"label": "On", "set": {"vsync": "0"}}, {"label": "Fast", "set": {"vsync": "1"}},
                         {"label": "Adaptive", "set": {"vsync": "2"}}, {"label": "Off", "set": {"vsync": "3"}}],
             "defaults": {"vsync": "1"}},
            {"id": "frame_cap", "label": "Frame rate cap", "help": "Higher caps require a display and PC that can sustain them.",
             "choices": [{"label": f"{fps} FPS", "set": {"frame_cap": str(fps)}} for fps in (30, 60, 90, 120, 144)],
             "defaults": {"frame_cap": "60"}},
            {"id": "upscaler", "label": "Upscaling", "choices": [
                {"label": "Bilinear", "set": {"upscaler": "0"}}, {"label": "FSR 1", "set": {"upscaler": "1"}}],
             "defaults": {"upscaler": "1"}},
            {"id": "antialiasing", "label": "Anti-aliasing", "choices": [
                {"label": "Off", "set": {"antialiasing": "0"}}, {"label": "FXAA", "set": {"antialiasing": "1"}}],
             "defaults": {"antialiasing": "0"}},
            {"id": "anisotropy", "label": "Anisotropic filtering", "choices": [
                {"label": "Game default", "set": {"anisotropy": "0"}},
                *[{"label": f"{x}x", "set": {"anisotropy": str(x)}} for x in (2, 4, 8, 16)]],
             "defaults": {"anisotropy": "0"}},
            {"id": "game_language", "label": "Game language", "choices": [
                {"label": "English", "set": {"game_language": "0"}},
                {"label": "Italiano", "set": {"game_language": "1"}},
                {"label": "Deutsch", "set": {"game_language": "2"}},
                {"label": "Français", "set": {"game_language": "3"}}],
             "defaults": {"game_language": "0"}},
            {"id": "lowvram", "label": "Low VRAM", "group": "Advanced", "advanced": True,
             "help": "For graphics cards with 4 GB or less.",
             "choices": [{"label": "Off", "set": {"low_vram_profile": "0"}}, {"label": "On", "set": {"low_vram_profile": "1"}}],
             "defaults": {"low_vram_profile": "0"}},
        ],
        "saves": ["{install}/data/user/save"],
        "folders": {"logs": "{install}/data"},
        "exit_codes": {"0": "ok", "1": "SuperWonders stopped with an error: its log is data/nx.log"},
        "requires": ["start", "settings.ini", "settings.choices", "files.folder"],
    }


def drop_caches(folder):
    """Python's bytecode caches: never shipped (a .pyc keeps the path it was compiled at; Python makes them again)."""
    for base, dirs, files in os.walk(folder):
        for d in [d for d in dirs if d == "__pycache__"]:
            shutil.rmtree(os.path.join(base, d))
            dirs.remove(d)
        for f in files:
            if f.endswith((".pyc", ".pyo")):
                os.remove(os.path.join(base, f))


# Files that may say "OneDrive" (Windows' own known-folder names, FOLDERID_OneDrive / SkyDrive): the toolchain's
# Windows headers and import libraries, nothing else.
ONEDRIVE_OK = re.compile(r"(^|.*/)game-build/toolchain/.*\.(h|a|def)$", re.I)
GAME_FILE = re.compile(r"(^|/)(main|sdk|main\.npdm|rtld)$|\.(nso|nro|nca|nsp|xci|npdm|sarc|szs|bfres|bntx|zs|byml|msbt|blarc)$|"
                       r"(^|/)(romfs|exefs)(/|$)", re.I)


# An e-mail address. Looked for in SuperWonders' own files only: the third-party parts of a release (Python's
# standard library, the compiler's headers, the .NET runtime) are full of their authors' public addresses.
# (With a real top-level domain at the end: shader code in the program has names such as "X@entryPointOutput.pos".)
TLDS = (b"com|org|net|edu|gov|mil|int|io|me|dev|app|co|info|biz|xyz|tech|online|email|mail|cloud|uk|de|fr|it|es|nl|be|"
        b"ch|at|se|no|fi|dk|pl|cz|pt|ie|ru|ua|jp|cn|kr|tw|hk|in|au|nz|ca|us|br|mx|ar|cl|eu|is|gg|tv|me|ai|so|to|ly|im")
EMAIL = re.compile(rb"[A-Za-z0-9._%+-]{1,64}@[A-Za-z0-9-]{1,63}(?:\.[A-Za-z0-9-]{1,63})*\.(?:" + TLDS +
                   rb")(?![A-Za-z0-9])", re.I)
THIRD_PARTY = ("game-build/python/", "game-build/toolchain/", "game-build/translator/")
# Personal strings to refuse anywhere in a release, one per line ('#' comments): the packer's own names, mail,
# folders. Local and untracked (.gitignore), so nothing personal is ever written in the code.
EXTRA = os.path.join(REPO, "tools", "privacy-extra.txt")


def privacy_check(folder):
    """Fails the pack if anything in `folder` names the machine that packed it or holds the game. Looks at every
    file's name and contents (UTF-8 and UTF-16, as programs keep paths in either): this PC's home folder and user
    name, the private game data folder, OneDrive, an e-mail address (in SuperWonders' own files), the strings in
    tools/privacy-extra.txt, and files that are, or come from, the game."""
    home = os.path.expanduser("~")
    user = os.environ.get("USERNAME") or os.environ.get("USER") or getpass.getuser()
    needles = {  # name -> the text
        # (with a separator after it: a path inside the home folder. The bare name can be ordinary text, e.g. "/root"
        # in the .NET runtime when the build runs as root.)
        "home folder": home.rstrip("\\/") + os.sep, "home folder (/)": home.replace("\\", "/").rstrip("/") + "/",
        "SuperWonders-private": "SuperWonders-private", "OneDrive": "OneDrive",
        "user folder": f"Users\\{user}", "user folder (/)": f"Users/{user}", "home (Linux)": f"/home/{user}/",
    }
    if os.path.isfile(EXTRA):
        with open(EXTRA, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    needles[f"a string from {os.path.basename(EXTRA)}"  + f" #{len(needles)}"] = line
    pats = []
    for name, text in needles.items():
        for enc in ("utf-8", "utf-16-le"):
            pat = re.escape(text.encode(enc))
            if name == "OneDrive":  # (not Windows' known-folder id FOLDERID_OneDrive, which every Windows program has)
                pat = b"(?<!" + re.escape("FOLDERID_".encode(enc)) + b")" + pat
            if name.startswith("home"):  # (not "Users\<name>": that one always follows a drive, C:\)
                # Only where a path starts (after the start of the file, a NUL, a quote, a space...), never inside
                # another path: build machines' "/opt/rh/devtoolset-10/root/usr/..." in manylinux wheels and LLVM.
                c = b"[A-Za-z0-9_./\\\\-]"
                pat = (b"(?<!" + c + b")" if enc == "utf-8" else b"(?<!" + c + b"\x00)") + pat
            pats.append((name, re.compile(pat, re.I)))
    problems = []
    for base, dirs, files in os.walk(folder):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(base, f)
            rel = os.path.relpath(p, folder).replace(os.sep, "/")
            if GAME_FILE.search(rel) and not rel.startswith(("game-build/toolchain/", "game-build/python/")):
                problems.append(f"{rel}: looks like a game file")
            if os.path.islink(p):
                continue
            with open(p, "rb") as fh:
                data = fh.read()
            # Random bytes in a native executable can accidentally match the email regex (and removing NULs
            # makes that far more likely). Paths and game-file signatures are still checked in binaries below.
            is_native_binary = data.startswith((b"\x7fELF", b"MZ", b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf"))
            is_third_party = any(rel.startswith(prefix) or f"/{prefix}" in f"/{rel}" for prefix in THIRD_PARTY)
            if not is_third_party and not is_native_binary:
                m = EMAIL.search(data) or EMAIL.search(data.replace(b"\x00", b""))
                if m:
                    problems.append(f"{rel}: contains an e-mail address ({m.group()!r})")
            for name, pat in pats:
                if name == "OneDrive" and ONEDRIVE_OK.search(rel):
                    continue
                m = pat.search(data)
                if m:
                    at = data[max(0, m.start() - 40):m.end() + 40]
                    problems.append(f"{rel}: contains {name} ({at!r})")
                    break
    if problems:
        print("privacy check FAILED: this release must not ship:")
        for line in problems[:60]:
            print("  " + line)
        if len(problems) > 60:
            print(f"  ... and {len(problems) - 60} more")
        raise SystemExit(1)
    print(f"privacy check: {folder}: nothing of this PC or of the game")


def write_manifest(folder, version):
    lines = ["superwonders-update-manifest 1", f"version {version}"]
    for base, dirs, files in os.walk(folder):
        dirs.sort()
        for name in sorted(files):
            p = os.path.join(base, name)
            rel = os.path.relpath(p, folder).replace(os.sep, "/")
            if rel == "superwonders-manifest.txt" or os.path.islink(p):  # (links come with the archive, not the list)
                continue
            h = hashlib.sha256()
            with open(p, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
            lines.append(f"f {h.hexdigest()} {os.path.getsize(p)} {rel}")
    with open(os.path.join(folder, "superwonders-manifest.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return len(lines) - 2


def archive(out, folder, version):
    if WIN:
        name = f"SuperWonders-v{version}-Windows-x64.zip"
        with zipfile.ZipFile(os.path.join(out, name), "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for base, dirs, files in os.walk(folder):
                dirs.sort()
                for f in sorted(files):
                    p = os.path.join(base, f)
                    z.write(p, TOP + "/" + os.path.relpath(p, folder).replace(os.sep, "/"))
    else:
        name = f"SuperWonders-v{version}-Linux-x64.tar.gz"
        with tarfile.open(os.path.join(out, name), "w:gz") as t:
            t.add(folder, arcname=TOP)
    return os.path.join(out, name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True, help="a build folder with NX_GAME_LIBRARY=ON")
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", default=open(os.path.join(REPO, "VERSION")).read().strip())
    ap.add_argument("--repo", default="", help="the GitHub repository SuperWonders Update checks (owner/name)")
    ap.add_argument("--sign", help="the repository owner's release key (PEM): writes <archive>.sig (docs/RELEASES.md)")
    ap.add_argument("--pubkey", default="", help="the release key's public half (tools/sign_update.py --public), for "
                                                  "update.ini: then only releases signed with it are installed")
    ap.add_argument("--toolchain", help="llvm-mingw (Windows)")
    ap.add_argument("--python", help="a CPython install (Windows)")
    ap.add_argument("--site-packages", help=f"where {', '.join(PY_MODULES)} are (default: this Python's)")
    ap.add_argument("--translator", help="a published spirv_harness (dotnet publish --self-contained)")
    ap.add_argument("--linux-toolchain", help="Linux: clang, lld and a sysroot for game-build/toolchain")
    ap.add_argument("--linux-python", help="Linux: a relocatable Python with the modules, for game-build/python")
    ap.add_argument("--no-archive", action="store_true")
    if len(sys.argv) == 3 and sys.argv[1] == "--check-only":  # (any folder: the AppImage's AppDir, tools/linux/package.sh)
        drop_caches(sys.argv[2])
        privacy_check(sys.argv[2])
        return 0
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    folder = os.path.join(out, TOP)
    if os.path.exists(folder):
        shutil.rmtree(folder)
    exe = f"{GAME}.exe" if WIN else GAME
    program = [exe] + (["libc++.dll", "libunwind.dll"] if WIN else [])
    for f in program:
        if not os.path.isfile(os.path.join(a.build, f)):
            raise SystemExit(f"{a.build}: no {f} (a build with NX_GAME_LIBRARY=ON)")
        copy(os.path.join(a.build, f), os.path.join(folder, f))
    if os.path.isfile(os.path.join(a.build, f"{GAME}_game.dll")) or os.path.isfile(os.path.join(a.build, f"lib{GAME}_game.so")):
        print("note: the build's game code library is not packed (it holds the game)")
    with open(os.path.join(folder, "VERSION"), "w", newline="\n") as f:
        f.write(a.version + "\n")
    with open(os.path.join(folder, "update.ini"), "w", newline="\n") as f:
        f.write("# SuperWonders Update checks this GitHub repository's releases. Empty: no checks.\n"
                f"repo={a.repo}\n"
                "# The repository owner's release key, public half (docs/RELEASES.md). Set: only releases signed with\n"
                "# it are installed. Empty: GitHub's SHA-256 of the file alone is checked.\n"
                f"pubkey={a.pubkey}\n")
    with open(os.path.join(folder, "recomplauncher.json"), "w", newline="\n") as f:
        json.dump(recomplauncher_json(a.version), f, indent=2)
    kit_id = pack_kit(os.path.join(folder, "game-build"), a)
    drop_caches(folder)
    privacy_check(folder)
    n = write_manifest(folder, a.version)
    print(f"{folder}: {n} files, {size(folder) / 1e6:.0f} MB, version {a.version}, kit {kit_id}")
    if not a.no_archive:
        arc = archive(out, folder, a.version)
        print(f"{arc}: {os.path.getsize(arc) / 1e6:.0f} MB")
        if a.sign:
            subprocess.run([sys.executable, os.path.join(REPO, "tools", "sign_update.py"), arc, a.sign], check=True)


if __name__ == "__main__":
    sys.exit(main())
