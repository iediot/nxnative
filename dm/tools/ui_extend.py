"""Extend a game's NintendoWare layouts (BFLYT / BFLAN in SARC archives) to a wider screen, as a RomFS overlay folder.

Usage: ui_extend.py <RomFS image or folder> <out folder> --aspect <width / height> [--dir /Layout] [--only <substring>] [--narrow <layouts>] [--keep <panes>] [--dry]

With screen fill (profile screen_fill) the game draws a wider 16:9 picture that the front end stretches over the screen, and a projection gate gives the
layouts the screen's aspect: they keep their proportions, centred, with the sides empty. This tool moves what sits at the sides out to the new edges
and widens full-screen backgrounds, in the layout files themselves, so the HUD reaches the corners and menus cover the screen, nothing stretched:

- A layout is extended when its screen (lyt1) is 16:9 and at least 1280 wide (a full-screen layout; parts layouts have their own small sizes), unless it is a part that some
  layout places off centre (Animal Crossing: New Horizons' passport card, LProfileBtn, declares a 1920 x 1080 screen and sits in its list at x 0 and 1919): a first pass
  collects every part pane's placement.
- Pane positions are followed from the root (translation times the scales above; rotation ignored). A pane's extent is that of the pictures, text boxes,
  windows and parts in its subtree (null and alignment panes have none of their own).
- The topmost pane whose extent lies wholly on one side, beyond a tenth of the width from the centre and not inside a box (a picture or window
  narrower than the screen that does not move itself), reaching the outer 15 % of the width (what a menu panel holds in its middle stays) and not wholly off the
  screen (a carousel's next page), moves out by the extra half width (W * (k - 1) / 2, k = aspect / (16 / 9); with --move-aspect, the extra half width of that aspect instead, the safe area's), and so do the
  translate-X keys of the animations that move it. So does a null or alignment pane placed on a screen edge (a group the designer anchored there).
  A part counts by its position only: its size is that of its own layout's screen. One of three or more like siblings (pictures, windows, text boxes or
  parts of the same kind and size, within 5 %; null groups holding the same kinds of panes) does not move unless they all do (a row of buttons whose ends reach the outer 15 % stays whole).
- A square picture or window (a circle wipe: a window whose corner frames draw the ring around its transparent centre) keeps its shape: scaled by k
  both ways when it would not cover the wider screen, otherwise left as it is.
- A full-screen picture (as wide as the screen and at least half its height: key art, a backdrop) is zoomed by k both ways, so it fills the
  width without being stretched (a little of its top and bottom goes off screen); not one on a placeholder texture the game fills at run time
  (a capture of the frame behind, which is the wide picture already and is widened with the screen).
- A picture or window as wide as the screen (at least 95 %) and centred is widened by k; one that reaches past a screen edge (a wipe's black sides)
  grows by the extra width there.

- A layout named with --narrow (one the game draws with a projection the gate does not widen, such as Super Mario Bros. Wonder's menu cursor, which
  it places at the selected button) gets its root scaled 1 / k horizontally instead: the button's position lands where the button is drawn.

Archives are found in the folder given (--dir) and its subfolders: zstd-compressed SARCs (.zs), Yaz0-compressed ones (.szs, Super Mario Odyssey; written again with
a simple greedy encoder) or raw ones (.sarc / .arc, Mario Kart 8 Deluxe's, which hold .szs layouts). An archive inside an archive is handled the same way and put back stored as it
was; when its size changes, the outer archive is rebuilt with each file at its old alignment. Each archive written is recompressed and padded with a zstd skippable frame to the size of the game's own,
since the IPA's RomFS image only takes files of the same size (build_ipa_from_nsp.py, romfs_for_ipa); one that does not fit is left out and reported.
"""
import argparse
import os
import struct
import sys
from pathlib import Path

import zstandard

sys.path.insert(0, str(Path(__file__).resolve().parent))
import romfs  # noqa: E402

SCREEN = False
PARTS = {}   # part layout name → the x of each part pane that places it (collect_parts): a part placed anywhere but the centre is not a full-screen layout
NARROW = set()   # --narrow: layouts the game draws with an unwidened projection
KEEP = set()     # --keep: panes that never move (with what they hold)
MOVE_K = None    # --move-aspect / (16 / 9): how far edge items move (default: as far as the screen is wide)
PANES = {b'pan1', b'pic1', b'txt1', b'wnd1', b'bnd1', b'prt1', b'ali1', b'scr1'}
VISUAL = {b'pic1', b'txt1', b'wnd1', b'prt1'}


class Pane:
    def __init__(self, kind, name, off, b, parent):
        self.kind, self.name, self.off, self.parent, self.children = kind, name, off, parent, []
        self.basepos = b[off + 9]
        self.tx, self.ty = struct.unpack_from('<ff', b, off + 0x2c)
        self.sx, self.sy = struct.unpack_from('<ff', b, off + 0x44)
        self.w, self.h = struct.unpack_from('<ff', b, off + 0x4c)


def parse_flyt(b):
    """(screen width, height, root pane) of a BFLYT"""
    o = struct.unpack_from('<H', b, 6)[0]; sw = sh = None; root = last = None; stack = []
    while o + 8 <= len(b):
        mg, sz = struct.unpack_from('<4sI', b, o)
        if mg == b'lyt1': sw, sh = struct.unpack_from('<ff', b, o + 0xc); parse_flyt.lyt1 = o
        elif mg in PANES:
            p = Pane(mg, b[o + 12:o + 36].split(b'\0')[0].decode('latin1'), o, b, stack[-1] if stack else None)
            if p.parent: p.parent.children.append(p)
            elif root is None: root = p
            last = p
        elif mg == b'pas1': stack.append(last)
        elif mg == b'pae1': stack.pop()
        if sz == 0: break
        o += sz
    return sw, sh, root


def placeholder_pics(b):
    """offsets of the pictures whose texture is a placeholder the game replaces at run time (a capture of the frame behind a menu or wipe:
    Super Mario Bros. Wonder's CmnDummy): what they show is the wide picture itself, so they are stretched with the screen, never zoomed"""
    o = struct.unpack_from('<H', b, 6)[0]; tex = []; mats = []; out = set()
    while o + 8 <= len(b):
        mg, sz = struct.unpack_from('<4sI', b, o)
        if mg == b'txl1':
            n = struct.unpack_from('<H', b, o + 8)[0]; t = o + 12
            tex = [b[t + struct.unpack_from('<I', b, t + 4 * i)[0]:].split(b'\0')[0].decode('latin1') for i in range(n)]
        elif mg == b'mat1':
            n = struct.unpack_from('<H', b, o + 8)[0]
            for i in range(n):
                mo = o + struct.unpack_from('<I', b, o + 12 + 4 * i)[0]   # name[0x1c], flags (bits 0-1: texture maps), 4 bytes, black, white, then the texture maps (u16 index, wraps)
                tis = [struct.unpack_from('<H', b, mo + 44 + 4 * j)[0] for j in range(b[mo + 28] & 3)]
                mats.append(' '.join(tex[ti] for ti in tis if ti < len(tex)))
        elif mg == b'pic1':
            mi = struct.unpack_from('<H', b, o + 0x64)[0]
            if mi < len(mats) and 'dummy' in mats[mi].lower(): out.add(o)
        if sz == 0: break
        o += sz
    return out


def extents(p, x0=0.0, k0=1.0, out=None, y0=0.0, ky0=1.0):
    """the horizontal extent of each pane's subtree (absolute, layout units), into out[pane]; its own vertical one, and its subtree's, as p.yown / p.yext"""
    if out is None: out = {}
    x, k = x0 + k0 * p.tx, k0 * p.sx
    y, ky = y0 + ky0 * p.ty, ky0 * p.sy
    lo, hi = float('inf'), float('-inf'); ylo, yhi = float('inf'), float('-inf')
    if p.kind == b'prt1':   # a part's size is its own layout's screen, often far larger than what it draws: its position only
        lo, hi = x, x; ylo, yhi = y, y
    elif p.kind in VISUAL and p.w > 0:
        w = abs(p.w * k); h = p.basepos & 3
        a = x - w / 2 if h == 0 else x if h == 1 else x - w
        lo, hi = a, a + w
        p.own = (a, a + w)
        hh = abs(p.h * ky); v = (p.basepos >> 2) & 3   # vertical origin: centre, top, bottom
        b0 = y - hh / 2 if v == 0 else y - hh if v == 1 else y
        ylo, yhi = b0, b0 + hh
    p.yown = (ylo, yhi)
    for c in p.children:
        extents(c, x, k, out, y, ky); cl, ch, _ = out[c]
        lo, hi = min(lo, cl), max(hi, ch); ylo, yhi = min(ylo, c.yext[0]), max(yhi, c.yext[1])
    p.yext = (ylo, yhi)
    out[p] = (lo, hi, x)
    return out


def plan_flyt(b, k):
    """{pane name: x shift} and [(pane, new width)] for a BFLYT, or None when it is not a full-screen 16:9 layout"""
    sw, sh, root = parse_flyt(b)
    if not root or not sw or not sh or sw < 1280 or abs(sw / sh / (16 / 9) - 1) > 0.01: return None
    ext = extents(root); extra = sw * (k - 1) / 2; edge = sw * 0.1
    move = sw * (min(MOVE_K or k, k) - 1) / 2   # how far edge items move out (--move-aspect: to the screen's safe area, clear of the Dynamic Island), backgrounds widening to the screen
    def outer(lo, hi):   # wholly on one side and reaching the outer 15 % of the width
        return lo <= hi and (hi < -edge or lo > edge) and max(-lo, hi) >= sw * 0.35 and -sw / 2 < hi and lo < sw / 2   # not wholly off the
        # screen (a carousel's next page waiting outside it: Animal Crossing: New Horizons' second passport at x 1919)
    # boxes: pictures and windows narrower than the screen (a dialogue window, a menu panel, a materials list) that do not move themselves; what lies inside one stays with
    # it (Animal Crossing: New Horizons' crafting counts left their panel when only the centre-spanning boxes counted)
    boxes = [q.own for q in ext if getattr(q, 'own', None) and q.kind in (b'pic1', b'wnd1') and q.own[1] - q.own[0] < 0.95 * sw and (q.own[0] < 0 < q.own[1] or not outer(*q.own))]
    def boxed(lo, hi): return any(a - 1 <= lo and hi <= b + 1 for a, b in boxes)
    shifts, widen = {}, []
    def walk(p):
        lo, hi, x = ext[p]
        if p.name in KEEP: return   # --keep: stays whole where it is
        if p is not root and p.kind in (b'pan1', b'ali1') and abs(abs(x) - sw / 2) < sw * 0.01:   # a group the designer anchored at a screen edge
            shifts[p] = move if x > 0 else -move
            return
        if p is not root and outer(lo, hi) and not boxed(lo, hi):
            shifts[p] = -move if hi < -edge else move
            return
        for c in p.children: walk(c)
    walk(root)
    def sig(p, depth=2):   # what a pane is, for telling like siblings: its kind and size (a null's own size means nothing) and, two levels down, its children's
        own = (p.kind, round(p.w), round(p.h)) if p.kind in VISUAL else (p.kind,)
        return own + (tuple(sig(c, depth - 1) for c in p.children) if depth else ())
    for p in list(shifts):   # one of a row of like siblings (Super Mario Bros. Wonder's twelve character buttons, Animal Crossing: New Horizons' five Nook Miles cards in their
        if not p.parent: continue   # null groups, whose ends reach the outer 15 %): the row stays whole, where it is
        sp = sig(p); sibs = [c for c in p.parent.children if c.kind == p.kind and (sig(c) == sp if p.kind not in VISUAL else abs(c.w - p.w) <= 0.05 * abs(p.w) and abs(c.h - p.h) <= 0.05 * abs(p.h))]
        if len(sibs) >= 3 and any(c not in shifts for c in sibs): shifts.pop(p, None)
    # attached: an item within 4 % of the width of a central bar that stays (spanning the screen's centre), at its end and level with it, stays with it (the L / R labels at
    # the ends of Animal Crossing: New Horizons' crafting category tabs); repeated until nothing changes, so neighbours (the parts of a date) move or stay together
    def under(q, p):
        while q is not None:
            if q is p: return True
            q = q.parent
        return False
    def grouped(p, q):   # p and q meet in a group narrower than the screen (the tabs' own group; not the screen-wide root, where a header title meets a title banner)
        anc = set(); a = p
        while a is not None: anc.add(a); a = a.parent
        a = q
        while a is not None and a not in anc: a = a.parent
        return a is not None and a is not root and ext[a][1] - ext[a][0] < 0.95 * sw
    staying = [q for q in ext if q is not root and q.kind in (b'pic1', b'wnd1') and ext[q][0] < 0 < ext[q][1] and ext[q][1] - ext[q][0] < 0.95 * sw
               and ext[q][1] - ext[q][0] >= 3 * (q.yext[1] - q.yext[0])]   # central bars only: a picture or window spanning the centre, at least three times wider than
    # tall (a row of tabs' base; not a text box's declared box, Super Mario Bros. Wonder's "Press Start", nor a picture or glow, its course list's title art)
    changed = True
    while changed:
        changed = False
        for p in list(shifts):
            if p.kind in (b'pan1', b'ali1') and abs(abs(ext[p][2]) - sw / 2) < sw * 0.01: continue   # anchored at an edge on purpose
            lo, hi, _ = ext[p]; ylo, yhi = p.yext
            for q in staying:
                if under(q, p) or any(under(q, m) for m in shifts): continue
                qlo, qhi, _ = ext[q]; qy0, qy1 = q.yext
                if max(qlo - hi, lo - qhi) < sw * 0.04 and qy0 <= (ylo + yhi) / 2 <= qy1 and grouped(p, q):   # at its end, level with it, in its group
                    shifts.pop(p); changed = True; break
    held = placeholder_pics(b)
    def bg(p, k0=1.0, moved=False):
        kk = k0 * p.sx; moved = moved or p in shifts
        own = getattr(p, 'own', None)
        if p.kind in (b'pic1', b'wnd1') and p.w > 0 and abs(p.w - p.h) < 1e-3 * p.w and abs(ext[p][2]) < 0.05 * sw:
            # a square (a circle wipe: a window whose corner frames draw the ring, its centre the hole): only uniformly, and only when it falls short
            if abs(p.w * kk) >= 0.95 * sw and abs(p.w * kk) < sw * k: widen.append((p, p.w * k, p.h * k))
        elif p.kind in (b'pic1', b'wnd1') and abs(p.w * kk) >= 1.02 * sw and abs(ext[p][2]) < 0.05 * sw: widen.append((p, p.w * 3))   # wider than the screen on purpose (a wipe's cover, which its animation scales): with room to spare
        elif p.kind == b'pic1' and p.off not in held and abs(p.w * kk) >= 0.95 * sw and abs(p.h * kk) >= 0.5 * sh and abs(ext[p][2]) < 0.05 * sw:
            widen.append((p, p.w * k, p.h * k))   # a full-screen picture (key art, a backdrop): zoomed to the width, its proportions kept (a little of its top and bottom off screen)
        elif p.kind in (b'pic1', b'wnd1') and abs(p.w * kk) >= 0.95 * sw and abs(ext[p][2]) < 0.05 * sw: widen.append((p, p.w * k))
        elif p.kind in (b'pic1', b'wnd1') and own and not moved and kk:   # a cover that reaches past a screen edge (a wipe's black sides): to the new edge
            h = p.basepos & 3; grow = 0.0
            if own[1] >= sw / 2 * 0.99 and h != 2: grow += extra   # its right edge, from a left or centre anchor
            if own[0] <= -sw / 2 * 0.99 and h != 1: grow += extra
            if h == 0 and grow: grow = 2 * max(own[1] - sw / 2 >= -sw * 0.005 and extra or 0, own[0] + sw / 2 <= sw * 0.005 and extra or 0)
            if grow: widen.append((p, p.w * 3))   # a cover (its animation may scale it): with room to spare
        for c in p.children: bg(c, kk, moved)
    bg(root)
    return shifts, widen


def apply_flyt(b, shifts, widen, k=None):
    b = bytearray(b)
    if k and SCREEN:   # the layout's screen as wide as the picture: projection texture coordinates (a pane showing the frame behind it) are made from it
        parse_flyt(bytes(b)); o = parse_flyt.lyt1; struct.pack_into('<f', b, o + 0xc, struct.unpack_from('<f', b, o + 0xc)[0] * k)
    for p, d in shifts.items(): struct.pack_into('<f', b, p.off + 0x2c, p.tx + d / (scale_above(p) or 1))
    for p, w, *h in widen:
        struct.pack_into('<f', b, p.off + 0x4c, w)
        if h: struct.pack_into('<f', b, p.off + 0x50, h[0])
    return bytes(b)


def scale_above(p):
    k = 1.0; q = p.parent
    while q: k *= q.sx; q = q.parent
    return k


def apply_flan(b, moves, widths=None):
    """adds moves[pane name] to the translate-X keys of a BFLAN's pane animations, and multiplies the width and height keys of widths[pane name]
    ((width factor, height factor or None): the wipe covers' sizes are animated); the number of keys changed"""
    widths = widths or {}
    b = bytearray(b); o = struct.unpack_from('<H', b, 6)[0]; n = 0
    while o + 8 <= len(b):
        mg, sz = struct.unpack_from('<4sI', b, o)
        if mg == b'pai1':
            _, _, _, _, nent, eofs = struct.unpack_from('<HBBHHI', b, o + 8)
            for i in range(nent):
                eo = o + struct.unpack_from('<I', b, o + eofs + 4 * i)[0]
                name = bytes(b[eo:eo + 28]).split(b'\0')[0].decode('latin1')
                if (name not in moves and name not in widths) or b[eo + 29]: continue   # a material's animation has no translation
                for t in range(b[eo + 28]):
                    to = eo + struct.unpack_from('<I', b, eo + 32 + 4 * t)[0]
                    if bytes(b[to:to + 4]) != b'FLPA': continue
                    for g in range(b[to + 4]):
                        go = to + struct.unpack_from('<I', b, to + 8 + 4 * g)[0]
                        _, tgt, ctype, _, nkey, _, ko = struct.unpack_from('<BBBBHHI', b, go)
                        if ctype != 2: continue   # hermite keys (frame, value, slope); translations and sizes use no other kind
                        if tgt == 0 and name in moves:   # translate X
                            for j in range(nkey):
                                at = go + ko + j * 12 + 4
                                struct.pack_into('<f', b, at, struct.unpack_from('<f', b, at)[0] + moves[name]); n += 1
                        elif tgt in (8, 9) and name in widths and widths[name][tgt - 8]:   # size W or H: value and slope
                            f = widths[name][tgt - 8]
                            for j in range(nkey):
                                at = go + ko + j * 12 + 4; v, sl = struct.unpack_from('<ff', b, at)
                                struct.pack_into('<ff', b, at, v * f, sl * f); n += 1
        if sz == 0: break
        o += sz
    return bytes(b), n


def sarc_files(d):
    n = struct.unpack_from('<H', d, 0x14 + 6)[0]; doff = struct.unpack_from('<I', d, 0xc)[0]; sfnt = 0x14 + 12 + n * 16
    for i in range(n):
        _, a, s, e = struct.unpack_from('<IIII', d, 0x14 + 12 + i * 16)
        yield d[sfnt + 8 + (a & 0xffffff) * 4:].split(b'\0')[0].decode('latin1'), doff + s, doff + e


def sarc_rebuild(d, repl):
    """the SARC d with the files repl[index] replaced by new contents of any size: names, hashes and order kept, each file at its old alignment"""
    n = struct.unpack_from('<H', d, 0x14 + 6)[0]; doff = struct.unpack_from('<I', d, 0xc)[0]; ent = 0x14 + 12
    head = bytearray(d[:doff]); data = bytearray()
    spans = [struct.unpack_from('<II', d, ent + i * 16 + 8) for i in range(n)]
    for i in sorted(range(n), key=lambda i: spans[i][0]):
        s0, e0 = spans[i]; body = repl.get(i, d[doff + s0:doff + e0])
        al = min(s0 & -s0 if s0 else 0x2000, 0x2000) or 0x2000   # the alignment the file had (a texture archive needs its own)
        while (doff + len(data)) % al: data.append(0)
        struct.pack_into('<II', head, ent + i * 16 + 8, len(data), len(data) + len(body)); data += body
    struct.pack_into('<I', head, 8, len(head) + len(data))
    return bytes(head + data)


def unpack(raw):
    """(SARC, how it was stored) for a SARC stored raw, Yaz0- or zstd-compressed; (None, None) for anything else"""
    try:
        if raw[:4] == b'SARC': return raw, 'raw'
        if raw[:4] == b'Yaz0': d = yaz0_decode(raw); return (d, 'yaz0') if d[:4] == b'SARC' else (None, None)
        if raw[:4] == b'\x28\xb5\x2f\xfd':
            d = zstandard.ZstdDecompressor().decompressobj().decompress(raw); return (d, 'zstd') if d[:4] == b'SARC' else (None, None)
    except (zstandard.ZstdError, IndexError, struct.error): pass
    return None, None


def repack(d, how, size=None):
    return d if how == 'raw' else yaz0_encode(d) if how == 'yaz0' else recompress(d, size)


def part_panes(b):
    """(part layout name, x) of each part pane of a BFLYT: the name follows the pane (0x54 bytes), its property count and magnification and its properties (0x28 each)"""
    o = struct.unpack_from('<H', b, 6)[0]; out = []
    while o + 8 <= len(b):
        mg, sz = struct.unpack_from('<4sI', b, o)
        if mg == b'prt1':
            n = struct.unpack_from('<I', b, o + 0x54)[0]; at = o + 0x54 + 12 + n * 0x28
            if at < o + sz: out.append((b[at:at + 64].split(b'\0')[0].decode('latin1'), struct.unpack_from('<f', b, o + 0x2c)[0]))
        if sz == 0: break
        o += sz
    return out


def collect_parts(d):
    """every part placement in a SARC (and the archives inside it) into PARTS"""
    for name, s, e in sarc_files(d):
        sub, _ = unpack(d[s:e])
        if sub is not None: collect_parts(sub)
        elif d[s:s + 4] == b'FLYT':
            for part, x in part_panes(d[s:e]): PARTS.setdefault(part, []).append(x)


def layout_name(name, outer):   # 'blyt/LProfileBtn.bflyt' → 'LProfileBtn'; an archive that keeps no names: the file it came in ('mn_L_ChangeBtn_01.szs' → 'mn_L_ChangeBtn_01')
    if name: return name.rsplit('/', 1)[-1].rsplit('.', 1)[0]
    return (outer or '').rsplit('/', 1)[-1].rsplit('.', 1)[0]


def extend_archive(d, k, log, outer=''):
    """the SARC with its full-screen layouts extended, or None when nothing changed"""
    out = bytearray(d); files = list(sarc_files(d)); moves = {}; widths = {}; changed = False; repl = {}
    for i, (name, s, e) in enumerate(files):   # an archive inside the archive (Animal Crossing: New Horizons' <name>.arc; Mario Kart 8 Deluxe's .szs layouts inside
        sub, how = unpack(d[s:e])                # its .sarc files): the same, put back stored as it was, the outer archive rebuilt when a size changed
        if sub is None: continue
        nd = extend_archive(sub, k, (lambda l, nm=name: log(l.replace('  ', f'  {nm} > ', 1))) if name else log, name)
        if nd is None: continue
        c = repack(nd, how)
        if len(c) == e - s: out[s:e] = c
        else: repl[i] = c
        changed = True
    for name, s, e in files:
        if d[s:s + 4] != b'FLYT': continue   # by its magic: some archives keep no names, only hashes (Mario Kart 8 Deluxe's .szs layouts)
        ln = layout_name(name, outer)
        if any(abs(x) > 1 for x in PARTS.get(ln, ())):   # a part some layout places off centre (a passport card in its list): its screen is not the screen
            log(f'  {name or ln}: a part placed off centre, left as it is'); continue
        name = name or f'layout at {s:#x}'
        if name.rsplit('/', 1)[-1][:-6] in NARROW:   # drawn with a projection the gate does not widen: positions the game sets land 1 / k nearer the centre
            _, _, root = parse_flyt(d[s:e]); b = bytearray(out[s:e]); struct.pack_into('<f', b, root.off + 0x44, root.sx / k); out[s:e] = b; changed = True
            log(f'  {name}: root scaled 1 / k horizontally (--narrow)'); continue
        pl = plan_flyt(d[s:e], k)
        if not pl or not (pl[0] or pl[1]): continue
        shifts, widen = pl
        out[s:e] = apply_flyt(d[s:e], shifts, widen, k); changed = True
        for p, dx in shifts.items(): moves[p.name] = dx / (scale_above(p) or 1)
        for p, w, *h in widen:
            if p.w: widths[p.name] = (w / p.w, h[0] / p.h if h and p.h else None)
        log(f'  {name}: moved {", ".join(f"{p.name} {dx:+.0f}" for p, dx in shifts.items()) or "-"}; widened {", ".join(p.name + (' (zoomed)' if h else '') for p, w, *h in widen) or "-"}')
    if moves or widths:
        for name, s, e in files:
            if d[s:s + 4] == b'FLAN':
                nb, n = apply_flan(bytes(out[s:e]), moves, widths)
                if n: out[s:e] = nb; log(f'  {name or f"animation at {s:#x}"}: {n} animation keys moved or widened')
    if repl: return sarc_rebuild(bytes(out), repl)
    return bytes(out) if changed else None


def yaz0_decode(b):
    """a Yaz0 stream (Super Mario Odyssey's .szs) decoded"""
    n = struct.unpack_from('>I', b, 4)[0]; out = bytearray(); i = 16
    while len(out) < n:
        code = b[i]; i += 1
        for bit in range(8):
            if len(out) >= n: break
            if code & (0x80 >> bit): out.append(b[i]); i += 1
            else:
                b1, b2 = b[i], b[i + 1]; i += 2; dist = ((b1 & 0xf) << 8 | b2) + 1; cnt = b1 >> 4
                if cnt == 0: cnt = b[i] + 0x12; i += 1
                else: cnt += 2
                for _ in range(cnt): out.append(out[-dist])
    return bytes(out)


def yaz0_encode(d):
    """d as a Yaz0 stream: runs of the previous 4 KB matched greedily (3-byte hash), literals otherwise"""
    out = bytearray(b'Yaz0' + struct.pack('>I', len(d)) + bytes(8)); i = 0; n = len(d); last = {}
    while i < n:
        hdr_at = len(out); out.append(0); code = 0
        for bit in range(8):
            if i >= n: break
            best_len = 0; best_dist = 0
            if i + 3 <= n:
                key = d[i:i + 3]; j = last.get(key)
                if j is not None and i - j <= 0x1000:
                    m = 0; lim = min(0x111, n - i)
                    while m < lim and d[j + m] == d[i + m]: m += 1
                    if m >= 3: best_len, best_dist = m, i - j
                last[key] = i
            if best_len:
                dd = best_dist - 1
                if best_len >= 0x12: out += bytes([dd >> 8, dd & 0xff, best_len - 0x12])
                else: out += bytes([((best_len - 2) << 4) | (dd >> 8), dd & 0xff])
                for k in range(i + 1, min(i + best_len, n - 2)): last[d[k:k + 3]] = k
                i += best_len
            else:
                code |= 0x80 >> bit; out.append(d[i]); i += 1
        out[hdr_at] = code
    return bytes(out)


def recompress(data, size):
    """data zstd-compressed and padded with a skippable frame to exactly size bytes, or None when it does not fit; size None: unpadded"""
    if size is None: return zstandard.ZstdCompressor(level=19, write_content_size=True).compress(data)
    for level in (19, 22):
        c = zstandard.ZstdCompressor(level=level, write_content_size=True).compress(data)
        gap = size - len(c)
        if gap == 0: return c
        if gap >= 8: return c + struct.pack('<II', 0x184D2A50, gap - 8) + b'\0' * (gap - 8)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('romfs'); ap.add_argument('out'); ap.add_argument('--aspect', type=float, required=True)
    ap.add_argument('--dir', default='/Layout'); ap.add_argument('--only', default=''); ap.add_argument('--dry', action='store_true'); ap.add_argument('--screen', action='store_true', help='also widen each extended layout\'s screen (lyt1)'); ap.add_argument('--pad', action='store_true', help='pad each archive to the size of the game\'s own')
    ap.add_argument('--narrow', default='', help='comma-separated layouts (BFLYT names without .bflyt) the game draws with a projection the gate does not widen: their root is scaled 1 / k horizontally, so the positions the game gives them (from a widened layout\'s panes) land on those panes, and the front end\'s stretch gives back their proportions')
    ap.add_argument('--keep', default='', help='comma-separated pane names that never move, with what they hold: a group the game lays out at run time that the rules cannot tell (Animal Crossing: New Horizons\' DIY recipes N_HaveNum_00, a "Pockets" count aligned with a "Storage" count that sits past the screen edge in the file)')
    ap.add_argument('--move-aspect', type=float, help='the width / height that edge items move out to (default --aspect): the safe area\'s, so they stay clear of the rounded corners and the Dynamic Island (iPhone 15 Pro Max in landscape: about 1.95 for 2.1667) while backgrounds still widen to the whole screen')
    a = ap.parse_args()
    k = a.aspect / (16 / 9)
    global SCREEN, MOVE_K; SCREEN = a.screen; MOVE_K = a.move_aspect / (16 / 9) if a.move_aspect else None; NARROW.update(n for n in a.narrow.split(',') if n); KEEP.update(n for n in a.keep.split(',') if n)
    if k <= 1: sys.exit('the aspect must be wider than 16:9')
    root = a.romfs.rstrip('/'); img = romfs.is_image(root)
    if img:
        im = romfs.Image(root)
        def files(d):   # every file under d, recursively (Mario Kart 8 Deluxe's UI is in folders: /UI/cmn/menu.sarc, /UI/USen/...)
            for n, c in im.children(im.find(d)):
                if im.entry(c)[4]: yield d + '/' + n
                else: yield from files(d + '/' + n)
        def read(p):
            with romfs.open(im.path + p, 'rb') as f: return f.read()
    else:
        def files(d):
            for q in sorted(Path(root + d).rglob('*')):
                if q.is_file(): yield '/' + str(q.relative_to(root))
        def read(p): return Path(root + p).read_bytes()
    done = skipped = 0
    for n in sorted(files(a.dir)):   # first pass: where each part layout is placed (PARTS)
        if n.endswith(('.zs', '.szs', '.sarc', '.arc')):
            d, _ = unpack(read(n))
            if d is not None: collect_parts(d)
    for n in sorted(files(a.dir)):
        if not n.endswith(('.zs', '.szs', '.sarc', '.arc')) or a.only not in n: continue
        raw = read(n); d, how = unpack(raw)
        if d is None: continue
        lines = []
        nd = extend_archive(d, k, lines.append)
        if nd is None: continue
        c = repack(nd, how, len(raw) if a.pad else None)
        if c is None: print(f'[RED] {n}: does not fit its {len(raw)} bytes once recompressed; left as it is'); skipped += 1; continue
        print(n); print('\n'.join(lines))
        if not a.dry:
            p = Path(a.out + n); p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(c)
        done += 1
    print(f'{done} archives extended for aspect {a.aspect:.4f} (k {k:.4f}), {skipped} left out')


if __name__ == '__main__':
    main()
