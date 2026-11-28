#!/usr/bin/env python3
"""Builds the recomp's own entries into Wonder's menus as a RomFS mod (docs/MENU.md).

"Quit Game" in two places, each the game's own look, with its label in every language:
- the pause menu in a course, under "Exit Course": a copy of the L_BtnExit part (same look and animations) that
  reports the new PauseResult value QuitToDesktop;
- the title screen: a "(-) Quit Game" line in the corner. The title has no buttons (it waits for L + R), so Minus
  there quits, and the line says so.
games/wonder/menu.cpp acts on both. (The world map's menu is left alone: at 1.0.0 its cursor never stops on an
added button; docs/MENU.md.)

Reads the RomFS (the game's own, 1.0.0 or later) and writes <out>/romfs/:
  Layout/Pause.Nin_NX_NVN.blarc.zs    the pause layout with the L_QuitBtn pane
  Layout/Title.Nin_NX_NVN.blarc.zs    the title layout with the T_QuitGame line
  Pack/Bootup.Nin_NX_NVN.pack.zs      the pause screen's button table with the QuitToDesktop button
  Mals/<lang>.Product.<v>.sarc.zs     the labels (LayoutMsgRef/PauseRef, TitleRef)
  System/Resource/ResourceSizeTable.Product.<v>.rsizetable.zs   those files' sizes (they grew)
Run it again after the RomFS changes (a game update). Uses the zstandard module, or the system's libzstd.

Usage: python tools/wonder_menu_mod.py [--romfs <dir>] [--out <mod dir>]
"""
import argparse
import os
import re
import struct
import glob
import zlib

try:
    import zstandard
except ImportError:  # the system's libzstd through ctypes (Linux, MSYS2/Git for Windows)
    zstandard = None

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

QUIT_PANE = "L_QuitBtn"
QUIT_ENUM = "QuitToDesktop"  # the PauseResult name games/wonder/menu.cpp acts on
QUIT_Y = -265.0  # one button step (130) under L_BtnExit
ICON_ROW_Y = -405.0  # the round buttons (Settings, Control Guide...) move down to make room: from -355
ICON_PANES = ("L_PlayerChangeBtn", "L_OptionBtn", "L_HelpBtn", "L_AmiiboBtn")
# The title screen: a text line (a copy of "Press L + R to Start") in the bottom right corner, placed relative to
# T_PressStart (at 0, -418 on the screen).
# The line under each of the two "Press L + R to Start" panes (the game shows one of them, by region).
TITLE_PANES = {"T_PressStart": "T_QuitGame", "T_PressStart_USEU": "T_QuitGameUS"}
TITLE_POS, TITLE_SCALE = (700.0, -62.0), 0.75
MINUS = "\ue0f0"  # the Minus button's glyph in the game's font (GameMsg/Key.msbt "Minus")
QUIT_GLYPH = "\ue0f8"  # remapped in shared_5.ttf by runtime/src/ttf_cmap_remap.cpp to the configured quit button
QUIT_TEXT = {
    "USen": "Quit Game", "EUen": "Quit Game", "USfr": "Quitter le jeu", "EUfr": "Quitter le jeu",
    "USes": "Salir del juego", "EUes": "Salir del juego", "EUde": "Spiel beenden", "EUit": "Esci dal gioco",
    "EUnl": "Spel afsluiten", "EUpt": "Sair do jogo", "USpt": "Sair do jogo", "EUru": "Выйти из игры",
    "EUpl": "Zakończ grę", "JPja": "ゲームを終了", "KRko": "게임 종료", "CNzh": "退出游戏", "TWzh": "結束遊戲",
    "THth": "ออกจากเกม",
}
TITLE_QUIT_TEXT = {
    "USen": f"Hold {QUIT_GLYPH} to quit", "EUen": f"Hold {QUIT_GLYPH} to quit",
    "USfr": f"Maintenir {QUIT_GLYPH} pour quitter", "EUfr": f"Maintenir {QUIT_GLYPH} pour quitter",
    "USes": f"Mantén {QUIT_GLYPH} para salir", "EUes": f"Mantén {QUIT_GLYPH} para salir",
    "EUde": f"{QUIT_GLYPH} gedrückt halten zum Beenden", "EUit": f"Tieni premuto {QUIT_GLYPH} per uscire",
    "EUnl": f"Houd {QUIT_GLYPH} ingedrukt om te stoppen",
    "EUpt": f"Segure {QUIT_GLYPH} para sair", "USpt": f"Segure {QUIT_GLYPH} para sair",
    "EUru": f"Удерживайте {QUIT_GLYPH} для выхода", "EUpl": f"Przytrzymaj {QUIT_GLYPH}, aby wyjść",
    "JPja": f"{QUIT_GLYPH}長押しで終了", "KRko": f"{QUIT_GLYPH} 길게 눌러 종료",
    "CNzh": f"按住 {QUIT_GLYPH} 退出", "TWzh": f"按住 {QUIT_GLYPH} 結束",
    "THth": f"กด {QUIT_GLYPH} ค้างไว้เพื่อออก",
}


# ---- zstd + SARC -------------------------------------------------------------------------------------------------

def _libzstd():
    import ctypes
    import ctypes.util
    names = [ctypes.util.find_library("zstd"), "libzstd.so.1", "libzstd.dylib", "libzstd.dll",
             r"C:\Program Files\Git\ucrt64\bin\libzstd.dll", r"C:\msys64\ucrt64\bin\libzstd.dll"]
    for n in names:
        try:
            z = ctypes.CDLL(n) if n else None
        except OSError:
            continue
        if z:
            z.ZSTD_getFrameContentSize.restype = ctypes.c_ulonglong
            for f in ("ZSTD_decompress", "ZSTD_compress", "ZSTD_compressBound"):
                getattr(z, f).restype = ctypes.c_size_t
            z.ZSTD_isError.restype = ctypes.c_uint
            return ctypes, z
    raise SystemExit("needs zstd: pip install zstandard (or a system libzstd)")


def unzs(path):
    src = open(path, "rb").read()
    if zstandard:
        return zstandard.ZstdDecompressor().decompress(src, max_output_size=1 << 31)
    ctypes, z = _libzstd()
    n = z.ZSTD_getFrameContentSize(src, ctypes.c_size_t(len(src)))
    assert n < 1 << 31, f"{path}: no content size in the zstd frame"
    dst = ctypes.create_string_buffer(n)
    r = z.ZSTD_decompress(dst, ctypes.c_size_t(n), src, ctypes.c_size_t(len(src)))
    assert not z.ZSTD_isError(r) and r == n, f"{path}: zstd error"
    return dst.raw


def zs(data):
    if zstandard:
        return zstandard.ZstdCompressor(level=19).compress(data)
    ctypes, z = _libzstd()
    cap = z.ZSTD_compressBound(ctypes.c_size_t(len(data)))
    dst = ctypes.create_string_buffer(cap)
    r = z.ZSTD_compress(dst, ctypes.c_size_t(cap), data, ctypes.c_size_t(len(data)), 19)
    assert not z.ZSTD_isError(r), "zstd error"
    return dst.raw[:r]


def sarc_nodes(d):
    """[(name, node offset, data start, data end)]; data offsets are absolute."""
    assert d[:4] == b"SARC" and d[6:8] == b"\xff\xfe", "not a little-endian SARC"
    data = struct.unpack_from("<I", d, 0xC)[0]
    n = struct.unpack_from("<H", d, 0x1A)[0]
    names = 0x20 + n * 0x10 + 8
    out = []
    for k in range(n):
        node = 0x20 + k * 0x10
        _, attr, start, end = struct.unpack_from("<IIII", d, node)
        o = names + (attr & 0xFFFF) * 4
        out.append((d[o:d.index(b"\0", o)].decode(), node, data + start, data + end))
    return out


def sarc_get(d, name):
    for n, _, s, e in sarc_nodes(d):
        if n == name:
            return d[s:e]
    raise KeyError(name)


def sarc_replace(d, files):
    """Each replaced file's new bytes go at the end (0x2000-aligned); every other file keeps its offset, so
    alignment the game relies on is untouched. The old copies stay as unused bytes."""
    out = bytearray(d)
    data = struct.unpack_from("<I", d, 0xC)[0]
    for name, _, _, _ in sarc_nodes(d):
        if name in files:
            node = next(o for n, o, _, _ in sarc_nodes(bytes(out)) if n == name)
            out += b"\0" * (-len(out) % 0x2000)
            start = len(out)
            out += files[name]
            struct.pack_into("<II", out, node + 8, start - data, len(out) - data)
    missing = set(files) - {n for n, _, _, _ in sarc_nodes(d)}
    assert not missing, f"not in the archive: {missing}"
    struct.pack_into("<I", out, 0x8, len(out))
    return bytes(out)


# ---- BYML (version 7, what Wonder's gyml use) -------------------------------------------------------------------

class U32(int):
    pass


def byml_read(d):
    assert d[:2] == b"YB", "not a little-endian BYML"
    kt, st, root = struct.unpack_from("<III", d, 4)

    def table(o):
        if not o:
            return []
        n = struct.unpack_from("<I", d, o)[0] >> 8
        offs = struct.unpack_from(f"<{n + 1}I", d, o + 4)
        return [d[o + x:d.index(b"\0", o + x)].decode() for x in offs[:-1]]

    keys, strs = table(kt), table(st)

    def val(t, v):
        if t == 0xA0:
            return strs[v]
        if t in (0xC0, 0xC1):
            return node(v)
        if t == 0xD0:
            return bool(v)
        if t == 0xD1:
            return struct.unpack("<i", struct.pack("<I", v))[0]
        if t == 0xD2:
            return struct.unpack("<f", struct.pack("<I", v))[0]
        if t == 0xD3:
            return U32(v)
        raise ValueError(f"BYML node type {t:#x} not handled")

    def node(o):
        t, n = d[o], struct.unpack_from("<I", d, o)[0] >> 8
        if t == 0xC0:
            types = d[o + 4:o + 4 + n]
            vals = o + 4 + ((n + 3) & ~3)
            return [val(types[i], struct.unpack_from("<I", d, vals + 4 * i)[0]) for i in range(n)]
        if t == 0xC1:
            r = {}
            for i in range(n):
                e = o + 4 + 8 * i
                k = struct.unpack_from("<I", d, e)[0] & 0xFFFFFF
                r[keys[k]] = val(d[e + 3], struct.unpack_from("<I", d, e + 4)[0])
            return r
        raise ValueError(f"BYML node type {t:#x} not handled")

    return struct.unpack_from("<H", d, 2)[0], node(root)


def byml_write(version, root):
    keys, strs = set(), set()

    def collect(v):
        if isinstance(v, dict):
            keys.update(v)
            for x in v.values():
                collect(x)
        elif isinstance(v, list):
            for x in v:
                collect(x)
        elif isinstance(v, str):
            strs.add(v)

    collect(root)
    keys, strs = sorted(keys, key=str.encode), sorted(strs, key=str.encode)
    kidx, sidx = {k: i for i, k in enumerate(keys)}, {s: i for i, s in enumerate(strs)}
    out = bytearray(16)

    def align():
        out.extend(b"\0" * (-len(out) % 4))

    def table(items):
        align()
        start = len(out)
        blob = [s.encode() + b"\0" for s in items]
        offs, o = [], 4 + 4 * (len(items) + 1)
        for b in blob:
            offs.append(o)
            o += len(b)
        offs.append(o)
        out.extend(struct.pack("<I", 0xC2 | len(items) << 8) + struct.pack(f"<{len(offs)}I", *offs) + b"".join(blob))
        return start

    def typed(v):
        if isinstance(v, dict):
            return 0xC1
        if isinstance(v, list):
            return 0xC0
        if isinstance(v, str):
            return 0xA0
        if isinstance(v, bool):
            return 0xD0
        if isinstance(v, U32):
            return 0xD3
        if isinstance(v, int):
            return 0xD1
        return 0xD2

    def scalar(t, v):
        if t == 0xA0:
            return sidx[v]
        if t == 0xD2:
            return struct.unpack("<I", struct.pack("<f", v))[0]
        return int(v) & 0xFFFFFFFF

    def node(v):
        # Children first, so this node's slots can hold their offsets.
        items = sorted(v.items(), key=lambda kv: kidx[kv[0]]) if isinstance(v, dict) else list(enumerate(v))
        sub = {k: node(x) for k, x in items if isinstance(x, (dict, list))}
        align()
        start = len(out)
        if isinstance(v, dict):
            out.extend(struct.pack("<I", 0xC1 | len(items) << 8))
            for k, x in items:
                t = typed(x)
                out.extend(struct.pack("<II", kidx[k] | t << 24, sub[k] if k in sub else scalar(t, x)))
        else:
            out.extend(struct.pack("<I", 0xC0 | len(items) << 8) + bytes(typed(x) for _, x in items))
            align()
            for k, x in items:
                out.extend(struct.pack("<I", sub[k] if k in sub else scalar(typed(x), x)))
        return start

    kt = table(keys) if keys else 0
    st = table(strs) if strs else 0
    root_off = node(root)
    struct.pack_into("<2sHIII", out, 0, b"YB", version, kt, st, root_off)
    return bytes(out)


# ---- BFLYT ------------------------------------------------------------------------------------------------------

def flyt_sections(d):
    o, n = struct.unpack_from("<H", d, 6)[0], struct.unpack_from("<H", d, 0x10)[0]
    for _ in range(n):
        size = struct.unpack_from("<I", d, o + 4)[0]
        yield o, d[o:o + 4], size
        o += size


def pane_name(d, o):
    return d[o + 0xC:o + 0xC + 24].split(b"\0")[0].decode("latin1")


def patch_pause_layout(d):
    """L_QuitBtn: a copy of L_BtnExit (same part, same look) right after it, one step lower; the icon row moves down."""
    out = bytearray(d)
    src = None
    for o, magic, size in flyt_sections(d):
        if magic == b"prt1" and pane_name(d, o) == "L_BtnExit":
            src = (o, size)
        if magic == b"prt1" and pane_name(d, o) in ICON_PANES:
            struct.pack_into("<f", out, o + 0x30, ICON_ROW_Y)  # translate.y
        if magic == b"prt1" and pane_name(d, o) == QUIT_PANE:
            raise SystemExit("the pause layout already has " + QUIT_PANE)
    assert src, "L_BtnExit not found in Pause.bflyt"
    o, size = src
    clone = bytearray(d[o:o + size])
    clone[0xC:0xC + 24] = QUIT_PANE.encode().ljust(24, b"\0")
    struct.pack_into("<f", clone, 0x30, QUIT_Y)
    out[o + size:o + size] = clone
    struct.pack_into("<I", out, 0xC, len(out))  # file size
    struct.pack_into("<H", out, 0x10, struct.unpack_from("<H", d, 0x10)[0] + 1)  # section count
    return bytes(out)


def patch_title_layout(d):
    """The "(Minus) Quit Game" line: a copy of the T_PressStart_USEU text pane (the game's title font and look) as a
    child of each of the two "Press L + R to Start" text panes, T_PressStart and T_PressStart_USEU (TITLE_PANES). The
    game shows one of them by region and hides it once the title is left (the title layout stays up behind the user
    and character screens; a line beside them stayed visible there), and children go with their parent. The parents
    get the "influenced alpha" flag, so a fade of the parent fades the line too. Each line's text is the TitleRef.msbt
    label of its own name; the copy's own text (shown if no label is found) is set to the English one."""
    out = bytearray(d)
    src, parents = None, {}
    for o, magic, size in flyt_sections(d):
        name = pane_name(d, o) if magic == b"txt1" else None
        if name == "T_PressStart_USEU":
            src = (o, size)
        if name in TITLE_PANES:
            parents[name] = o
        if name in TITLE_PANES.values():
            raise SystemExit("the title layout already has " + name)
    assert src and len(parents) == len(TITLE_PANES), "T_PressStart / T_PressStart_USEU not found in Title.bflyt"
    o, size = src
    template = bytes(out[o:o + size])
    # Last parent first, so the offsets of the earlier ones stay right.
    for parent, po in sorted(parents.items(), key=lambda kv: -kv[1]):
        psize = struct.unpack_from("<I", out, po + 4)[0]
        nxt = po + psize
        if out[nxt:nxt + 4] == b"usd1":  # the parent's user data stays right after it
            nxt += struct.unpack_from("<I", out, nxt + 4)[0]
        assert out[nxt:nxt + 4] != b"pas1", parent + " already has children"
        out[po + 8] |= 0x02  # influenced alpha: its children take its alpha
        clone = bytearray(template)
        clone[0xC:0xC + 24] = TITLE_PANES[parent].encode().ljust(24, b"\0")
        struct.pack_into("<ff", clone, 0x2C, *TITLE_POS)
        struct.pack_into("<ff", clone, 0x44, TITLE_SCALE, TITLE_SCALE)
        buf = struct.unpack_from("<H", clone, 0x54)[0]
        text_at = struct.unpack_from("<I", clone, 0x64)[0]
        text = TITLE_QUIT_TEXT["USen"].encode("utf-16-le")[:buf - 2] + b"\0\0"
        clone[text_at:text_at + buf] = text.ljust(buf, b"\0")
        struct.pack_into("<H", clone, 0x56, len(text))
        out[nxt:nxt] = b"pas1" + struct.pack("<I", 8) + clone + b"pae1" + struct.pack("<I", 8)
    struct.pack_into("<I", out, 0xC, len(out))
    struct.pack_into("<H", out, 0x10, struct.unpack_from("<H", d, 0x10)[0] + 3 * len(TITLE_PANES))
    return bytes(out)


# ---- MSBT -------------------------------------------------------------------------------------------------------

def msbt_add(d, label, text):
    """Adds one message: LBL1 rebuilt (hash buckets), ATR1 count + 1 (Wonder's entries are 0 bytes), TXT2 appended."""
    assert d[:8] == b"MsgStdBn" and d[8:10] == b"\xff\xfe" and d[0xC] == 1, "not a little-endian UTF-16 MSBT"
    sections, o = [], 0x20
    while o < len(d):
        size = struct.unpack_from("<I", d, o + 4)[0]
        sections.append([d[o:o + 4], d[o + 16:o + 16 + size]])
        o += 16 + ((size + 15) & ~15)
    body = dict((bytes(m), b) for m, b in sections)
    lbl, txt = body[b"LBL1"], body[b"TXT2"]
    buckets = struct.unpack_from("<I", lbl)[0]
    labels = []
    for i in range(buckets):
        count, off = struct.unpack_from("<II", lbl, 4 + 8 * i)
        for _ in range(count):
            n = lbl[off]
            labels.append((lbl[off + 1:off + 1 + n], struct.unpack_from("<I", lbl, off + 1 + n)[0]))
            off += 5 + n
    assert label.encode() not in [l for l, _ in labels], label + " already in the MSBT"
    count = struct.unpack_from("<I", txt)[0]
    offs = list(struct.unpack_from(f"<{count}I", txt, 4)) + [len(txt)]
    texts = [txt[offs[i]:offs[i + 1]] for i in range(count)]
    labels.append((label.encode(), count))
    texts.append(text.encode("utf-16-le") + b"\0\0")

    def h(s):
        v = 0
        for c in s:
            v = (v * 0x492 + c) & 0xFFFFFFFF
        return v % buckets

    table, blob = bytearray(struct.pack("<I", buckets)), bytearray()
    base = 4 + 8 * buckets
    for b in range(buckets):
        mine = [(l, i) for l, i in labels if h(l) == b]
        table += struct.pack("<II", len(mine), base + len(blob))
        for l, i in mine:
            blob += bytes([len(l)]) + l + struct.pack("<I", i)
    body[b"LBL1"] = bytes(table + blob)
    t, o = bytearray(struct.pack("<I", count + 1)), 4 + 4 * (count + 1)
    for x in texts:
        t += struct.pack("<I", o)
        o += len(x)
    body[b"TXT2"] = bytes(t + b"".join(texts))
    if b"ATR1" in body:
        n, size = struct.unpack_from("<II", body[b"ATR1"])
        assert size == 0, "ATR1 entries with data are not handled"
        body[b"ATR1"] = struct.pack("<II", n + 1, 0) + body[b"ATR1"][8:]
    out = bytearray(d[:0x20])
    for m, _ in sections:
        b = body[bytes(m)]
        out += m + struct.pack("<I", len(b)) + b"\0" * 8 + b + b"\xab" * (-len(b) % 16)
    struct.pack_into("<I", out, 0x12, len(out))
    return bytes(out)


# ---- the mod ----------------------------------------------------------------------------------------------------

def rstb_grow(d, growth):
    """System/Resource/ResourceSizeTable: the game sizes each file's load buffer from it, so a file that grew needs
    its entry grown by as much (RESTBL v1: CRC32-of-path entries, then entries by name for collisions)."""
    assert d[:6] == b"RESTBL", "not a RESTBL"
    _, name_len, ncrc, nname = struct.unpack_from("<IIII", d, 6)
    out = bytearray(d)
    left = dict(growth)
    for i in range(ncrc):
        crc, size = struct.unpack_from("<II", d, 0x16 + 8 * i)
        for path in [p for p in left if zlib.crc32(p.encode()) == crc]:
            struct.pack_into("<I", out, 0x16 + 8 * i + 4, size + left.pop(path))
    o = 0x16 + 8 * ncrc
    for i in range(nname):
        name = d[o:o + name_len].split(b"\0")[0].decode()
        if name in left:
            struct.pack_into("<I", out, o + name_len, struct.unpack_from("<I", d, o + name_len)[0] + left.pop(name))
        o += name_len + 4
    assert not left, f"not in the size table: {list(left)}"
    return bytes(out)


def write_zs(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(zs(data))
    print("wrote", path)


def pick_latest_rstb(romfs):
    """Return the ResourceSizeTable with the highest Product.<N> version number (handles merged base+update dumps)."""
    tables = glob.glob(os.path.join(glob.escape(romfs), "System", "Resource", "ResourceSizeTable.Product.*.rsizetable.zs"))
    assert tables, "no resource size table found under System/Resource"

    def ver(path):
        m = re.search(r"Product\.(\d+)\.rsizetable\.zs$", os.path.basename(path))
        return int(m.group(1)) if m else -1

    best = max(tables, key=ver)
    return best, ver(best)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--romfs", default=os.path.join(REPO, "private", "wonder", "extract", "romfs"))
    ap.add_argument("--out", default=os.path.join(REPO, "private", "wonder", "mods", "recomp-menu"))
    a = ap.parse_args()
    romfs, out = a.romfs, os.path.join(a.out, "romfs")
    growth = {}

    def mod(name, edits, member_heap=None):
        """edits: {member: function(old bytes) -> new bytes}; member_heap: {member: bytes} for pack members, whose
        own size-table entry also sizes what they are parsed into."""
        arc = unzs(os.path.join(romfs, name))
        files = {}
        for member, edit in edits.items():
            old = sarc_get(arc, member)
            files[member] = edit(old)
            heap = (member_heap or {}).get(member, 0)
            if heap:
                growth[member] = len(files[member]) - len(old) + heap
        new = sarc_replace(arc, files)
        growth[name[:-3]] = len(new) - len(arc)
        write_zs(os.path.join(out, name), new)

    mod("Layout/Pause.Nin_NX_NVN.blarc.zs", {"blyt/Pause.bflyt": patch_pause_layout})
    mod("Layout/Title.Nin_NX_NVN.blarc.zs", {"blyt/Title.bflyt": patch_title_layout})

    def add_button(data):
        version, screen = byml_read(data)
        assert byml_read(byml_write(version, screen))[1] == screen, "BYML round trip changed the pause screen"
        buttons = screen["ButtonInfoGenerateTable"]
        exit_btn = next(b for b in buttons if b["PaneName"] == "L_BtnExit")
        buttons.insert(buttons.index(exit_btn) + 1, {
            **exit_btn, "PaneName": QUIT_PANE, "UniqueName": "QuitBtn",
            "DecideInfoGenerateTable": {**exit_btn["DecideInfoGenerateTable"], "EnumValue": QUIT_ENUM}})
        return byml_write(version, screen)

    # The pause screen's entry (19136 for a 4660-byte file at 1.2.0) is its parsed tables too: room for one more button.
    pause = "Gyml/UIExtraScreenParam/Pause.game__UIExtraScreenParam.bgyml"
    mod("Pack/Bootup.Nin_NX_NVN.pack.zs", {pause: add_button}, member_heap={pause: 0x2000})

    best_table, product_ver = pick_latest_rstb(romfs)
    mals_all = sorted(os.listdir(os.path.join(romfs, "Mals")))
    tag = f".Product.{product_ver}."
    mals_matching = [f for f in mals_all if tag in f]
    mals_selected = mals_matching or mals_all
    for f in mals_selected:
        lang = f.split(".")[0]
        if lang not in QUIT_TEXT:
            print("skipped", f, "(no translation)")
            continue
        button = lambda m, lang=lang: msbt_add(m, QUIT_PANE + "-T_BtnName", QUIT_TEXT[lang])
        def title(m, lang=lang):
            for pane in TITLE_PANES.values():
                m = msbt_add(m, pane, TITLE_QUIT_TEXT[lang])
            return m
        mod("Mals/" + f, {"LayoutMsgRef/PauseRef.msbt": button, "LayoutMsgRef/TitleRef.msbt": title})

    name = "System/Resource/" + os.path.basename(best_table)
    write_zs(os.path.join(out, name), rstb_grow(unzs(os.path.join(romfs, name)), growth))


if __name__ == "__main__":
    main()
