#!/usr/bin/env python3
"""Package the Android on-device game-build kit into private/android-kit/bundle/.

Produces:
  private/android-kit/bundle/lib_arm64/lib{zig,py3,ninja,python3.14[,spirv_harness]}.so
  private/android-kit/bundle/assets/game-build.tar.gz
"""
import hashlib
import os
import shutil
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
KIT = os.path.join(ROOT, "private", "android-kit")
BUNDLE = os.path.join(KIT, "bundle")
LIB_OUT = os.path.join(BUNDLE, "lib_arm64")
ASSETS_OUT = os.path.join(BUNDLE, "assets")

ZIG_DIR = os.path.join(KIT, "zig-linux-aarch64-0.14.0")
TERMUX_USR = os.path.join(KIT, "termux_root", "data", "data", "com.termux", "files", "usr")
NDK_SYS = r"C:\Android\sdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr"
PC_SITE = os.path.join(os.path.dirname(ROOT), "SuperWonders-release-0.7.0", "SuperWonders", "game-build", "python", "Lib", "site-packages")


def kit_id():
    h = hashlib.sha256()
    for d in ("recomp", os.path.join("runtime", "include"), os.path.join("tools", "maxwell")):
        base = os.path.join(ROOT, d)
        for cur, dirs, files in os.walk(base):
            dirs[:] = sorted(x for x in dirs if x != "__pycache__")
            for fn in sorted(files):
                p = os.path.join(cur, fn)
                h.update(os.path.relpath(p, ROOT).replace("\\", "/").encode() + b"\0")
                h.update(open(p, "rb").read())
    for fn in ("tools/setup_game.py", "tools/gamebuild.py", "runtime/src/gamelib_entry.cpp"):
        p = os.path.join(ROOT, fn)
        h.update(fn.encode() + b"\0")
        h.update(open(p, "rb").read())
    return h.hexdigest()[:16]


def main():
    if os.path.isdir(BUNDLE):
        shutil.rmtree(BUNDLE)
    os.makedirs(LIB_OUT, exist_ok=True)
    os.makedirs(ASSETS_OUT, exist_ok=True)

    # 1. Native executables + libpython3.14.so in lib/arm64-v8a/
    bins = {
        "libzig.so": os.path.join(ZIG_DIR, "zig"),
        "libpy3.so": os.path.join(TERMUX_USR, "bin", "python3.14"),
        "libninja.so": os.path.join(TERMUX_USR, "bin", "ninja"),
        "libpython3.14.so": os.path.join(TERMUX_USR, "lib", "libpython3.14.so"),
    }
    harness_aot = os.path.join(KIT, "spirv_harness_aot", "spirv_harness")
    if os.path.isfile(harness_aot):
        bins["libspirv_harness.so"] = harness_aot

    for dst_name, src in bins.items():
        if not os.path.isfile(src):
            raise SystemExit(f"Missing binary: {src}")
        shutil.copyfile(src, os.path.join(LIB_OUT, dst_name))

    # 2. Build assets/game-build.tar.gz
    tar_path = os.path.join(ASSETS_OUT, "game-build.tar.gz")
    kid = kit_id()

    def add_tree(tf, src_dir, arc_prefix, skip_names=("__pycache__",)):
        for cur, dirs, files in os.walk(src_dir):
            dirs[:] = sorted(x for x in dirs if x not in skip_names)
            for fn in sorted(files):
                if fn.endswith(".pyc"):
                    continue
                full = os.path.join(cur, fn)
                rel = os.path.relpath(full, src_dir).replace("\\", "/")
                ti = tf.gettarinfo(full, arcname=f"{arc_prefix}/{rel}")
                ti.mode = 0o644
                with open(full, "rb") as f:
                    tf.addfile(ti, f)

    def add_bytes(tf, arcname, data, mode=0o644):
        import io
        ti = tarfile.TarInfo(arcname)
        ti.size = len(data)
        ti.mode = mode
        tf.addfile(ti, io.BytesIO(data))

    def add_file(tf, src, arcname, mode=0o644):
        ti = tf.gettarinfo(src, arcname=arcname)
        ti.mode = mode
        with open(src, "rb") as f:
            tf.addfile(ti, f)

    with tarfile.open(tar_path, "w:gz", format=tarfile.GNU_FORMAT, compresslevel=6) as tf:
        add_bytes(tf, "kit-id.txt", (kid + "\n").encode())
        add_file(tf, os.path.join(ROOT, "tools", "setup_game.py"), "setup_game.py")
        add_file(tf, os.path.join(ROOT, "tools", "gamebuild.py"), "gamebuild.py")
        add_file(tf, os.path.join(ROOT, "runtime", "src", "gamelib_entry.cpp"), "gamelib_entry.cpp")
        add_tree(tf, os.path.join(ROOT, "runtime", "include"), "include")
        add_tree(tf, os.path.join(ROOT, "recomp"), "recomp")
        for sub in ("fonts", "maxwell"):
            add_tree(tf, os.path.join(ROOT, "tools", sub), f"tools/{sub}")
        for fn in ("nso2bin.py", "wonder_menu_mod.py", "wonder_uw_map_hud.py", "wonder_uw_prepare.py",
                   "wonder_uw_screen_backgrounds.py"):
            p = os.path.join(ROOT, "tools", fn)
            if os.path.isfile(p):
                add_file(tf, p, f"tools/{fn}")
        add_bytes(tf, "tools/__init__.py", b"")

        # Python 3.14 stdlib + site-packages
        add_tree(tf, os.path.join(TERMUX_USR, "lib", "python3.14"), "python/lib/python3.14",
                 skip_names=("__pycache__", "test", "idlelib", "tkinter", "turtledemo", "ensurepip"))
        if os.path.isdir(os.path.join(PC_SITE, "capstone")):
            add_tree(tf, os.path.join(PC_SITE, "capstone"), "python/lib/python3.14/site-packages/capstone")
        if os.path.isdir(os.path.join(PC_SITE, "fontTools")):
            add_tree(tf, os.path.join(PC_SITE, "fontTools"), "python/lib/python3.14/site-packages/fontTools")
        add_tree(tf, os.path.join(HERE, "kit_shims"), "python/lib/python3.14/site-packages")

        # Shared libraries (SONAMEs)
        so_map = {
            "libz.so.1": "libz.so.1.4",
            "liblzma.so.5": "liblzma.so.5.8.4",
            "libbz2.so.1.0": "libbz2.so.1.0.8",
            "libexpat.so.1": "libexpat.so.1.13.1",
            "libffi.so": "libffi.so",
            "libcrypt.so": "libcrypt.so",
            "libcrypto.so.3": "libcrypto.so.3",
            "libssl.so.3": "libssl.so.3",
            "libandroid-support.so": "libandroid-support.so",
            "libandroid-posix-semaphore.so": "libandroid-posix-semaphore.so",
            "libandroid-spawn.so": "libandroid-spawn.so",
            "libcapstone.so.5": "libcapstone.so",
            "libcapstone.so": "libcapstone.so",
            "liblz4.so": "liblz4.so",
            "liblz4.so.1": "liblz4.so",
            "libzstd.so.1": "libzstd.so.1.5.7",
        }
        for dst_so, src_so in so_map.items():
            p = os.path.join(TERMUX_USR, "lib", src_so)
            if os.path.isfile(p):
                add_file(tf, p, f"lib/{dst_so}", mode=0o755)

        # Zig compiler support files + NDK Android 29 sysroot
        zlib = os.path.join(ZIG_DIR, "lib")
        for d in ("compiler", "compiler_rt", "include", "libcxx", "libcxxabi", "libunwind", "std"):
            add_tree(tf, os.path.join(zlib, d), f"ziglib/{d}")
        for fn in ("c.zig", "compiler_rt.zig", "fuzzer.zig", "ubsan_rt.zig", "zig.h"):
            add_file(tf, os.path.join(zlib, fn), f"ziglib/{fn}")
        add_tree(tf, os.path.join(NDK_SYS, "include"), "sysroot/usr/include")
        crt = os.path.join(NDK_SYS, "lib", "aarch64-linux-android", "29")
        for fn in os.listdir(crt):
            if fn.endswith(".so") or fn.endswith(".o"):
                add_file(tf, os.path.join(crt, fn), f"sysroot/usr/lib/aarch64-linux-android/29/{fn}")

    mb = os.path.getsize(tar_path) / (1024 * 1024)
    print(f"Packed {tar_path} ({mb:.1f} MB), kit-id={kid}")


if __name__ == "__main__":
    main()
