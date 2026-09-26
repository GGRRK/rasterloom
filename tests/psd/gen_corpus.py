#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent PSD/PSB writer for the BUILD-SPEC round-trip corpus (12 self-authored files).

Written struct by struct from Adobe's public "Photoshop File Formats Specification"
(https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/); psd-tools (MIT) was read only as a
layout cross-check for the descriptor, TySh, PlLd/SoLd, lnk2 and vector-mask records. It never
calls Rasterloom and shares no code with src/core/psd/.

    python3 tests/psd/gen_corpus.py OUT_DIR

Files: nested_groups, passthrough_group, clipping_stack, mask_vector, adjustment, layer_style,
text_layer, smart_object, depth16, cmyk, psb_30001 (.psb), empty_layer. Every file carries some
image resources (1005, 1034, 1037) and at least one layer block the codec does not interpret, so the
pass-through path is exercised everywhere.
"""
import os
import struct
import sys

import numpy as np

W, H = 96, 64

# ---------------------------------------------------------------------------------------------
# primitives


def be(fmt, *v):
    return struct.pack(">" + fmt, *v)


def pad(b, n):
    return b + b"\0" * ((-len(b)) % n)


def packbits(row: bytes) -> bytes:
    """PackBits (TN1023). Independent implementation: repeat packets for runs >= 2."""
    out = bytearray()
    i, n = 0, len(row)
    while i < n:
        j = i
        while j + 1 < n and row[j + 1] == row[i] and j - i < 127:
            j += 1
        run = j - i + 1
        if run >= 2:
            out += bytes([257 - run, row[i]])
            i += run
            continue
        k = i
        while k < n and k - i < 128 and not (k + 1 < n and row[k + 1] == row[k]):
            k += 1
        if k == i:
            k = i + 1
        out += bytes([k - i - 1]) + row[i:k]
        i = k
    return bytes(out)


def rle_channel(plane, depth, psb):
    """compression 1 + byte counts + rows, for a 2-D uint8/uint16 plane."""
    h = plane.shape[0]
    rows = []
    for y in range(h):
        r = plane[y].astype(">u2").tobytes() if depth == 16 else plane[y].astype(np.uint8).tobytes()
        rows.append(packbits(r))
    counts = b"".join(be("I" if psb else "H", len(r)) for r in rows)
    return be("H", 1) + counts + b"".join(rows)


def raw_empty():
    return be("H", 0)


def unicode_string(s):
    u = s.encode("utf-16-be")
    return be("I", len(u) // 2) + u


def pascal(s, align):
    b = s.encode("latin-1")
    return pad(bytes([len(b)]) + b, align)


def block(key, data, sig=b"8BIM", psb=False, big=False, align=1):
    ln = be("Q", len(data)) if (psb and big) else be("I", len(data))
    return pad(sig + key + ln + data, align) if align > 1 else sig + key + ln + data


# ---- descriptors (Adobe spec "Descriptor structure") ----------------------------------------


def key_id(k):
    k = k.encode() if isinstance(k, str) else k
    return be("I", 0) + k if len(k) == 4 else be("I", len(k)) + k


def desc(cls, items, name=""):
    out = unicode_string(name) + key_id(cls) + be("I", len(items))
    for k, v in items:
        out += key_id(k) + value(v)
    return out


def value(v):
    t = v[0]
    if t == "long":
        return b"long" + be("i", v[1])
    if t == "doub":
        return b"doub" + be("d", v[1])
    if t == "bool":
        return b"bool" + bytes([1 if v[1] else 0])
    if t == "enum":
        return b"enum" + key_id(v[1]) + key_id(v[2])
    if t == "UntF":
        return b"UntF" + v[1] + be("d", v[2])
    if t == "TEXT":
        return b"TEXT" + unicode_string(v[1])
    if t == "Objc":
        return b"Objc" + desc(v[1], v[2])
    if t == "tdta":
        return b"tdta" + be("I", len(v[1])) + v[1]
    raise ValueError(t)


def rgbc(r, g, b):
    return ("Objc", "RGBC", [("Rd  ", ("doub", r)), ("Grn ", ("doub", g)), ("Bl  ", ("doub", b))])


# ---------------------------------------------------------------------------------------------
# layers


def ranges():
    return b"\x00\x00\xff\xff" * 10


class Layer:
    def __init__(self, name, rect=(0, 0, 0, 0), planes=None, blend=b"norm", opacity=255, clipping=0,
                 hidden=False, flags=0x08, blocks=(), mask=None, lsct=None, raw_channels=None):
        self.name, self.rect, self.planes = name, rect, planes or {}
        self.blend, self.opacity, self.clipping = blend, opacity, clipping
        self.flags = flags | (2 if hidden else 0)
        self.blocks, self.mask, self.lsct = list(blocks), mask, lsct
        self.raw_channels = raw_channels  # [(id, bytes)] overrides planes


def encode_layers(layers, depth, psb, merged_alpha=True):
    """Layer info body (count + records + channel data), without its length field."""
    recs, chans = b"", b""
    for L in layers:
        top, left, bottom, right = L.rect
        channel_list = []
        if L.raw_channels is not None:
            channel_list = L.raw_channels
        else:
            for cid in sorted(k for k in L.planes if k != -2):
                channel_list.append((cid, rle_channel(L.planes[cid], depth, psb)))
            if not channel_list:
                channel_list = [(c, raw_empty()) for c in (-1, 0, 1, 2)]
            # Photoshop order: alpha first.
            channel_list.sort(key=lambda c: (c[0] != -1, c[0]))
        mask_data = b""
        if L.mask is not None:
            m = L.mask
            mask_data = be("4iBB", m["top"], m["left"], m["bottom"], m["right"], m["default"], m["flags"]) + b"\0\0"
            if m.get("plane") is not None:
                channel_list.append((-2, rle_channel(m["plane"], depth, psb)))
            else:
                channel_list.append((-2, raw_empty()))
        rec = be("4i", top, left, bottom, right) + be("H", len(channel_list))
        for cid, data in channel_list:
            rec += be("h", cid) + (be("Q", len(data)) if psb else be("I", len(data)))
            chans += data
        rec += b"8BIM" + L.blend + bytes([L.opacity, L.clipping, L.flags, 0])
        extra = be("I", len(mask_data)) + mask_data + be("I", 40) + ranges() + pascal(L.name, 4)
        extra += block(b"luni", pad(unicode_string(L.name), 4))
        if L.lsct is not None:
            extra += block(b"lsct", L.lsct)
        for key, data in L.blocks:
            extra += block(key, data, psb=psb, big=key in BIG)
        rec += be("I", len(extra)) + extra
        recs += rec
    n = len(layers)
    return be("h", -n if merged_alpha else n) + recs + chans


BIG = {b"LMsk", b"Lr16", b"Lr32", b"Layr", b"Mt16", b"Mt32", b"Mtrn", b"Alph", b"FMsk", b"lnk2", b"FEid",
       b"FXid", b"PxSD", b"cinf", b"lnk3", b"lnkE", b"extd", b"extn", b"pths", b"artd", b"FELS"}


def divider(name="</Layer group>"):
    return Layer(name, flags=0x18, lsct=be("I", 3))


def group(name, blend=b"norm", opacity=255, closed=False, subtype=None, hidden=False):
    ls = be("I", 2 if closed else 1) + b"8BIM" + blend
    if subtype is not None:
        ls += be("I", subtype)
    rec_blend = blend
    return Layer(name, blend=rec_blend, opacity=opacity, lsct=ls, hidden=hidden)


def solid(rect, rgba, depth=8):
    top, left, bottom, right = rect
    h, w = bottom - top, right - left
    mx = 65535 if depth == 16 else 255
    dt = np.uint16 if depth == 16 else np.uint8
    planes = {}
    for cid, v in zip((0, 1, 2, -1), rgba):
        planes[cid] = np.full((h, w), round(v / 255 * mx), dtype=dt)
    return planes


def gradient(rect, c0, c1, alpha=None, depth=8):
    top, left, bottom, right = rect
    h, w = bottom - top, right - left
    mx = 65535 if depth == 16 else 255
    t = np.linspace(0.0, 1.0, w)[None, :] * np.ones((h, 1))
    planes = {}
    for cid, a, b in zip((0, 1, 2), c0, c1):
        planes[cid] = np.round((a + (b - a) * t) / 255 * mx).astype(np.uint16 if depth == 16 else np.uint8)
    if alpha is None:
        planes[-1] = np.full((h, w), mx, dtype=np.uint16 if depth == 16 else np.uint8)
    else:
        a0, a1 = alpha
        yy = np.linspace(0.0, 1.0, h)[:, None] * np.ones((1, w))
        planes[-1] = np.round((a0 + (a1 - a0) * yy) / 255 * mx).astype(np.uint16 if depth == 16 else np.uint8)
    return planes


def background(depth=8):
    return Layer("Background", (0, 0, H, W), gradient((0, 0, H, W), (30, 60, 200), (240, 200, 40), depth=depth))


# ---------------------------------------------------------------------------------------------
# file assembly


def irbs(extra=b""):
    def res(rid, data, name=""):
        return b"8BIM" + be("H", rid) + pascal(name, 2) + be("I", len(data)) + pad(data, 2)
    out = res(1005, be("IHHIHH", 72 << 16, 1, 1, 72 << 16, 1, 1))
    out += res(1034, b"\x01")                      # copyright flag
    out += res(1037, be("i", 120))                 # global angle
    return out + extra


def merged_rle(w, h, nch, depth, psb, fill=None):
    """Merged image data: RLE, all counts first. `fill` = per-channel constant (default mid grey)."""
    counts, rows = b"", b""
    mx = 65535 if depth == 16 else 255
    for c in range(nch):
        v = (fill[c] if fill else 128) * mx // 255
        plane = np.full((1, w), v, dtype=np.uint16 if depth == 16 else np.uint8)
        r = plane[0].astype(">u2").tobytes() if depth == 16 else plane[0].tobytes()
        pb = packbits(r)
        for _ in range(h):
            counts += be("I" if psb else "H", len(pb))
            rows += pb
    return be("H", 1) + counts + rows


def write_psd(path, layers, w=W, h=H, depth=8, mode=3, color_ch=3, psb=False, global_blocks=b"",
              lr16=False, extra_irbs=b""):
    nch = color_ch + 1
    hdr = b"8BPS" + be("H", 2 if psb else 1) + b"\0" * 6 + be("HIIHH", nch, h, w, depth, mode)
    cmd = be("I", 0)
    res = irbs(extra_irbs)
    res = be("I", len(res)) + res
    body = encode_layers(layers, depth, psb)
    body = pad(body, 4)
    if lr16:
        li = (be("Q", 0) if psb else be("I", 0))
        gblocks = block(b"Lr16", body, psb=psb, big=True, align=4) + global_blocks
    else:
        li = (be("Q", len(body)) if psb else be("I", len(body))) + body
        gblocks = global_blocks
    lmi = li + be("I", 0) + gblocks
    lmi = (be("Q", len(lmi)) if psb else be("I", len(lmi))) + lmi
    img = merged_rle(w, h, nch, depth, psb, fill=[128, 128, 128, 255] if nch == 4 else [128, 128, 128, 128, 255])
    with open(path, "wb") as f:
        f.write(hdr + cmd + res + lmi + img)


# ---------------------------------------------------------------------------------------------
# the 12 corpus files


def lyid(n):
    return (b"lyid", be("I", n))


def f_nested(out):
    L = [background()]
    L += [divider(),
          Layer("red", (4, 4, 40, 50), solid((4, 4, 40, 50), (220, 40, 30, 255))),
          divider(),
          Layer("green", (10, 20, 50, 70), gradient((10, 20, 50, 70), (20, 200, 60), (10, 90, 30), alpha=(255, 60))),
          divider(),
          Layer("blue soft", (20, 30, 60, 90), solid((20, 30, 60, 90), (40, 80, 230, 200)), blend=b"sLit", blocks=[lyid(7)]),
          Layer("overlay", (0, 60, 30, 96), gradient((0, 60, 30, 96), (255, 255, 255), (0, 0, 0)), blend=b"over", opacity=180),
          group("G3 normal", b"norm"),
          group("G2 multiply", b"mul ", opacity=200),
          group("G1 closed", b"norm", closed=True)]
    write_psd(os.path.join(out, "nested_groups.psd"), L)


def f_pass(out):
    L = [background(), divider(),
         Layer("mul child", (8, 8, 56, 60), solid((8, 8, 56, 60), (200, 120, 60, 255)), blend=b"mul "),
         Layer("screen child", (16, 40, 48, 90), gradient((16, 40, 48, 90), (0, 40, 120), (200, 0, 90)), blend=b"scrn", opacity=128),
         group("pass group", b"pass", subtype=0)]
    L[-1].blend = b"pass"
    write_psd(os.path.join(out, "passthrough_group.psd"), L)


def f_clip(out):
    L = [background(),
         Layer("base", (10, 10, 54, 70), solid((10, 10, 54, 70), (250, 250, 250, 255)),
               blocks=[(b"iOpa", bytes([128, 0, 0, 0])), (b"clbl", bytes([1, 0, 0, 0])), (b"knko", bytes([0, 0, 0, 0]))]),
         Layer("clip normal", (0, 0, 40, 96), gradient((0, 0, 40, 96), (255, 0, 0), (0, 0, 255)), clipping=1),
         Layer("clip screen", (30, 30, 64, 96), solid((30, 30, 64, 96), (0, 200, 100, 255)), blend=b"scrn", opacity=180, clipping=1)]
    write_psd(os.path.join(out, "clipping_stack.psd"), L)


def vmsk_rect(x0, y0, x1, y1):
    """Vector mask: version 3, flags 0, path records (26 bytes each), fixed 8.24 fractions."""
    def fx(v):
        return int(round(v * (1 << 24)))
    recs = be("H", 6) + b"\0" * 24                          # path fill rule
    recs += be("HH", 8, 0) + b"\0" * 22                     # initial fill rule
    recs += be("HHhHII", 0, 4, 1, 0, 0, 0) + b"\0" * 10    # closed subpath, 4 knots
    for (x, y) in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
        p = be("ii", fx(y), fx(x))
        recs += be("H", 2) + p + p + p                      # closed knot, unlinked
    return be("II", 3, 0) + recs


def f_mask(out):
    mplane = np.tile(np.linspace(0, 255, 60).round().astype(np.uint8), (40, 1))
    L = [background(),
         Layer("masked", (0, 0, 64, 96), solid((0, 0, 64, 96), (230, 30, 120, 255)),
               mask={"top": 12, "left": 18, "bottom": 52, "right": 78, "default": 0, "flags": 0, "plane": mplane},
               blocks=[(b"vmsk", vmsk_rect(0.1, 0.1, 0.8, 0.9))]),
         Layer("disabled mask", (30, 0, 64, 40), solid((30, 0, 64, 40), (20, 20, 20, 255)),
               mask={"top": 30, "left": 0, "bottom": 64, "right": 20, "default": 255, "flags": 2,
                     "plane": np.zeros((34, 20), dtype=np.uint8)},
               blocks=[(b"lspf", be("I", 1))])]
    write_psd(os.path.join(out, "mask_vector.psd"), L)


def levels_block():
    recs = [(20, 235, 0, 255, 140), (0, 255, 10, 245, 100), (0, 255, 0, 255, 100), (5, 250, 0, 255, 90)]
    recs += [(0, 255, 0, 255, 100)] * 25
    return be("H", 2) + b"".join(be("5H", *r) for r in recs)


def f_adjust(out):
    mplane = np.tile(np.linspace(255, 0, 64).round().astype(np.uint8)[:, None], (1, 96))
    L = [background(),
         Layer("Levels", blocks=[(b"levl", levels_block())],
               mask={"top": 0, "left": 0, "bottom": 64, "right": 96, "default": 255, "flags": 0, "plane": mplane}),
         Layer("Invert", opacity=128, blocks=[(b"nvrt", b"")]),
         Layer("Brightness", blocks=[(b"brit", be("hhHB", 20, -10, 127, 0) + b"\0" * 3)], hidden=True),
         Layer("Posterize", blocks=[(b"post", be("HH", 5, 0))], hidden=True)]
    write_psd(os.path.join(out, "adjustment.psd"), L)


def lfx2_block():
    shadow = ("Objc", "DrSh", [("enab", ("bool", True)), ("present", ("bool", True)),
                               ("showInDialog", ("bool", True)), ("Md  ", ("enum", "BlnM", "Mltp")),
                               ("Clr ", rgbc(0.0, 0.0, 0.0)), ("Opct", ("UntF", b"#Prc", 75.0)),
                               ("uglg", ("bool", True)), ("lagl", ("UntF", b"#Ang", 120.0)),
                               ("Dstn", ("UntF", b"#Pxl", 5.0)), ("Ckmt", ("UntF", b"#Pxl", 0.0)),
                               ("blur", ("UntF", b"#Pxl", 5.0))])
    d = desc("null", [("Scl ", ("UntF", b"#Prc", 100.0)), ("masterFXSwitch", ("bool", True)), ("DrSh", shadow)])
    return be("II", 0, 16) + d


def f_lfx2(out):
    L = [background(),
         Layer("styled", (16, 24, 48, 72), solid((16, 24, 48, 72), (255, 220, 0, 255)),
               blocks=[lyid(3), (b"lfx2", pad(lfx2_block(), 4)), (b"fxrp", be("dd", 24.0, 16.0))])]
    write_psd(os.path.join(out, "layer_style.psd"), L)


def tysh_block():
    engine = (b"\n\n<<\n\t/EngineDict\n\t<<\n\t\t/Editor\n\t\t<<\n\t\t\t/Text (\xfe\xff\x00R\x00l\x00\r)\n"
              b"\t\t>>\n\t>>\n>>")
    text = desc("TxLr", [("Txt ", ("TEXT", "Rl\r")), ("textGridding", ("enum", "textGridding", "None")),
                         ("Ornt", ("enum", "Ornt", "Hrzn")), ("AntA", ("enum", "Annt", "antiAliasSharp")),
                         ("EngineData", ("tdta", engine))])
    warp = desc("warp", [("warpStyle", ("enum", "warpStyle", "warpNone")), ("warpValue", ("doub", 0.0)),
                         ("warpPerspective", ("doub", 0.0)), ("warpPerspectiveOther", ("doub", 0.0)),
                         ("warpRotate", ("enum", "Ornt", "Hrzn"))])
    return (be("H", 1) + be("6d", 1, 0, 0, 1, 10, 40) + be("H", 50) + be("I", 16) + text +
            be("H", 1) + be("I", 16) + warp + be("4i", 0, 0, 0, 0))


def f_text(out):
    rect = (22, 10, 42, 40)
    planes = solid(rect, (10, 10, 10, 255))
    planes[-1] = (np.indices((20, 30)).sum(axis=0) % 3 == 0).astype(np.uint8) * 255
    L = [background(), Layer("Rl", rect, planes, blocks=[(b"TySh", pad(tysh_block(), 4)), lyid(9)])]
    write_psd(os.path.join(out, "text_layer.psd"), L)


UUID = "3f1c0e2a-5b7d-4c11-9a0e-6d2b8c4f7a10"


def smart_blocks():
    placed = desc("null", [("Idnt", ("TEXT", UUID)), ("placed", ("TEXT", UUID)), ("PgNm", ("long", 1)),
                           ("totalPages", ("long", 1)), ("Crop", ("long", 1)), ("Type", ("long", 2)),
                           ("Annt", ("long", 16))])
    sold = b"soLD" + be("I", 4) + be("I", 16) + placed
    warp = desc("warp", [("warpStyle", ("enum", "warpStyle", "warpNone")), ("warpValue", ("doub", 0.0)),
                         ("warpPerspective", ("doub", 0.0)), ("warpPerspectiveOther", ("doub", 0.0)),
                         ("warpRotate", ("enum", "Ornt", "Hrzn"))])
    plld = (b"plcL" + be("I", 3) + pascal(UUID, 1) + be("4I", 1, 1, 16, 2) +
            be("8d", 30, 20, 70, 20, 70, 50, 30, 50) + be("II", 0, 16) + warp)
    return [(b"SoLd", pad(sold, 4)), (b"PlLd", pad(plld, 4))]


def lnk2_block():
    data = b"\x89PNG\r\n\x1a\n" + b"not really a png, opaque payload" * 3
    item = (b"liFD" + be("I", 7) + pascal(UUID, 1) + unicode_string("inner.png") + b"png " + b"8BIM" +
            be("Q", len(data)) + b"\x00" + data + unicode_string("") + be("d", 0.0) + b"\x00")
    return pad(be("Q", len(item)) + pad(item, 4), 4)


def f_smart(out):
    L = [background(), Layer("smart", (20, 30, 50, 70), gradient((20, 30, 50, 70), (90, 0, 160), (250, 140, 0)),
                             blocks=smart_blocks())]
    write_psd(os.path.join(out, "smart_object.psd"), L, global_blocks=block(b"lnk2", lnk2_block(), align=4))


def f_16(out):
    L = [background(16),
         Layer("sixteen", (8, 8, 56, 88), gradient((8, 8, 56, 88), (255, 10, 10), (10, 255, 200), alpha=(255, 40), depth=16),
               blend=b"hLit", blocks=[lyid(12)])]
    write_psd(os.path.join(out, "depth16.psd"), L, depth=16, lr16=True)


def f_cmyk(out):
    def cmyk_planes(rect, c, m, y, k, a=255):
        top, left, bottom, right = rect
        h, w = bottom - top, right - left
        # stored inverted: 255 - ink
        return {0: np.full((h, w), 255 - c, np.uint8), 1: np.full((h, w), 255 - m, np.uint8),
                2: np.full((h, w), 255 - y, np.uint8), 3: np.full((h, w), 255 - k, np.uint8),
                -1: np.full((h, w), a, np.uint8)}
    L = [Layer("paper", (0, 0, H, W), cmyk_planes((0, 0, H, W), 0, 0, 0, 0)),
         Layer("cyan", (8, 8, 40, 60), cmyk_planes((8, 8, 40, 60), 255, 0, 0, 0)),
         Layer("magenta 50", (20, 30, 60, 90), cmyk_planes((20, 30, 60, 90), 0, 200, 0, 30, a=128), blend=b"mul ")]
    write_psd(os.path.join(out, "cmyk.psd"), L, mode=4, color_ch=4)


def f_psb(out):
    w, h = 30001, 8
    L = [Layer("left", (0, 0, 8, 16), solid((0, 0, 8, 16), (255, 0, 0, 255))),
         Layer("right", (0, 29985, 8, 30001), solid((0, 29985, 8, 30001), (0, 0, 255, 255)), blocks=[lyid(2)]),
         Layer("middle", (2, 15000, 6, 15010), solid((2, 15000, 6, 15010), (0, 255, 0, 128)))]
    write_psd(os.path.join(out, "psb_30001.psb"), L, w=w, h=h, psb=True)


def f_empty(out):
    L = [background(), Layer("empty", blocks=[lyid(4)]),
         Layer("after", (4, 4, 20, 20), solid((4, 4, 20, 20), (0, 0, 0, 255)), blend=b"diff")]
    write_psd(os.path.join(out, "empty_layer.psd"), L)


FILES = [f_nested, f_pass, f_clip, f_mask, f_adjust, f_lfx2, f_text, f_smart, f_16, f_cmyk, f_psb, f_empty]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "psd_corpus"
    os.makedirs(out, exist_ok=True)
    for f in FILES:
        f(out)
    print("wrote %d files to %s" % (len(FILES), out))


if __name__ == "__main__":
    main()
