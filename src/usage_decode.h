// usage_decode.inc - PDK3000 (TC1766) usage statistics / logs from EEPROM blocks 5, 2, 11.
// Source: firmware RE of SW G070, see usage_stats.md. Only fields at least INFERRED are printed.
//
// Inputs (all little-endian, as stored in the EEPROM emulation):
//   b5  = block 5 RAM image from 0xD000D190 (= block payload + 6; >= 0x108 bytes used)
//   b2  = block 2 buffer (= block payload + 2, i.e. without the inner CRC; 0x14 bytes)
//   b9  = block 9 buffer (payload + 2) - not decoded (meaning UNCERTAIN), accepted for API symmetry
//   b11 = block 11 buffer (payload + 2): u8 newest slot index, 3 pad bytes, 3 x 64-byte reset records
// No Windows dependencies.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pdkuse {

static inline uint32_t u32le(const std::vector<uint8_t>& v, size_t o) {
    return (uint32_t)v[o] | ((uint32_t)v[o+1] << 8) | ((uint32_t)v[o+2] << 16) | ((uint32_t)v[o+3] << 24);
}
static inline uint16_t u16le(const std::vector<uint8_t>& v, size_t o) { return (uint16_t)(v[o] | (v[o+1] << 8)); }

// value + bit-inverted copy (FUN_a00da800 / a00c8c2e style)
static inline bool pairOk(const std::vector<uint8_t>& v, size_t o, size_t oc, uint32_t* val) {
    uint32_t a = u32le(v, o), c = u32le(v, oc);
    *val = a;
    return (a ^ c) == 0xFFFFFFFFu;
}

// 8 x (byte,byte) event log + u16 count/seed + u16 sum (FUN_a00da6d8 / FUN_a00be004 style)
static std::string logLines(const char* name, const std::vector<uint8_t>& v, size_t o, size_t oCount, bool countIsByte, size_t oSum) {
    char m[200]; std::string r;
    unsigned cnt = countIsByte ? v[oCount] : u16le(v, oCount);
    int n = 0;
    std::string items;
    for (int i = 0; i < 8; i++) {
        uint8_t a = v[o + 2*i], b = v[o + 2*i + 1];
        if (a || b) { snprintf(m, sizeof m, " %02X/%02X", a, b); items += m; n++; }
    }
    if (n == 0 && cnt == 0)
        snprintf(m, sizeof m, "[USE] %s: empty (no events logged)\n", name);
    else
        snprintf(m, sizeof m, "[USE] %s: %u event(s) logged, last 8 (id/qualifier, raw):%s\n", name, cnt, items.c_str());
    r += m;
    (void)oSum;
    return r;
}

static std::string usageReport(const std::vector<uint8_t>& b5, const std::vector<uint8_t>& b2,
                               const std::vector<uint8_t>& b9, const std::vector<uint8_t>& b11) {
    (void)b9;
    std::string r; char m[256];
    // ---------- block 5 ----------
    if (b5.size() >= 0x108) {
        uint32_t odo = 0, ikm = 0;
        bool okOdo = pairOk(b5, 0x20, 0x24, &odo);        // 0xD000D1B0 / B4
        bool okIkm = pairOk(b5, 0x28, 0x2C, &ikm);        // 0xD000D1B8 / BC
        snprintf(m, sizeof m, "[USE] last odometer seen on CAN: %u km%s\n", odo, okOdo ? "" : " (copy check FAILED)"); r += m;
        snprintf(m, sizeof m, "[USE] distance integrated by the TCU itself (vehicle speed, while powered): %u km%s\n",
                 ikm, okIkm ? "" : " (copy check FAILED)"); r += m;
        // operating time counters, 1 s per count (task 100 ms, every 10th call) - INFERRED unit
        uint32_t t0 = 0, t1 = 0, t2 = 0;
        bool ok0 = pairOk(b5, 0xCC, 0xD8, &t0);           // 0xD000D25C / D268
        bool ok1 = pairOk(b5, 0xD0, 0xDC, &t1);           // 0xD000D260 / D26C
        bool ok2 = pairOk(b5, 0xD4, 0xE0, &t2);           // 0xD000D264 / D270
        if (ok0 && ok1 && ok2) {
            double tot = ((double)t0 + t1 + t2) / 3600.0;
            snprintf(m, sizeof m, "[USE] TCU operating time: %.0f h (%.0f h in state A, %.0f h in state B; state meaning uncertain)\n",
                     tot, (t0 + (double)t1) / 3600.0, t2 / 3600.0); r += m;
            if (okIkm && tot > 1.0) {
                snprintf(m, sizeof m, "[USE] average speed over TCU life: %.1f km/h (TCU distance / operating time)\n", ikm / tot); r += m;
            }
        } else {
            r += "[USE] TCU operating time: counters invalid (copy check failed)\n";
        }
        // EDS3/EDS4 coil-temperature model (4 learned floats with complements, 0xD000D190..D1AF)
        bool okModel = true; uint32_t dummy;
        for (int i = 0; i < 4; i++) okModel = okModel && pairOk(b5, 8*i, 8*i + 4, &dummy);
        snprintf(m, sizeof m, "[USE] EDS3/EDS4 coil-temperature model parameters: %s\n", okModel ? "stored, valid" : "INVALID (copy check failed)"); r += m;
        r += logLines("event log 1 (0xD000D1C8)", b5, 0x38, 0x48, false, 0x4A);
        r += logLines("safety-monitor log (0xD000D244)", b5, 0xB4, 0xC4, true, 0xC6);
    } else {
        r += "[USE] block 5 missing or too short\n";
    }
    // ---------- block 2 ----------
    if (b2.size() >= 0x12) {
        snprintf(m, sizeof m, "[USE] EEPROM-emulation write counters: image sequence %u, write jobs %u\n",
                 u32le(b2, 10), u32le(b2, 14)); r += m;
    }
    // ---------- block 11 ----------
    if (b11.size() >= 4 + 3*64) {
        int used = 0;
        for (int i = 0; i < 3; i++) {
            const uint8_t* s = &b11[4 + 64*i];
            bool any = false;
            for (int k = 0; k < 64; k++) if (s[k]) { any = true; break; }
            if (any) used++;
        }
        unsigned newest = b11[0] < 3 ? b11[0] : 0;
        const uint8_t* s = &b11[4 + 64*newest];
        snprintf(m, sizeof m, "[USE] reset/trap log: %d of 3 slots used; newest slot %u header %02X%02X%02X%02X %02X%02X%02X%02X (raw, for experts)\n",
                 used, newest, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]); r += m;
    }
    return r;
}

} // namespace pdkuse
