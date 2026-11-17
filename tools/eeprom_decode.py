#!/usr/bin/env python3
"""
PDK3000 (Siemens/Continental 5WP54023, TC1766) EEPROM-emulation decoder.

Usage:  python3 -I eeprom_decode.py <eeprom_dump.bin> [--dump-blocks DIR]

Accepts:
  * 32 KB dump  (sector0 0xAFE00000 + sector1 0xAFE10000, 16 KB each, concatenated)
  * 128 KB dump (raw 0xAFE00000..0xAFE1FFFF; sectors at 0x0000 and 0x10000)

Format (reverse engineered from firmware SW G070/G088, verified on 2 dumps):
  Sector (16 KB): 4 header pages (0x80 each; "1N1N" = 0x4E31 0x4E31 when the
  matching 0xF80 slot is in use) + 4 slots of 0xF80 bytes.
    slots 0,1  = IMAGE area  : snapshot of image blocks
    slots 2,3  = RECORD area : 0x80-byte pages holding 'F' records
  IMAGE slot  : u32 seq @0, data @4 (len bytes), u16 len @0xF78, u16 crc @0xF7A,
                u32 ~seq @0xF7C.  crc = CRC16-ARC table (poly 0xA001 refl), init 0xABCD,
                over data[4:4+len].
  RECORD 'F'  : page[0]='F'(0x46), [1]=block id, u16 npages @2, u32 seq @4,
                u16 len @8, u16 crc @10, payload @12 (len bytes, may span pages).
                Last page of the record: byte @0x7D = ~id, u16 @0x7E = npages.
                crc = CRC16-ARC init 0xABCD over payload.
  Every block payload: u16 inner_crc (CRC16-ARC init 0xABCD over payload[2:]) +
                buffer (block 3 = directory uses magic 0x4711 instead of a crc).
  The newest valid copy (highest seq) of each block wins.
  Block 4 (adaptation / teach-in data, RAM 0xD000C790, 0xA00 B) is made of
  sub-blocks [u16 version][data] with a CRC16-KERMIT table (poly 0x8408 refl),
  init 0xABCD, over nwords*2 bytes from the sub-block start, stored separately.
"""
import struct, sys, os

def _tab(poly):
    t = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ poly if c & 1 else c >> 1
        t.append(c)
    return t

T_ARC, T_KERMIT = _tab(0xA001), _tab(0x8408)

def crc16(b, table=T_ARC, init=0xABCD):
    c = init
    for x in b:
        c = (c >> 8) ^ table[(x ^ c) & 0xFF]
    return c

BLOCK_INFO = {   # id: (name, RAM mirror, buffer size, status)
    1:  ("SW-ID copy (12 B from flash 0x800806F4), checked at every start", "-", 0x0C, "VERIFIED"),
    2:  ("counters / status (d0002740, d0001694, seq, d00019FC)", "various", 0x14, "UNCERTAIN meaning"),
    3:  ("NVM directory (magic 0x4711, block types+lengths)", "-", 0x6C, "VERIFIED"),
    4:  ("ADAPTATION / TEACH-IN data (shift rods, clutches, hydraulics)", "0xD000C790", 0xA00, "VERIFIED container, partial field meaning"),
    5:  ("statistics / operating data (image area)", "0xD000D190", 0x180, "UNCERTAIN"),
    6:  ("fault memory (DTC store, image area)", "0xD000D310", 0x600, "owner = DTC module a00c3..a00ce"),
    7:  ("ident aux (8 B)", "0xD000D910", 0x08, "UNCERTAIN"),
    8:  ("ECU identification: VIN, SW, part no., market, dates", "0xD000D91C", 0xC8, "VERIFIED"),
    9:  ("ident aux (0x1C B)", "0xD000D9E4", 0x1C, "UNCERTAIN"),
    10: ("6-byte block (image area, no callbacks)", "-", 0x06, "UNCERTAIN"),
    11: ("reset/exception log ring buffer (3 x 64 B)", "0xD000192C", 0xC4, "likely"),
    12: ("SW-ID copy #2 (image area)", "-", 0x0C, "VERIFIED (same callbacks as id 1)"),
}

# Block-4 sub-blocks: (RAM start, version, nwords, RAM addr of crc, label)
B4_BASE = 0xD000C790
B4_SUB = [
    (0xC790, 4,    0x5A, 0xC88E, "valve/pressure characteristic curves (hydraulics)"),
    (0xC890, 0x2B, 0x33, 0xC90E, "clutch/hydraulic adaptation values"),
    (0xC910, 10,   0x1F, 0xC972, "pressure offset adaptation table"),
    (0xC974, 2,    6,    0xC992, "teach-in status flags (c976/c978)"),
    (0xC994, 0x15, 0x1C, 0xCA12, "SHIFT RODS: neutral/end/synchro per rod"),
    (0xCA14, 1,    0x16, 0xCA92, "shift-rod related factors"),
    (0xCAB8, 8,    0x11, 0xCADE, "clutch adaptation"),
    (0xCAF8, 8,    0xC0, 0xCCF6, "large adaptation map"),
    (0xCC94, 0xD,  0x1D, 0xCCF4, "adaptation parameters"),
    (0xCDB8, 2,    6,    0xCF16, "small adaptation block"),
    (0xCF18, 0x13, 0x16, 0xCF8E, "clutch touch points / kiss points"),
    (0xCA94, 0x01, 0x01, 0xCAB4, "small status block"),
    (0xCCF8, 0x10, 0x5F, 0xCDB6, "adaptation block CCF8"),
    (0xCF90, 0x04, 0x05, 0xCF9E, "small adaptation block CF90"),
]

def load(path):
    e = open(path, "rb").read()
    if len(e) == 0x20000:
        e = e[:0x4000] + e[0x10000:0x14000]
    if len(e) != 0x8000:
        raise SystemExit("unexpected size %d (need 32 KB or 128 KB dump)" % len(e))
    return e

def parse(e):
    """returns (records{id:(seq,payload,offset)}, image(seq,data,offset), report[])"""
    rep, best, imgs = [], {}, []
    for s in (0, 0x4000):
        for k in range(2):
            o = s + 0x200 + k * 0xF80
            blk = e[o:o + 0xF80]
            seq, = struct.unpack_from("<I", blk, 0)
            ln, ck = struct.unpack_from("<HH", blk, 0xF78)
            nseq, = struct.unpack_from("<I", blk, 0xF7C)
            ok = nseq == (seq ^ 0xFFFFFFFF) and ln <= 0xF80 and crc16(blk[4:4 + ln]) == ck
            rep.append("image slot @%05X seq %08X len %4d crc %04X %s" % (o, seq, ln, ck, "VALID" if ok else "-"))
            if ok:
                imgs.append((seq, blk[4:4 + ln], o))
        base = s + 0x2100
        p = 0
        while p < 62:
            o = base + p * 0x80
            if e[o] != 0x46:
                p += 1
                continue
            rid = e[o + 1]
            npg, = struct.unpack_from("<H", e, o + 2)
            seq, = struct.unpack_from("<I", e, o + 4)
            ln, ck = struct.unpack_from("<HH", e, o + 8)
            last = base + (p + npg - 1) * 0x80
            ok = (1 <= npg and p + npg <= 62 and e[last + 0x7D] == (rid ^ 0xFF)
                  and struct.unpack_from("<H", e, last + 0x7E)[0] == npg
                  and ln < 0x1F01 and crc16(e[o + 12:o + 12 + ln]) == ck)
            rep.append("record @%05X id %2d pages %2d seq %08X len %4d crc %04X %s" %
                       (o, rid, npg, seq, ln, ck, "VALID" if ok else "BAD"))
            if ok and (rid not in best or seq > best[rid][0]):
                best[rid] = (seq, e[o + 12:o + 12 + ln], o)
            p += max(npg, 1)
    img = max(imgs) if imgs else None
    if img:   # split image into its blocks
        data = img[1]
        for rid, off, L in ((5, 4, 0x188), (6, 0x18C, 0x608), (10, 0x794, 6), (12, 0x79A, 0x10)):
            if off + L <= len(data):
                best.setdefault(rid, (img[0], data[off:off + L], img[2] + 4 + off))
    return best, img, rep

def inner_ok(rid, p):
    if rid == 3:
        return struct.unpack_from("<H", p, 0)[0] == 0x4711
    return crc16(p[2:]) == struct.unpack_from("<H", p, 0)[0]

def asc(b):
    return "".join(chr(x) if 32 <= x < 127 else "." for x in b)

IDENT_FIELDS = [   # RAM addr, len, label   (buffer of block 8 = RAM 0xD000D91C)
    (0xD000D950, 17, "VIN (positions 6,7,9 stored as '_')"),
    (0xD000D962, 8,  "software version"),
    (0xD000D96E, 8,  "software version + variant"),
    (0xD000D9A6, 11, "Porsche part number (calibration dataset)"),
    (0xD000D930, 12, "HW/SW ident string"),
    (0xD000D93C, 12, "production status"),
    (0xD000D976, 4,  "market variant (RW01 = Rest of World, US01 = USA)"),
    (0xD000D99A, 3,  "last programming date (DD MM YY, BCD)"),
    (0xD000D920, 3,  "date #2 (DD MM YY, BCD)"),
    (0xD000D96A, 4,  "tester / workshop id (UNCERTAIN)"),
    (0xD000D97C, 17, "second 17-char field (spaces = empty)"),
]

def decode_ident(p):
    buf = p[2:]
    out = []
    for a, l, lab in IDENT_FIELDS:
        o = a - 0xD000D91C
        b = buf[o:o + l]
        out.append((lab, b))
    return out

def decode_b4(p):
    buf = p[6:]           # u16 inner crc, u16 used-length, u16 pad, then RAM image of 0xD000C790
    used, = struct.unpack_from("<H", p, 2)
    subs = []
    for st, ver, nw, ca, lab in B4_SUB:
        o = st - 0xC790
        v, = struct.unpack_from("<H", buf, o)
        stored, = struct.unpack_from("<H", buf, ca - 0xC790)
        calc = crc16(buf[o:o + nw * 2], T_KERMIT)
        subs.append((st, ver, v, stored, calc, lab))
    rods = []
    for r in range(4):
        v = struct.unpack_from("<5h", buf, 0x208 + 12 * r)
        rods.append(v)
    marker, = struct.unpack_from("<H", buf, 0x238)
    flags = struct.unpack_from("<HH", buf, 0xC976 - 0xC790)
    return used, subs, rods, marker, flags

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    e = load(sys.argv[1])
    best, img, rep = parse(e)
    print("== EEPROM:", os.path.basename(sys.argv[1]))
    for s in (0, 0x4000):
        print("sector %d header pages:" % (s // 0x4000), " ".join(e[s + i * 0x80:s + i * 0x80 + 4].hex() for i in range(4)))
    print("\n-- slots / records (all copies) --")
    for r in rep:
        print("  " + r)
    print("\n-- current blocks (newest valid copy) --")
    for rid in sorted(best):
        seq, p, off = best[rid]
        name, ram, size, st = BLOCK_INFO.get(rid, ("?", "?", 0, ""))
        print("  id %2d seq %08X len %4d inner-crc %-4s  %s  [RAM %s] (%s)" %
              (rid, seq, len(p), "OK" if inner_ok(rid, p) else "BAD", name, ram, st))
    if 8 in best:
        print("\n-- identification (block 8) --")
        for lab, b in decode_ident(best[8][1]):
            txt = asc(b) if "date" not in lab else "%02x.%02x.20%02x" % (b[0], b[1], b[2])
            print("  %-48s %-20s %s" % (lab, txt, b.hex(" ")))
    if 1 in best:
        print("\n  SW-ID stored in block 1: %s  (must equal flash 0x800806F4, else adaptations reset at start)"
              % asc(best[1][1][2:10]))
    if 4 in best:
        used, subs, rods, marker, flags = decode_b4(best[4][1])
        print("\n-- adaptation / teach-in data (block 4, used %d bytes) --" % used)
        for st, ver, v, stored, calc, lab in subs:
            print("  sub-block @%04X ver %3d (exp %3d) crc %04X %-5s %s" %
                  (st, v, ver, stored, "OK" if stored == calc else "BAD", lab))
        taught = marker == 0xA55A
        print("\n  shift-rod teach-in marker (0xA55A) : %04X -> %s" %
              (marker, "TAUGHT" if taught else "NOT TAUGHT (defaults)"))
        print("  rod  neutral   end(+)  synchro(+)   end(-)  synchro(-)   [mm, 1/256 mm units]")
        for i, v in enumerate(rods):
            print("   %d   %+6.2f   %+6.2f   %+6.2f    %+6.2f   %+6.2f" % ((i + 1,) + tuple(x / 256 for x in v)))
        print("  teach-in status flags (sub-block 0xC974): %04X %04X  (0 = nothing learned)" % flags)
    if len(sys.argv) > 3 and sys.argv[2] == "--dump-blocks":
        d = sys.argv[3]
        os.makedirs(d, exist_ok=True)
        for rid, (seq, p, off) in best.items():
            open(os.path.join(d, "block%02d.bin" % rid), "wb").write(p)
        print("\nblocks written to", d)

if __name__ == "__main__":
    main()
