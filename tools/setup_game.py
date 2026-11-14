#!/usr/bin/env python3
"""Sets up the game from the player's own files: everything the program needs that it can't ship, in the data root.
What SuperWonders Launcher's "Set up game files" runs, and what WonderBuilder's "Build game" did.

    setup_game.py setup --exefs <dir> --romfs <dir> [--data-root DIR] [--romfs-mode copy|link] [--jobs N]
    setup_game.py check --exefs <dir> --romfs <dir> [--data-root DIR]   exit 0: ready, 10: needs setup (says why)

Steps, each skipped when its inputs and tools haven't changed since it last finished (<data root>/setup-state.txt):
    1. analysis   ExeFS main and sdk as flat images (tools/nso2bin.py)        -> analysis/main.bin, sdk.bin
    2. romfs      the RomFS, copied (or linked: --romfs-mode link)            -> extract/romfs/
    3. shaders    the game's shaders found and translated to SPIR-V           -> spirv/, shader_programs.bin
                  (tools/maxwell/: extract_romfs_shaders.py, exefs_shaders.py, then the translator, spirv_harness)
    4. font       the controller-button font, drawn over a system font        -> fonts/shared_5.ttf
    5. menu       "Quit Game" in the pause menu and on the title screen, as a RomFS mod (tools/wonder_menu_mod.py)
                                                                              -> mods/recomp-menu/romfs/
    6. code       the game code library (tools/gamebuild.py)                  -> game/wonder_game.dll, build-id.txt
The work files of steps 3 and 5 hold the player's game and are removed when the step finishes.

Progress: "progress <0..1> <what>" lines for the launcher (RecompLauncher's prepare protocol); exit 0 when it worked.
Runs from a checkout or from a release's game-build/ folder (the same tools/ and recomp/ layout, a Python, a compiler
and the translator: tools/pack_gamebuild_kit.py).
"""
import argparse
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gamebuild  # noqa: E402  (same folder in a checkout's tools/ and a release's game-build/)

ROOT = gamebuild.Kit().root  # the checkout, or game-build/
TOOLS = os.path.join(ROOT, "tools") if os.path.isdir(os.path.join(ROOT, "tools")) else HERE
WIN = os.name == "nt"
EXIT_NEEDS_SETUP = 10
# (step, share of the progress bar)
STEPS = (("analysis", 0.02), ("romfs", 0.13), ("shaders", 0.30), ("font", 0.01), ("menu", 0.01), ("code", 0.53))
MENU_MOD = "recomp-menu"  # <data root>/mods/<this>: the name games/wonder/menu.cpp's docs (docs/MENU.md) use


def say(fraction, what):
    gamebuild.progress(fraction, what)


def h_files(paths, extra=b""):
    h = hashlib.sha256(extra)
    for p in paths:
        h.update(os.path.basename(p).encode() + b"\0")
        with open(p, "rb") as f:
            h.update(f.read().replace(b"\r\n", b"\n"))
    return h.hexdigest()[:32]


def romfs_fingerprint(romfs):
    """Names and sizes of every file: enough to tell a different RomFS (a copy can't be told by its dates)."""
    h = hashlib.sha256()
    n = total = 0
    for base, dirs, files in os.walk(romfs):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(base, f)
            s = os.path.getsize(p)
            h.update(f"{os.path.relpath(p, romfs)}\0{s}\n".replace("\\", "/").encode())
            n, total = n + 1, total + s
    return h.hexdigest()[:32], n, total


def system_font():
    for p in ([os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", f) for f in ("arial.ttf", "segoeui.ttf")]
              if WIN else ["/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf"]):
        if os.path.isfile(p):
            return p
    return None


def harness_command():
    """The shader translator: a release's own build, NX_SPIRV_HARNESS, or a checkout's (docs/nvn/TRANSLATOR.md)."""
    exe = "spirv_harness.exe" if WIN else "spirv_harness"
    for p in (os.environ.get("NX_SPIRV_HARNESS"), os.path.join(ROOT, "translator", exe)):
        if p and os.path.isfile(p):
            if not WIN and not p.endswith(".dll") and not os.access(p, os.X_OK):
                try:  # (copied without its executable bit; inside a read-only AppImage the packer has set it)
                    os.chmod(p, os.stat(p).st_mode | 0o755)
                except OSError:
                    pass
            return [p] if not p.endswith(".dll") else ["dotnet", p]
    dll = os.path.join(ROOT, "private", "build", "spirv_harness", "bin", "Release", "net10.0", "spirv_harness.dll")
    if os.path.isfile(dll) and shutil.which("dotnet"):
        return ["dotnet", dll]
    return None


class Setup:
    def __init__(self, a):
        self.exefs = os.path.abspath(a.exefs)
        self.romfs = os.path.abspath(a.romfs)
        self.data = os.path.abspath(a.data_root or gamebuild.default_data_root())
        self.state_path = os.path.join(self.data, "setup-state.txt")
        self.mode = getattr(a, "romfs_mode", "copy")
        self.jobs = getattr(a, "jobs", max(1, (os.cpu_count() or 2) - 1))
        self.py = sys.executable
        self.kit = gamebuild.Kit()

    # ---- what each step depends on ----
    def check_inputs(self):
        gamebuild.check_exefs(self.exefs)
        if not os.path.isdir(self.romfs):
            raise SystemExit(f"error: {self.romfs} not found: pick the RomFS folder of your own copy of the game")
        if not glob.glob(os.path.join(self.romfs, "Shader", "*.zs")):
            raise SystemExit(f"error: {self.romfs} doesn't look like Super Mario Bros. Wonder's RomFS (no Shader/*.zs)")

    def ids(self):
        if not hasattr(self, "_ids"):
            exe_id = gamebuild.input_id(self.exefs)
            rom_id, self.rom_files, self.rom_bytes = romfs_fingerprint(self.romfs)
            maxwell = sorted(glob.glob(os.path.join(TOOLS, "maxwell", "*.py")))
            harness = harness_command()
            font = system_font() or ""
            self._ids = {
                "analysis": h_files([os.path.join(TOOLS, "nso2bin.py")], exe_id.encode()),
                "romfs": rom_id,
                "shaders": h_files(maxwell, (exe_id + rom_id + " ".join(harness or [])).encode()),
                "font": h_files([os.path.join(TOOLS, "fonts", "make_button_font.py")], font.encode()),
                "menu": h_files([os.path.join(TOOLS, "wonder_menu_mod.py")], rom_id.encode()),
            }
        return self._ids

    def state(self):
        s = {}
        try:
            with open(self.state_path) as f:
                for line in f:
                    k, _, v = line.strip().partition(" ")
                    s[k] = v
        except OSError:
            pass
        return s

    def done(self, step, sid):
        s = self.state()
        s[step] = sid
        os.makedirs(self.data, exist_ok=True)
        tmp = self.state_path + ".tmp"
        with open(tmp, "w") as f:
            f.writelines(f"{k} {v}\n" for k, v in s.items())
        os.replace(tmp, self.state_path)

    def needed(self):
        """[(step, why)] still to do."""
        s, ids, out = self.state(), self.ids(), []
        names = {"analysis": "the game's executable", "romfs": "the game's files", "shaders": "the shaders",
                 "font": "the button font", "menu": "Quit Game in the game's menus"}
        for step, _ in STEPS:
            if step == "code":
                why = gamebuild.why_build(self.kit, self.exefs, os.path.join(self.data, "game"))
                if why:
                    out.append((step, why))
            elif s.get(step) != ids[step]:
                out.append((step, f"{names[step]}: not set up yet" if step not in s else f"{names[step]}: changed"))
        return out

    # ---- the steps ----
    def run(self, cmd, at, share, what, pattern=None, total=None, cwd=None):
        """A tool, with its progress lines ("12/345" by pattern) turned into ours."""
        env = dict(os.environ, PYTHONPATH=ROOT + os.pathsep + os.environ.get("PYTHONPATH", ""), PYTHONUNBUFFERED="1")
        p = subprocess.Popen(cmd, cwd=cwd or ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, errors="replace")
        tail = []
        for line in p.stdout:
            tail = (tail + [line])[-40:]
            if pattern:
                m = re.search(pattern, line)
                if m:
                    n, t = int(m.group(1)), int(m.group(2)) if m.lastindex >= 2 else (total or 1)
                    say(at + share * min(1.0, n / max(1, t)), what)
        if p.wait():
            sys.stderr.write("".join(tail))
            raise SystemExit(f"error: {what} failed (exit {p.returncode})")

    def step_analysis(self, at, share):
        out = os.path.join(self.data, "analysis")
        os.makedirs(out, exist_ok=True)
        for k, name in enumerate(("main", "sdk")):
            say(at + share * k / 2, "Reading the game's executable")
            self.run([self.py, os.path.join(TOOLS, "nso2bin.py"), os.path.join(self.exefs, name),
                      os.path.join(out, name + ".bin")], at, share, "Reading the game's executable")

    def step_romfs(self, at, share):
        dest = os.path.join(self.data, "extract", "romfs")
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if os.path.islink(dest) or (WIN and os.path.isdir(dest) and is_junction(dest)):
            remove_link(dest)
        if self.mode == "link":
            if os.path.isdir(dest):
                shutil.rmtree(dest)
            make_link(self.romfs, dest)
            say(at + share, "Linked the game's files")
            return
        copied, last = 0, 0.0
        for base, dirs, files in os.walk(self.romfs):
            rel = os.path.relpath(base, self.romfs)
            os.makedirs(os.path.join(dest, rel), exist_ok=True)
            for f in files:
                s, d = os.path.join(base, f), os.path.join(dest, rel, f)
                size = os.path.getsize(s)
                if not (os.path.isfile(d) and os.path.getsize(d) == size):
                    shutil.copyfile(s, d)
                copied += size
                now = time.time()
                if now - last > 0.25:
                    last = now
                    say(at + share * copied / max(1, self.rom_bytes),
                        f"Copying the game's files ({copied >> 20} of {self.rom_bytes >> 20} MB)")

    def step_shaders(self, at, share):
        work = os.path.join(self.data, "shaders_extracted")
        shutil.rmtree(work, ignore_errors=True)
        maxwell = os.path.join(TOOLS, "maxwell")
        say(at, "Finding the game's shaders")
        self.run([self.py, os.path.join(maxwell, "extract_romfs_shaders.py"), self.romfs, work], at, share * 0.3,
                 "Finding the game's shaders")
        self.run([self.py, os.path.join(maxwell, "exefs_shaders.py"), os.path.join(self.data, "analysis", "main.bin"),
                  "--extract", os.path.join(work, "exefs")], at, share * 0.3, "Finding the game's shaders")
        harness = harness_command()
        if not harness:
            raise SystemExit("error: no shader translator (game-build/translator) in this build of SuperWonders")
        spirv = os.path.join(self.data, "spirv")
        say(at + share * 0.3, "Translating the shaders")
        self.run(harness + ["--batch", work, "--out", spirv, "--jobs", str(self.jobs), "--no-val"],
                 at + share * 0.3, share * 0.68, "Translating the shaders", pattern=r"^\s*(\d+)/(\d+)\s")
        shutil.copyfile(os.path.join(work, "shader_programs.bin"), os.path.join(self.data, "shader_programs.bin"))
        shutil.rmtree(work, ignore_errors=True)  # (the game's shader code)

    def step_font(self, at, share):
        base = system_font()
        if not base:
            raise SystemExit("error: no system font to draw the button font over (install DejaVu Sans or Noto Sans)")
        out = os.path.join(self.data, "fonts", "shared_5.ttf")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        self.run([self.py, os.path.join(TOOLS, "fonts", "make_button_font.py"), "--base", base, "--out", out],
                 at, share, "Drawing the button font")

    def step_menu(self, at, share):
        # Without this mod games/wonder/menu.cpp has no pane to act on: the pause menu has no Quit Game (the title
        # screen's hold-Minus still works). Not fatal: a RomFS the tool doesn't know only loses the entry.
        out = os.path.join(self.data, "mods", MENU_MOD)
        try:
            self.run([self.py, os.path.join(TOOLS, "wonder_menu_mod.py"), "--romfs", self.romfs, "--out", out],
                     at, share, "Adding Quit Game to the game's menus")
        except SystemExit as e:
            print(f"warning: {e}; the game runs without Quit Game in its pause menu", flush=True)
            return False
        return True

    def step_code(self, at, share):
        cmd = [self.py, "-u", os.path.join(HERE, "gamebuild.py"), "build", "--exefs", self.exefs, "--data-root",
               self.data, "--jobs", str(self.jobs)]
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        tail = []
        for line in p.stdout:
            tail = (tail + [line])[-40:]
            m = re.match(r"progress ([\d.]+) (.*)", line)
            if m:
                say(at + share * float(m.group(1)), m.group(2).strip())
        if p.wait():
            sys.stderr.write("".join(tail))
            raise SystemExit("error: building the game code failed")

    def setup(self):
        self.check_inputs()
        say(0.0, "Checking your game files")
        todo = dict(self.needed())
        t0, at = time.time(), 0.0
        for step, share in STEPS:
            if step in todo:
                print(f"{step}: {todo[step]}", flush=True)
                ok = getattr(self, "step_" + step)(at, share)
                if step != "code" and ok is not False:
                    self.done(step, self.ids()[step])
            at += share
            say(at, "Set up" if at >= 1 else "Setting up")
        say(1.0, f"Your game is set up ({time.time() - t0:.0f} s)")
        return 0


def is_junction(p):
    try:
        return bool(os.readlink(p))
    except OSError:
        return False


def make_link(src, dest):
    if WIN:  # a directory junction: no administrator rights or developer mode needed, unlike a symbolic link
        r = subprocess.run(["cmd", "/c", "mklink", "/J", dest, src], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"error: couldn't link {dest} to {src}: {r.stdout}{r.stderr}")
    else:
        os.symlink(src, dest)


def remove_link(p):
    if WIN:
        os.rmdir(p)  # removes the junction, not what it points to
    else:
        os.unlink(p)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("setup", "check"):
        p = sub.add_parser(name)
        p.add_argument("--exefs", required=True)
        p.add_argument("--romfs", required=True)
        p.add_argument("--data-root")
        if name == "setup":
            p.add_argument("--romfs-mode", choices=("copy", "link"), default="copy")
            p.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    a = ap.parse_args()
    s = Setup(a)
    if a.cmd == "setup":
        return s.setup()
    s.check_inputs()
    need = s.needed()
    for step, why in need:
        print(f"{step}: {why}")
    if not need:
        print("ready")
    return EXIT_NEEDS_SETUP if need else 0


if __name__ == "__main__":
    sys.exit(main())
