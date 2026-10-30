"""The launcher's logo, from the player's own game files: Title_WW_LogoS of romfs/Layout/Common_Title_00 (a zstd SARC
holding timg/__Combined.bntx), decoded by tools/extract/bntx_png.c into build/work/title-logo.png. Game art: it stays in
build/ and is never committed.
The logo is also the exes' icon: build/work/tomodachi.ico, written into TomodachiLauncher.exe and tomodachi_native.exe
(Explorer, shortcuts, taskbar) on this PC only, and copied to opensmash.ico, the file runtime/display.c loads for the
game window's icon. The runtime package's exes carry no icon, so no game art.

    python scripts/launcher_art.py [game folder [build folder]]      (default build/dump: exefs/ and romfs/)

The island behind the menu (build/work/launcher-background.jpg) is not in the game files: it is a 4K render of the title
screen with the title's 2D drawn out (TOMO_SKIPPROG=3ad3b2cf,a2462987,7328f6bd,8ac327a9,d8efa8a1,4abb6b23,9d8999e9,
012b6b99,9578c377 with SSBU_SHOT_*). Without it the launcher draws a plain gradient.
The in-game settings menu (runtime/menu.cpp) is drawn with the game's own fonts: Seurat Capie EB and B and the button
glyphs of NinDev-TCExt, decoded from romfs/Font/Font.Nin_NX_NVN.bfarc.zs into build/work/ui/ (game files too: never committed).
Needs the zstandard module (pip install zstandard); the icon needs Pillow.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import zstandard

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(REPO, 'build', 'work')
DEPS = os.environ.get('SSBU_DEPS') or next((d for d in (os.path.join(REPO, 'deps'), os.path.join(REPO, '..', 'ssbu-recomp', 'deps'))
                                            if os.path.isdir(os.path.join(d, 'llvm'))), os.path.join(REPO, 'deps'))
ARCHIVE = os.path.join('romfs', 'Layout', 'Common_Title_00.Nin_NX_NVN.blarc.zs')
LOGO = 'Title_WW_LogoS^w'
FONT_ARCHIVE = os.path.join('romfs', 'Font', 'Font.Nin_NX_NVN.bfarc.zs')
UI_FONTS = {'scft/ninP_SeuratCapie-EB_003.bfotf': 'seurat_eb.otf', 'scft/ninP_SeuratCapie-B_003.bfotf': 'seurat_b.otf',
            'scft/NinDev-TCExt.bfttf': 'keys.ttf'}
ICON_EXES = ('TomodachiLauncher.exe', 'tomodachi_native.exe')


def sarc_file(blob, want):
    """One file of a SARC archive by name, or None."""
    assert blob[:4] == b'SARC', 'not a SARC archive'
    data = struct.unpack_from('<I', blob, 0xC)[0]
    n = struct.unpack_from('<H', blob, 0x14 + 6)[0]
    names = 0x14 + 12 + n * 16 + 8
    for i in range(n):
        _, attr, start, end = struct.unpack_from('<IIII', blob, 0x14 + 12 + i * 16)
        if attr & 0x01000000:
            o = names + (attr & 0xFFFFFF) * 4
            if blob[o:blob.index(b'\0', o)].decode() == want:
                return blob[data + start:data + end]
    return None


def bntx_png():
    exe = os.path.join(WORK, 'bntx_png.exe')                   # from the runtime package, or built by scripts/setup.py
    if os.path.exists(exe):
        return exe
    exe = os.path.join(WORK, 'tools', 'bntx_png.exe')
    if not os.path.exists(exe):
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        src = os.path.join(REPO, 'tools', 'extract', 'bntx_png.c'), os.path.join(REPO, 'runtime', 'tex_decode.c')
        subprocess.run([os.path.join(DEPS, 'llvm', 'bin', 'clang.exe'), '-O2', '-w', '-D_CRT_SECURE_NO_WARNINGS', *src,
                        '-I' + os.path.join(REPO, 'runtime'), '-o', exe], check=True)
    return exe


def make_icon(work):
    """work/tomodachi.ico (16 to 256 px) from work/title-logo.png: the logo centred on a transparent square."""
    from PIL import Image
    im = Image.open(os.path.join(work, 'title-logo.png')).convert('RGBA')
    im = im.crop(im.getbbox())
    side = max(im.size)
    sq = Image.new('RGBA', (side, side))
    sq.paste(im, ((side - im.width) // 2, (side - im.height) // 2))
    ico = os.path.join(work, 'tomodachi.ico')
    sq.save(ico, format='ICO', sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    shutil.copyfile(ico, os.path.join(work, 'opensmash.ico'))


def stamp_icon(work):
    """work/tomodachi.ico as the icon resource of ICON_EXES; -> the exes that could not be changed (missing, or running)."""
    import ctypes
    from ctypes import wintypes
    with open(os.path.join(work, 'tomodachi.ico'), 'rb') as f:
        ico = f.read()
    n = struct.unpack_from('<H', ico, 4)[0]
    entries = [struct.unpack_from('<BBBBHHII', ico, 6 + 16 * i) for i in range(n)]       # ..., size, offset
    group = struct.pack('<HHH', 0, 1, n) + b''.join(struct.pack('<BBBBHHIH', *e[:7], i + 1) for i, e in enumerate(entries))
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.BeginUpdateResourceW.restype = wintypes.HANDLE
    k32.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
    k32.UpdateResourceW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD, ctypes.c_char_p, wintypes.DWORD]
    k32.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]
    failed = []
    for name in ICON_EXES:
        exe = os.path.join(work, name)
        h = k32.BeginUpdateResourceW(exe, False) if os.path.exists(exe) else None
        ok = bool(h)
        if h:
            for i, e in enumerate(entries):
                ok = ok and bool(k32.UpdateResourceW(h, 3, i + 1, 0, ico[e[7]:e[7] + e[6]], e[6]))      # RT_ICON
            ok = ok and bool(k32.UpdateResourceW(h, 14, 1, 0, group, len(group)))                     # RT_GROUP_ICON
            ok = bool(k32.EndUpdateResourceW(h, not ok)) and ok                                       # failed: discarded
        if ok:
            ctypes.windll.shell32.SHChangeNotify(0x2000, 0x0005, exe, None)     # SHCNE_UPDATEITEM: Explorer drops its cached icon
        else:
            failed.append(name)
    return failed


def ui_fonts(game):
    """The settings menu's fonts (UI_FONTS) into WORK/ui; a game without the archive keeps the menu's fallback font."""
    path = os.path.join(game, FONT_ARCHIVE)
    if not os.path.exists(path):
        print('no %s: the in-game menu uses a Windows font' % path)
        return
    sys.path.insert(0, os.path.join(REPO, 'tools', 'extract'))
    from fw_fonts import decode_bfttf
    with open(path, 'rb') as f:
        blob = zstandard.ZstdDecompressor().decompress(f.read(), max_output_size=1 << 30)
    os.makedirs(os.path.join(WORK, 'ui'), exist_ok=True)
    for name, out in UI_FONTS.items():
        font = decode_bfttf(sarc_file(blob, name) or b'')
        if font[:4] not in (b'OTTO', bytes([0, 1, 0, 0])):
            print('%s: not found or not a font' % name)
            continue
        with open(os.path.join(WORK, 'ui', out), 'wb') as f:
            f.write(font)


def main():
    global WORK
    game = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'build', 'dump')
    if len(sys.argv) > 2:                                      # another build folder (the install location, scripts/setup.py)
        WORK = os.path.join(sys.argv[2], 'work')
    path = os.path.join(game, ARCHIVE)
    if not os.path.exists(path):
        sys.exit('no %s: give the game folder (exefs/ and romfs/)' % path)
    with open(path, 'rb') as f:
        blob = zstandard.ZstdDecompressor().decompress(f.read(), max_output_size=1 << 30)
    bntx = sarc_file(blob, 'timg/__Combined.bntx')
    if not bntx:
        sys.exit('%s has no timg/__Combined.bntx' % path)
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, 'title.bntx')
        with open(p, 'wb') as f:
            f.write(bntx)
        subprocess.run([bntx_png(), p, LOGO, os.path.join(WORK, 'title-logo.png')], check=True)
    ui_fonts(game)
    try:
        make_icon(WORK)
    except ImportError:
        print('no Pillow (pip install Pillow): the exes keep no icon')
        return
    failed = stamp_icon(WORK)
    if failed:
        print('the icon could not be written into %s (close the game and the launcher, then run this again)' % ', '.join(failed))


if __name__ == '__main__':
    main()
