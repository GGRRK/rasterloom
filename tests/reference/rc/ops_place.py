# SPDX-License-Identifier: GPL-3.0-or-later
"""Render-script ops of doc 60 §14 (addendum 2026-09-26): place_image, layer_via_copy, clear.

Written from docs/math/60-editing-ops.md §14 only (independence rule: no C++ was read). The PNG
payload decoder below is a direct transcription of §14.1-§14.2 on Python's zlib (crc32 and the
zlib stream), with no PNG library, so its accept/reject rules are the doc's.
"""

import re
import struct
import zlib

import numpy as np

from .composite import render_document
from .core import ScriptError, canonicalize, q
from .model import RasterLayer
from .ops_adjust_filters import coverage_lerp
from .ops_editing import name_problem
from .registry import op

B64_RE = re.compile(r"^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$")
B64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
PNG_SIG = b"\x89PNG\r\n\x1a\n"
XY_MIN, XY_MAX = -32768, 32768
MAX_SIDE = 16384


# ---------------------------------------------------------------------------------------------
# §14.1 base64
# ---------------------------------------------------------------------------------------------

def _b64decode(s):
    if not isinstance(s, str) or not B64_RE.match(s):
        raise ScriptError("png: not canonical padded base64 (RFC 4648 §4 alphabet)")
    out = bytearray()
    for g in range(0, len(s), 4):
        grp = s[g:g + 4]
        pad = grp.count("=")
        vals = [B64_ALPHABET.index(ch) for ch in grp[:4 - pad]] + [0] * pad
        word = (vals[0] << 18) | (vals[1] << 12) | (vals[2] << 6) | vals[3]
        out.append((word >> 16) & 0xFF)
        if pad < 2:
            out.append((word >> 8) & 0xFF)
        if pad < 1:
            out.append(word & 0xFF)
    return bytes(out)


# ---------------------------------------------------------------------------------------------
# §14.2 the PNG subset
# ---------------------------------------------------------------------------------------------

def _bad(why):
    raise ScriptError("png payload: " + why)


def _inflate_exact(data, need):
    """Step 6: one zlib stream decompressing to exactly `need` bytes; trailing IDAT bytes ignored."""
    d = zlib.decompressobj()
    try:
        out = d.decompress(data, need + 1)
        if len(out) > need:
            _bad("image data longer than height * (1 + 4 * width)")
        if not d.eof:
            more = d.decompress(d.unconsumed_tail, 1)
            if more:
                _bad("image data longer than height * (1 + 4 * width)")
            if not d.eof:
                _bad("zlib stream is truncated")
    except zlib.error as e:
        _bad("zlib stream is corrupt (%s)" % e)
    if len(out) != need:
        _bad("image data shorter than height * (1 + 4 * width)")
    return out


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode_png_payload(b):
    """-> (w, h, P) with P a uint8 (h, w, 4) array of straight RGBA bytes, as stored."""
    if len(b) < 8 or b[:8] != PNG_SIG:
        _bad("bad signature")
    pos = 8
    seen_ihdr = seen_plte = seen_iend = False
    idat_state = 0            # 0 none yet, 1 inside the consecutive run, 2 run finished
    w = h = 0
    z = bytearray()
    while not seen_iend:
        if len(b) - pos < 8:
            _bad("chunk header past the end (no IEND)")
        length = struct.unpack(">I", b[pos:pos + 4])[0]
        ctype = b[pos + 4:pos + 8]
        if length > 0x7FFFFFFF:
            _bad("chunk length above 2^31 - 1")
        if not all((65 <= c <= 90) or (97 <= c <= 122) for c in ctype):
            _bad("chunk type is not four ASCII letters")
        if len(b) - pos - 8 < length + 4:
            _bad("chunk past the end")
        data = b[pos + 8:pos + 8 + length]
        crc = struct.unpack(">I", b[pos + 8 + length:pos + 12 + length])[0]
        critical = 65 <= ctype[0] <= 90
        name = ctype.decode("ascii")
        if name != "IDAT" and idat_state == 1:
            idat_state = 2
        if critical:
            if (zlib.crc32(ctype + data) & 0xFFFFFFFF) != crc:
                _bad("CRC mismatch in critical chunk " + name)
            if not seen_ihdr and name != "IHDR":
                _bad("first chunk is not IHDR")
            if name == "IHDR":
                if seen_ihdr:
                    _bad("second IHDR")
                seen_ihdr = True
                if length != 13:
                    _bad("IHDR length is not 13")
                w, h, depth, color, comp, filt, lace = struct.unpack(">IIBBBBB", data)
                if depth != 8 or color != 6 or comp != 0 or filt != 0 or lace != 0:
                    _bad("only 8-bit RGBA (colour type 6), non-interlaced PNGs are accepted")
                if not (1 <= w <= MAX_SIDE and 1 <= h <= MAX_SIDE):
                    _bad("width/height outside 1..16384")
            elif name == "PLTE":
                if seen_plte or idat_state != 0:
                    _bad("PLTE repeated or after IDAT")
                seen_plte = True
            elif name == "IDAT":
                if idat_state == 2:
                    _bad("IDAT chunks are not consecutive")
                idat_state = 1
                z += data
            elif name == "IEND":
                seen_iend = True
            else:
                _bad("unknown critical chunk " + name)
        elif not seen_ihdr:
            _bad("first chunk is not IHDR")
        pos += 12 + length
    if idat_state == 0:
        _bad("no IDAT")

    stride = 4 * w
    raw = _inflate_exact(bytes(z), h * (1 + stride))
    rows = []
    prev = bytearray(stride)
    for y in range(h):
        t = raw[y * (1 + stride)]
        filt = raw[y * (1 + stride) + 1:(y + 1) * (1 + stride)]
        if t > 4:
            _bad("filter type above 4")
        cur = bytearray(stride)
        for k in range(stride):
            a = cur[k - 4] if k >= 4 else 0
            bb = prev[k] if y > 0 else 0
            c = prev[k - 4] if (y > 0 and k >= 4) else 0
            if t == 0:
                pred = 0
            elif t == 1:
                pred = a
            elif t == 2:
                pred = bb
            elif t == 3:
                pred = (a + bb) // 2
            else:
                pred = _paeth(a, bb, c)
            cur[k] = (filt[k] + pred) & 0xFF
        rows.append(bytes(cur))
        prev = cur
    P = np.frombuffer(b"".join(rows), dtype=np.uint8).reshape(h, w, 4).copy()
    return w, h, P


# ---------------------------------------------------------------------------------------------
# shared: names, placement (§14.3.1)
# ---------------------------------------------------------------------------------------------

def _read_name(f):
    name = f.str("name", "")
    problem = name_problem(name)
    if problem is not None:
        raise f.err("name " + problem)
    return name


def _read_placement(f):
    above = f.str("above", None)
    parent = f.str("parent", None)
    if above is not None and parent is not None:
        raise f.err("'above' and 'parent' cannot both be given")
    return above, parent


def _check_placement(doc, above, parent):
    if above is not None:
        doc.get_node(above)
    if parent is not None:
        doc.get_container(parent)


def _insert(doc, above, parent, default_above, node):
    if above is None and parent is None:
        above = default_above
    if above is not None:
        target = doc.get_node(above)
        siblings = doc.parent_of(target).children
        siblings.insert(siblings.index(target) + 1, node)
    else:
        doc.get_container(parent if parent is not None else "root").children.append(node)


def _new_raster(node_id, rgba, name):
    node = RasterLayer(node_id, rgba)
    node.name = name
    return node


# ---------------------------------------------------------------------------------------------
# §14.3 place_image
# ---------------------------------------------------------------------------------------------

@op("place_image")
def place_image(doc, f):
    node_id = f.str("id")
    payload = f.str("png")
    if f.has("x") != f.has("y"):
        raise f.err("'x' and 'y' go together")
    x = f.int("x", None, XY_MIN, XY_MAX)
    y = f.int("y", None, XY_MIN, XY_MAX)
    name = _read_name(f)
    above, parent = _read_placement(f)
    f.check_unused()
    doc.check_new_id(node_id)
    _check_placement(doc, above, parent)
    w, h, P = decode_png_payload(_b64decode(payload))

    if x is None:
        x = (doc.w - w) // 2          # floor division (toward -inf)
        y = (doc.h - h) // 2
    L = doc.empty_rgba()
    cx0, cy0 = max(x, 0), max(y, 0)
    cx1, cy1 = min(x + w, doc.w), min(y + h, doc.h)
    if cx0 < cx1 and cy0 < cy1:
        L[cy0:cy1, cx0:cx1] = P[cy0 - y:cy1 - y, cx0 - x:cx1 - x]
    canonicalize(L)
    _insert(doc, above, parent, None, _new_raster(node_id, L, name))


# ---------------------------------------------------------------------------------------------
# §14.4 COPY, §14.6 CLEAR
# ---------------------------------------------------------------------------------------------

def COPY(doc, layer):
    """X over the whole canvas; `layer` None = merged (the png8 composite over bg)."""
    Src = render_document(doc, onto_bg=True) if layer is None else layer.rgba
    E = doc.selection_effective()
    part = Src.copy()
    part[..., 3] = q((Src[..., 3].astype(np.float64) / 255.0) * (E.astype(np.float64) / 255.0))
    canonicalize(part)
    X = np.where((E == 255)[..., None], Src,
                 np.where((E == 0)[..., None], np.zeros_like(Src), part))
    return np.ascontiguousarray(X, dtype=np.uint8)


def CLEAR(doc, layer):
    if layer.lock_alpha:
        return
    F = np.zeros_like(layer.rgba)
    layer.rgba = coverage_lerp(layer.rgba, F, doc.selection_effective())


# ---------------------------------------------------------------------------------------------
# §14.5 layer_via_copy
# ---------------------------------------------------------------------------------------------

@op("layer_via_copy")
def layer_via_copy(doc, f):
    node_id = f.str("id")
    merged = f.bool("merged", False)
    cut = f.bool("cut", False)
    layer_id = f.str("layer", None)
    name = _read_name(f)
    above, parent = _read_placement(f)
    f.check_unused()
    if merged and layer_id is not None:
        raise f.err("'layer' must be absent with merged")
    if not merged and layer_id is None:
        raise f.err("missing required field 'layer'")
    if merged and cut:
        raise f.err("'cut' with 'merged'")
    doc.check_new_id(node_id)
    src = None if merged else doc.get_raster(layer_id)
    _check_placement(doc, above, parent)

    X = COPY(doc, src)
    if not (X[..., 3] != 0).any():
        raise f.err("the selected area is empty")
    _insert(doc, above, parent, None if merged else layer_id, _new_raster(node_id, X, name))
    if cut:
        CLEAR(doc, src)


# ---------------------------------------------------------------------------------------------
# §14.6 clear
# ---------------------------------------------------------------------------------------------

@op("clear")
def clear(doc, f):
    layer = doc.get_raster(f.str("layer"))
    f.check_unused()
    CLEAR(doc, layer)
