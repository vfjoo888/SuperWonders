#!/usr/bin/env python3
"""Builds the game code library on the player's own PC (runtime/include/nx/gamelib.h): their ExeFS main and sdk,
recompiled (recomp/) and compiled into wonder_game.dll (Windows) / libwonder_game.so (Linux), which the program loads.

Nothing of the game ships with SuperWonders: this is how the player's copy becomes the game code. Savage Wind does the
same with its swgame library (runtime/gamebuild.c there): the program ships ready made, the game code is built here.

    gamebuild.py build --exefs <dir with main and sdk> [--data-root DIR] [--jobs N] [--keep-work]
    gamebuild.py check --exefs <dir> [--data-root DIR]     exit 0: up to date, 10: needs a build (says why)
    gamebuild.py id                                         the id of this build kit

The library goes to <data root>/game/ with build-id.txt beside it: the kit's id (the recompiler, the headers the
generated code includes, the compiler and its flags) and the input's (main and sdk). Either changing means a build.
The working files (the generated C++ and the objects, several GB: they hold the player's game) are made in
<data root>/game/work and removed after a good build. --keep-work keeps them, so a build after an interrupted one, or
for a debugger, only redoes what changed. A new kit id starts the objects again, since they can't be reused.

Progress for launchers: lines "progress <0..1> <what>" on stdout (RecompLauncher's prepare protocol), exit 0 when it
worked. It runs from a source checkout (recomp/, runtime/include/) or from a release's game-build/ folder, which holds
the same files beside this script plus a Python and a compiler (tools/pack_gamebuild_kit.py).
"""
import argparse
import concurrent.futures
import glob
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import time

GAME = "wonder"
HERE = os.path.dirname(os.path.abspath(__file__))
WIN = os.name == "nt"
LIB = f"{GAME}_game.dll" if WIN else f"lib{GAME}_game.so"
MODULES = (("main", "0x4000000000"), ("sdk", "0x4010000000"))
WONDER_PROGRAM_ID = 0x010015100B514000
# Full 40-hex (or 16-hex prefix for known unsupported patches) NSO0 main Build ID -> (version, expected_romfs_product, compatible)
EXEFS_BUILD_IDS = {
    "CD6E42AEE7934F4D8393CD43893AE25EDC83D338": ("1.0.0", 100, True),
    "FF773E90972D544EB79406EAA65396D53C43EFB9": ("1.2.1", 120, True),
    "F91868B88F60D3D5": ("1.0.1", 101, False),
    "B37398A1B68747B7": ("1.2.0", 120, False),
}
WARN = ["-Wno-unused-label", "-Wno-unused-value", "-Wno-parentheses-equality", "-Wno-unused-variable",
        "-Wno-unused-but-set-variable", "-Wno-constant-logical-operand", "-Wno-unused-function"]
# The same flags as CMakeLists.txt gives game_main / game_sdk and <game>_game (keep them in step).
if WIN:
    CXXFLAGS = ["-O3", "-DNDEBUG", "-std=gnu++20", *WARN, "-mavx2", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN"]
    LDFLAGS = ["-O3", "-shared"]
else:
    CXXFLAGS = ["-O2", "-DNDEBUG", "-std=gnu++20", *WARN, "-march=x86-64-v3", "-fPIC", "-fvisibility=hidden",
                "-fno-semantic-interposition"]
    LDFLAGS = ["-shared", "-Wl,-Bsymbolic", "-Wl,--no-undefined"]
EXIT_NEEDS_BUILD = 10


class Kit:
    """Where the recompiler, headers, entry file and compiler are: a release's game-build/ folder, or a checkout."""

    def __init__(self, android_ndk=None):
        if os.path.isdir(os.path.join(HERE, "recomp")):  # a release: game-build/{recomp,include,gamelib_entry.cpp}
            self.root = HERE
            self.include = os.path.join(HERE, "include")
            self.entry = os.path.join(HERE, "gamelib_entry.cpp")
        else:  # a checkout: tools/gamebuild.py
            self.root = os.path.dirname(HERE)
            self.include = os.path.join(self.root, "runtime", "include")
            self.entry = os.path.join(self.root, "runtime", "src", "gamelib_entry.cpp")
        self.recomp = os.path.join(self.root, "recomp")
        self.android_ndk = android_ndk or os.environ.get("NX_ANDROID_NDK")
        self.zig_bin = os.environ.get("NX_ZIG_BIN")
        self.lib_name = f"lib{GAME}_game.so" if (self.android_ndk or self.zig_bin) else LIB
        if self.zig_bin:
            self.cxx = self.zig_bin
            self.cxxflags = ["c++", "-target", "aarch64-linux-android.29", "-O2", "-DNDEBUG", "-std=gnu++20", *WARN,
                             "-fPIC", "-fvisibility=hidden", "-fno-semantic-interposition", "-DANDROID"]
            self.ldflags = ["c++", "-target", "aarch64-linux-android.29", "-shared", "-Wl,-Bsymbolic",
                            "-Wl,--no-undefined", "-Wl,-z,max-page-size=16384"]
        elif self.android_ndk:
            tag = "windows-x86_64" if WIN else "linux-x86_64"
            suffix = ".cmd" if WIN else ""
            self.cxx = os.path.join(self.android_ndk, "toolchains", "llvm", "prebuilt", tag, "bin",
                                    f"aarch64-linux-android29-clang++{suffix}")
            self.cxxflags = ["-O3", "-DNDEBUG", "-std=gnu++20", *WARN, "-march=armv8.2-a+crypto",
                             "-fPIC", "-fvisibility=hidden", "-fno-semantic-interposition", "-DANDROID"]
            self.ldflags = ["-shared", "-static-libstdc++", "-Wl,-Bsymbolic", "-Wl,--no-undefined",
                            "-Wl,-z,max-page-size=16384"]
        else:
            self.cxx = self.find_compiler()
            self.sysroot = os.path.join(HERE, "toolchain", "sysroot")
            extra = []
            if not WIN and os.path.isdir(self.sysroot) and self.cxx and self.cxx.startswith(os.path.join(HERE, "toolchain")):
                extra = [f"--sysroot={self.sysroot}", f"--gcc-toolchain={self.sysroot}/usr"]
            self.cxxflags = CXXFLAGS + extra
            self.ldflags = LDFLAGS + extra + (["-fuse-ld=lld"] if extra else [])

    def find_compiler(self):
        exe = "clang++.exe" if WIN else "clang++"
        for c in (os.environ.get("NX_GAMEBUILD_CXX"), os.path.join(HERE, "toolchain", "bin", exe),
                  os.path.join(os.path.dirname(self.root), "SuperWonders-builder-portable", "tools", "portable", "llvm-mingw", "bin", exe)):
            if c and os.path.isfile(c):
                return c
        return shutil.which(exe) or shutil.which("clang++")

    def files(self):
        """What the generated code's result depends on, besides the input."""
        out = sorted(glob.glob(os.path.join(glob.escape(self.recomp), "*.py")))
        out += [os.path.join(self.include, "nx", h) for h in ("cpu.h", "module.h", "gamelib.h")]
        out.append(self.entry)
        out.append(os.path.abspath(__file__))  # (how it builds: the order it links in, its flags)
        return out

    def compiler_version(self):
        if not self.cxx:
            return "none"
        try:
            out = subprocess.run([self.cxx, "--version"], capture_output=True, text=True, timeout=60).stdout
            return out.splitlines()[0] if out else "none"  # (the version line: the rest names the install folder)
        except OSError:
            return "none"

    def id(self):
        h = hashlib.sha256()
        for f in self.files():
            h.update(os.path.basename(f).encode() + b"\0")
            with open(f, "rb") as fh:
                h.update(fh.read().replace(b"\r\n", b"\n"))  # (a checkout's line endings don't count)
        h.update(" ".join(self.cxxflags + ["|"] + self.ldflags).encode())  # (not the sysroot's path: where the kit is doesn't matter)
        h.update(self.compiler_version().encode())
        h.update(self.lib_name.encode())
        return h.hexdigest()[:32]


def input_id(exefs):
    h = hashlib.sha256()
    for name, _ in MODULES:
        with open(os.path.join(exefs, name), "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
    return h.hexdigest()[:32]


def progress(fraction, what):
    print(f"progress {max(0.0, min(1.0, fraction)):.3f} {what}", flush=True)


def read_npdm_program_id(exefs):
    p = os.path.join(exefs, "main.npdm")
    if not os.path.isfile(p):
        return None
    with open(p, "rb") as f:
        raw = f.read()
    k = raw.find(b"ACI0")
    if k >= 0 and k + 0x18 <= len(raw):
        return struct.unpack_from("<Q", raw, k + 0x10)[0]
    return None


def inspect_exefs(exefs):
    """Identify the Super Mario Bros. Wonder ExeFS version and whether it is supported."""
    info = {
        "version": None,
        "compatible": False,
        "build_id": "",
        "program_id": None,
        "expected_romfs_product": None,
        "error": None,
    }
    for name, _ in MODULES:
        p = os.path.join(exefs, name)
        if not os.path.isfile(p):
            info["error"] = f"error: {p} not found: pick the ExeFS folder of your own copy of the game (it holds main and sdk)"
            return info
        with open(p, "rb") as f:
            hdr = f.read(0x60)
        if len(hdr) < 0x60 or hdr[:4] != b"NSO0":
            info["error"] = f"error: {p} is not an NSO module (an extracted ExeFS has main and sdk as NSO files)"
            return info
        if name == "main":
            info["build_id"] = hdr[0x40:0x54].hex().upper()

    pid = read_npdm_program_id(exefs)
    info["program_id"] = pid
    if pid is not None and pid != WONDER_PROGRAM_ID:
        info["error"] = f"error: ExeFS belongs to title ID {pid:016X}, expected Super Mario Bros. Wonder ({WONDER_PROGRAM_ID:016X})"
        return info

    bid = info["build_id"]
    match = EXEFS_BUILD_IDS.get(bid)
    if match is None:
        match = EXEFS_BUILD_IDS.get(bid[:16])
    if match is not None:
        ver, expected_romfs, compatible = match
        info["version"] = ver
        info["expected_romfs_product"] = expected_romfs
        info["compatible"] = compatible
        if not compatible:
            info["error"] = (
                f"error: ExeFS is Super Mario Bros. Wonder {ver} (Build ID {bid[:16]}), "
                f"which is not supported (supported versions: 1.0.0 and 1.2.1)"
            )
    else:
        info["error"] = (
            f"error: ExeFS has unknown main Build ID {bid[:16]} "
            f"(supported versions: 1.0.0 and 1.2.1)"
        )
    return info


def check_exefs(exefs):
    info = inspect_exefs(exefs)
    if info["error"] or not info["compatible"]:
        raise SystemExit(info["error"] or "error: incompatible ExeFS")
    return info


def default_data_root():
    r = os.environ.get("NX_DATA_ROOT")
    if r:
        return r
    if WIN and os.path.isfile(os.path.join(os.path.dirname(HERE), f"{GAME}.exe")):
        return os.path.join(os.path.dirname(HERE), "data")  # a release: <folder>/wonder.exe, <folder>/data
    raise SystemExit("error: no data root: pass --data-root (or set NX_DATA_ROOT)")


def read_ids(game_dir):
    ids = {}
    try:
        with open(os.path.join(game_dir, "build-id.txt")) as f:
            for line in f:
                k, _, v = line.strip().partition(" ")
                ids[k] = v
    except OSError:
        pass
    return ids


def why_build(kit, exefs, game_dir):
    if not os.path.isfile(os.path.join(game_dir, LIB)):
        return f"no {LIB} yet"
    ids = read_ids(game_dir)
    if ids.get("kit") != kit.id():
        return "this version of SuperWonders builds the game code differently"
    if ids.get("input") != input_id(exefs):
        return "the game files changed"
    return None


def recompile(kit, exefs, gen, done_before, share):
    """python -m recomp for each module, with the recompiler's own progress lines turned into ours."""
    env = dict(os.environ, PYTHONPATH=kit.root + os.pathsep + os.environ.get("PYTHONPATH", ""))
    env.pop("NX_TRACE_SP", None)
    # Apply verified Nextendo NPLN IPS32 guards for Wonder 1.2.1 (010015100b514000) when recompiling main.
    WONDER_121_NPLN_GUARDS = (
        (11545508, bytes.fromhex("C10D0054"), bytes.fromhex("1F2003D5")),
        (11545788, bytes.fromhex("00040035"), bytes.fromhex("1F2003D5")),
        (11548200, bytes.fromhex("AAE24039"), bytes.fromhex("2A008052")),
    )

    def prepare_main_nso(exefs_dir, tmp_dir):
        src_path = os.path.join(exefs_dir, "main")
        with open(src_path, "rb") as f:
            raw = f.read()
        if len(raw) < 0x100 or raw[:4] != b"NSO0":
            return src_path
        bid = raw[0x40:0x54].hex().upper()
        if bid != "FF773E90972D544EB79406EAA65396D53C43EFB9":
            return src_path
        import lz4.block
        flags = struct.unpack_from("<I", raw, 0x0C)[0]
        segs = []
        for i in range(3):
            file_off, mem_off, size = struct.unpack_from("<III", raw, 0x10 + i * 0x10)
            comp = struct.unpack_from("<I", raw, 0x60 + i * 4)[0]
            blob = raw[file_off:file_off + comp]
            if flags & (1 << i):
                blob = lz4.block.decompress(blob, uncompressed_size=size)
            segs.append((mem_off, bytearray(blob)))
        text = segs[0][1]
        for off_nso, orig, patched in WONDER_121_NPLN_GUARDS:
            off = off_nso - 0x100
            if off < 0 or off + 4 > len(text) or bytes(text[off:off + 4]) != orig:
                return src_path
        for off_nso, _, patched in WONDER_121_NPLN_GUARDS:
            off = off_nso - 0x100
            text[off:off + 4] = patched
        hdr = bytearray(raw[:0x100])
        struct.pack_into("<I", hdr, 0x0C, 0)
        cursor = 0x100
        out_blobs = []
        for i, (mem_off, blob) in enumerate(segs):
            struct.pack_into("<III", hdr, 0x10 + i * 0x10, cursor, mem_off, len(blob))
            struct.pack_into("<I", hdr, 0x60 + i * 4, len(blob))
            cursor += len(blob)
            out_blobs.append(bytes(blob))
        patched_path = os.path.join(tmp_dir, "main.npln_patched.nso")
        with open(patched_path, "wb") as f:
            f.write(bytes(hdr))
            for b in out_blobs:
                f.write(b)
        return patched_path

    for k, (name, base) in enumerate(MODULES):
        part = share * (0.85 if name == "main" else 0.15)
        start = done_before + (0 if k == 0 else share * 0.85)
        os.makedirs(gen, exist_ok=True)
        nso_input = prepare_main_nso(exefs, gen) if (GAME == "wonder" and name == "main") else os.path.join(exefs, name)
        # (-u: the recompiler's progress lines as they come, not all at the end)
        cmd = [sys.executable, "-u", "-m", "recomp", nso_input, os.path.join(gen, name),
               "--name", name, "--base", base]
        if GAME == "wonder" and name == "main":
            cmd.append("--wonder-realtime")
        p = subprocess.Popen(cmd, cwd=kit.root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        log = []
        for line in p.stdout:
            log.append(line)
            m = re.search(r"emitted (\d+)/(\d+)", line)
            if m:
                progress(start + part * int(m.group(1)) / max(1, int(m.group(2))), f"Recompiling {name}")
        if p.wait():
            sys.stderr.write("".join(log[-40:]))
            raise SystemExit(f"error: recompiling {name} failed (exit {p.returncode})")
        progress(start + part, f"Recompiled {name}")


def compile_all(kit, gen, obj, jobs, done_before, share):
    units = []
    for name, _ in MODULES:
        for src in sorted(glob.glob(os.path.join(glob.escape(gen), name, "*.cpp"))):
            units.append((src, os.path.join(obj, name, os.path.basename(src) + ".o"),
                          [f"-I{kit.include}", f"-I{os.path.join(gen, name)}"]))
    entry_o = os.path.join(obj, "gamelib_entry.o")
    units.append((kit.entry, entry_o, [f"-I{kit.include}", f'-DNX_GAME_NAME="{GAME}"']))
    for name, _ in MODULES:
        os.makedirs(os.path.join(obj, name), exist_ok=True)

    todo = [u for u in units if not os.path.isfile(u[1]) or os.path.getmtime(u[1]) < os.path.getmtime(u[0])]
    total = len(todo)
    progress(done_before, f"Compiling the game code ({total} of {len(units)} files)")

    def one(u):
        src, out, extra = u
        tmp = out + ".tmp"
        r = subprocess.run([kit.cxx, *kit.cxxflags, *extra, "-c", src, "-o", tmp], capture_output=True, text=True)
        if r.returncode:
            return src, r.stderr
        os.replace(tmp, out)
        return src, None

    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for src, err in pool.map(one, todo):
            if err:
                sys.stderr.write(err[-4000:])
                raise SystemExit(f"error: compiling {os.path.basename(src)} failed")
            done += 1
            if done % 8 == 0 or done == total:
                progress(done_before + share * done / max(1, total), f"Compiling the game code ({done} of {total})")
    return [u[1] for u in units]


def link(kit, objs, game_dir, work):
    tmp = os.path.join(work, kit.lib_name)
    rsp = os.path.join(work, "objects.rsp")
    with open(rsp, "w") as f:
        for o in objs:
            f.write('"' + o.replace("\\", "/") + '"\n')
    cmd = [kit.cxx, *kit.ldflags, "-o", tmp, "@" + rsp]
    if WIN and not kit.android_ndk:
        cmd.append("-Wl,-Map," + os.path.join(work, f"{GAME}_game.map").replace("\\", "/"))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr[-4000:])
        raise SystemExit("error: linking the game code failed")
    dest = os.path.join(game_dir, kit.lib_name)
    try:
        os.replace(tmp, dest)
    except PermissionError:
        raise SystemExit(f"error: {dest} is in use: close the game first")
    if WIN and not kit.android_ndk and os.path.isfile(os.path.join(work, f"{GAME}_game.map")):
        os.replace(os.path.join(work, f"{GAME}_game.map"), os.path.join(game_dir, f"{GAME}_game.map"))


def build(a):
    kit = Kit(getattr(a, "android_ndk", None))
    if not kit.cxx:
        raise SystemExit("error: no C++ compiler (clang++) found: this build of SuperWonders is missing game-build/toolchain")
    exefs = os.path.abspath(a.exefs)
    info = check_exefs(exefs)
    game_dir = os.path.join(os.path.abspath(a.data_root or default_data_root()), "game")
    reason = why_build(kit, exefs, game_dir)
    if not reason and not a.force:
        progress(1.0, "The game code is up to date")
        return 0
    print(f"building the game code (Wonder {info['version']}): {reason or 'forced'}", flush=True)
    t0 = time.time()
    work = os.path.join(game_dir, "work")
    gen, obj = os.path.join(work, "gen"), os.path.join(work, "obj")
    os.makedirs(work, exist_ok=True)
    # Objects from another kit (other headers, flags or compiler) or another game version can't be reused.
    stamp = os.path.join(work, "kit-id.txt")
    inp_stamp = os.path.join(work, "input-id.txt")
    kit_id = kit.id()
    inp_id = input_id(exefs)
    try:
        old = open(stamp).read().strip()
    except OSError:
        old = ""
    try:
        old_inp = open(inp_stamp).read().strip()
    except OSError:
        old_inp = ""
    if old_inp != inp_id:
        shutil.rmtree(gen, ignore_errors=True)
        shutil.rmtree(obj, ignore_errors=True)
    elif old != kit_id:
        shutil.rmtree(obj, ignore_errors=True)
    with open(stamp, "w") as f:
        f.write(kit_id + "\n")
    with open(inp_stamp, "w") as f:
        f.write(inp_id + "\n")

    recompile(kit, exefs, gen, 0.0, 0.25)
    objs = compile_all(kit, gen, obj, a.jobs, 0.25, 0.70)
    progress(0.96, "Linking the game code")
    link(kit, objs, game_dir, work)
    with open(os.path.join(game_dir, "build-id.txt"), "w") as f:
        f.write(f"kit {kit_id}\ninput {inp_id}\nversion {info['version']}\n")
    if not a.keep_work:
        shutil.rmtree(work, ignore_errors=True)
    progress(1.0, f"The game code is built ({time.time() - t0:.0f} s)")
    return 0


def check(a):
    kit = Kit(getattr(a, "android_ndk", None))
    exefs = os.path.abspath(a.exefs)
    check_exefs(exefs)
    game_dir = os.path.join(os.path.abspath(a.data_root or default_data_root()), "game")
    reason = why_build(kit, exefs, game_dir)
    print(reason or "up to date")
    return EXIT_NEEDS_BUILD if reason else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("build", "check"):
        p = sub.add_parser(name)
        p.add_argument("--exefs", required=True, help="the ExeFS folder of your own copy (main, sdk)")
        p.add_argument("--data-root", help="where the game's data lives (default: NX_DATA_ROOT, or the release's data/)")
        p.add_argument("--android-ndk", help="cross-compile libwonder_game.so for Android arm64-v8a using this NDK")
        if name == "build":
            p.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
            p.add_argument("--keep-work", action="store_true",
                           help="keep the generated code and objects in <data root>/game/work: a later build only "
                                "recompiles what changed")
            p.add_argument("--force", action="store_true", help="build even when up to date")
    sub.add_parser("id")
    a = ap.parse_args()
    if a.cmd == "id":
        print(Kit().id())
        return 0
    return build(a) if a.cmd == "build" else check(a)


if __name__ == "__main__":
    sys.exit(main())
