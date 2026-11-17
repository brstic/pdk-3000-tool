// ============================================================================
// PDK3000 (Continental 5WP54023 / TC1766) EEPROM-emulation parser + checksums.
// Self-contained, no Windows dependencies. Paste into PdkTool (C++17).
// Verified on two dumps (SW G088 RoW unit and SW G070 US unit): every image
// slot, every 'F' record, every block inner CRC and every adaptation
// sub-block CRC matches.
//
//   Input : 32 KB (sector0+sector1) or 128 KB raw DFlash dump (0xAFE00000..).
//   pdkee::Parsed p; pdkee::parse(data, len, p);
//   p.blocks[id].payload  -> newest valid copy of block <id>
//   pdkee::identOf(p), pdkee::rodsOf(p, ...), pdkee::teachSummary(p)
// ============================================================================
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>

namespace pdkee {

// CRC16, reflected, table driven, init 0xABCD.
//  ARC    (poly 0xA001): sector image slots, 'F' records, block inner CRC
//  KERMIT (poly 0x8408): adaptation (block 4) sub-blocks
inline uint16_t crc16(const uint8_t *p, size_t n, uint16_t poly, uint16_t init = 0xABCD) {
    static uint16_t tA[256], tK[256]; static bool done = false;
    if (!done) {
        for (int i = 0; i < 256; i++) {
            uint16_t a = (uint16_t)i, k = (uint16_t)i;
            for (int b = 0; b < 8; b++) { a = (a & 1) ? (a >> 1) ^ 0xA001 : a >> 1;
                                          k = (k & 1) ? (k >> 1) ^ 0x8408 : k >> 1; }
            tA[i] = a; tK[i] = k;
        }
        done = true;
    }
    const uint16_t *t = (poly == 0x8408) ? tK : tA;
    uint16_t c = init;
    for (size_t i = 0; i < n; i++) c = (uint16_t)((c >> 8) ^ t[(p[i] ^ c) & 0xFF]);
    return c;
}
inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

struct Block { uint32_t seq = 0; std::vector<uint8_t> payload; bool innerOk = false; };
struct Parsed {
    bool ok = false;
    std::map<int, Block> blocks;          // newest valid copy per block id
    int records = 0, badRecords = 0, imageSlotsValid = 0;
    std::string error;
};

// Block payload = u16 inner CRC (ARC over payload[2..]) + buffer. Block 3 = directory, magic 0x4711.
inline bool innerOk(int id, const std::vector<uint8_t> &p) {
    if (p.size() < 2) return false;
    if (id == 3) return rd16(p.data()) == 0x4711;
    return crc16(p.data() + 2, p.size() - 2, 0xA001) == rd16(p.data());
}

inline bool parse(const uint8_t *in, size_t n, Parsed &out) {
    out = Parsed();
    std::vector<uint8_t> e;
    if (n == 0x20000) { e.assign(in, in + 0x4000); e.insert(e.end(), in + 0x10000, in + 0x14000); }
    else if (n == 0x8000) e.assign(in, in + n);
    else { out.error = "dump must be 32 KB or 128 KB"; return false; }
    uint32_t imgSeq = 0; std::vector<uint8_t> img;
    for (int s = 0; s < 0x8000; s += 0x4000) {
        for (int k = 0; k < 2; k++) {                         // image slots
            const uint8_t *b = &e[s + 0x200 + k * 0xF80];
            uint32_t seq = rd32(b); uint16_t len = rd16(b + 0xF78), ck = rd16(b + 0xF7A);
            if (rd32(b + 0xF7C) == (seq ^ 0xFFFFFFFFu) && len <= 0xF80 && crc16(b + 4, len, 0xA001) == ck) {
                out.imageSlotsValid++;
                if (img.empty() || seq > imgSeq) { imgSeq = seq; img.assign(b + 4, b + 4 + len); }
            }
        }
        int base = s + 0x2100;                                // record area: 62 pages of 0x80
        for (int pg = 0; pg < 62;) {
            const uint8_t *r = &e[base + pg * 0x80];
            if (r[0] != 0x46) { pg++; continue; }
            int id = r[1]; uint16_t np = rd16(r + 2), len = rd16(r + 8), ck = rd16(r + 10);
            uint32_t seq = rd32(r + 4);
            bool good = np >= 1 && pg + np <= 62 && len < 0x1F01;
            if (good) {
                const uint8_t *last = &e[base + (pg + np - 1) * 0x80];
                good = last[0x7D] == (uint8_t)(id ^ 0xFF) && rd16(last + 0x7E) == np &&
                       base + pg * 0x80 + 12 + len <= (int)e.size() && crc16(r + 12, len, 0xA001) == ck;
            }
            out.records++;
            if (!good) out.badRecords++;
            else {
                Block &bl = out.blocks[id];
                if (bl.payload.empty() || seq > bl.seq) { bl.seq = seq; bl.payload.assign(r + 12, r + 12 + len); }
            }
            pg += np ? np : 1;
        }
    }
    // image = 4-byte header + block 5 (0x188) + block 6 (0x608) + block 10 (6) + block 12 (0x10)
    struct { int id, off, len; } IM[] = {{5, 4, 0x188}, {6, 0x18C, 0x608}, {10, 0x794, 6}, {12, 0x79A, 0x10}};
    for (auto &m : IM)
        if (m.off + m.len <= (int)img.size() && !out.blocks.count(m.id)) {
            Block &bl = out.blocks[m.id]; bl.seq = imgSeq;
            bl.payload.assign(img.begin() + m.off, img.begin() + m.off + m.len);
        }
    for (auto &kv : out.blocks) kv.second.innerOk = innerOk(kv.first, kv.second.payload);
    out.ok = !out.blocks.empty();
    if (!out.ok) out.error = "no valid record found";
    return out.ok;
}

// ---- identification (block 8 buffer = RAM 0xD000D91C, buffer starts at payload+2)
struct Ident { std::string vin, sw, swVariant, partNo, hwString, market, progDate; };
inline std::string fld(const std::vector<uint8_t> &p, uint32_t ram, int len) {
    size_t o = 2 + (ram - 0xD000D91Cu); std::string s;
    for (int i = 0; i < len && o + i < p.size(); i++) { char c = (char)p[o + i]; s += (c >= 32 && c < 127) ? c : '.'; }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
inline bool identOf(const Parsed &ps, Ident &id) {
    auto it = ps.blocks.find(8); if (it == ps.blocks.end() || it->second.payload.size() < 0xCA) return false;
    const auto &p = it->second.payload;
    id.vin = fld(p, 0xD000D950, 17);      // positions 6,7,9 are stored as '_'
    id.sw = fld(p, 0xD000D962, 8);
    id.swVariant = fld(p, 0xD000D96E, 8);
    id.partNo = fld(p, 0xD000D9A6, 11);
    id.hwString = fld(p, 0xD000D930, 12);
    id.market = fld(p, 0xD000D976, 4);    // RW01 = Rest of World, US01 = USA
    size_t o = 2 + (0xD000D99A - 0xD000D91C);
    char d[24]; snprintf(d, sizeof d, "%02x.%02x.20%02x", p[o], p[o + 1], p[o + 2]);   // DD.MM.YY BCD
    id.progDate = d;
    return true;
}

// ---- shift-rod teach-in data (block 4 buffer = RAM 0xD000C790, buffer at payload+6)
// rod record (12 bytes) at buffer+0x208: neutral, end(+), synchro(+), end(-), synchro(-) [1/256 mm]
// marker 0xA55A at buffer+0x238 = shift rods taught. Defaults: 0, 1920, 512, -1920, -512 (7.5/2.0 mm).
struct RodData { bool taught = false; double v[4][5] = {}; uint16_t flagsA = 0, flagsB = 0; };
inline bool rodsOf(const Parsed &ps, RodData &rd) {
    auto it = ps.blocks.find(4); if (it == ps.blocks.end() || it->second.payload.size() < 6 + 0x210) return false;
    const uint8_t *b = it->second.payload.data() + 6;
    rd.taught = rd16(b + 0x238) == 0xA55A;
    for (int r = 0; r < 4; r++) for (int k = 0; k < 5; k++)
        rd.v[r][k] = (int16_t)rd16(b + 0x208 + 12 * r + 2 * k) / 256.0;
    rd.flagsA = rd16(b + (0xC976 - 0xC790)); rd.flagsB = rd16(b + (0xC978 - 0xC790));
    return true;
}

// ---- adaptation sub-block check (block 4)
struct SubBlock { uint32_t ram; uint16_t ver, nwords, crcRam; const char *what; };
static const SubBlock B4SUB[] = {
    {0xD000C790, 4, 0x5A, 0xC88E, "valve/pressure characteristic curves"},
    {0xD000C890, 0x2B, 0x33, 0xC90E, "clutch/hydraulic adaptation"},
    {0xD000C910, 10, 0x1F, 0xC972, "pressure offset adaptation"},
    {0xD000C974, 2, 6, 0xC992, "teach-in status flags"},
    {0xD000C994, 0x15, 0x1C, 0xCA12, "shift rods (neutral/end/synchro)"},
    {0xD000CA14, 1, 0x16, 0xCA92, "shift-rod factors"},
    {0xD000CAB8, 8, 0x11, 0xCADE, "clutch adaptation"},
    {0xD000CAF8, 8, 0xC0, 0xCCF6, "large adaptation map"},
    {0xD000CC94, 0xD, 0x1D, 0xCCF4, "adaptation parameters"},
    {0xD000CDB8, 2, 6, 0xCF16, "small adaptation block"},
    {0xD000CF18, 0x13, 0x16, 0xCF8E, "clutch touch points"},
    {0xD000CA94, 1, 1, 0xCAB4, "small status block"},
    {0xD000CCF8, 0x10, 0x5F, 0xCDB6, "adaptation block CCF8"},
    {0xD000CF90, 4, 5, 0xCF9E, "small adaptation block CF90"},
};
inline int checkSubBlocks(const Parsed &ps, std::string &report) {   // returns number of bad sub-blocks
    auto it = ps.blocks.find(4); if (it == ps.blocks.end()) { report = "block 4 missing"; return -1; }
    const auto &p = it->second.payload; const uint8_t *b = p.data() + 6; int bad = 0;
    for (const auto &s : B4SUB) {
        size_t o = s.ram - 0xD000C790u, co = (0xD0000000u | s.crcRam) - 0xD000C790u;
        if (6 + co + 2 > p.size()) { bad++; continue; }
        bool ok = rd16(b + o) == s.ver && crc16(b + o, s.nwords * 2, 0x8408) == rd16(b + co);
        if (!ok) bad++;
        char ln[128]; snprintf(ln, sizeof ln, "  %-38s %s\n", s.what, ok ? "OK" : "BAD");
        report += ln;
    }
    return bad;
}

// After editing a block-4 buffer, re-seal: sub-block CRCs, then block inner CRC.
// (The 'F' record header CRC/seq/trailer must be rebuilt by whoever writes DFlash.)
inline void resealBlock4(std::vector<uint8_t> &p) {
    uint8_t *b = p.data() + 6;
    for (const auto &s : B4SUB) {
        size_t o = s.ram - 0xD000C790u, co = (0xD0000000u | s.crcRam) - 0xD000C790u;
        uint16_t c = crc16(b + o, s.nwords * 2, 0x8408); b[co] = c & 0xFF; b[co + 1] = c >> 8;
    }
    uint16_t c = crc16(p.data() + 2, p.size() - 2, 0xA001); p[0] = c & 0xFF; p[1] = c >> 8;
}

// One-line plain-language summary of what is learned in this unit.
inline std::string teachSummary(const Parsed &ps) {
    RodData rd; std::string s;
    if (!rodsOf(ps, rd)) return "adaptation block not found";
    s += rd.taught ? "Shift rods: TAUGHT. " : "Shift rods: NOT taught (default 7.5/2.0 mm values). ";
    s += (rd.flagsA || rd.flagsB) ? "Clutch/hydraulic teach flags set." : "Clutch/hydraulic teach flags: none.";
    return s;
}

} // namespace pdkee
