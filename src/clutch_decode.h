// clutch_decode.inc - PDK3000 (TC1766 / 5WP54023, SW G070/G088) clutch + hydraulic adaptation decoder.
// Source: firmware RE, see clutch_adaptation.md. Input: block-4 RAM image, i.e. the bytes starting at
// RAM 0xD000C790 (EEPROM block 4 payload+6, or 23-read RAM mirror), at least 0x7B0 bytes.
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>

namespace pdkclu {

inline int16_t s16(const std::vector<uint8_t> &b, int o) { return (int16_t)(b[o] | (b[o + 1] << 8)); }
inline uint32_t u32(const std::vector<uint8_t> &b, int o) {
    return (uint32_t)b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

// Offsets relative to RAM 0xD000C790.
enum : int {
    O_TP1 = 0x102, O_TP2 = 0x104,          // C892/C894 touch-point adaptation, -1 = not learned (default)
    O_TP1REF = 0x106, O_TP2REF = 0x108,    // C896/C898 value at last teach-in (default 0)
    O_FF1 = 0x10C, O_FF2 = 0x114,          // C89C/C8A4 fast-fill time, 4 x s16 per clutch (max 200/180/150/120)
    O_EV1 = 0x12C, O_EV2 = 0x14C,          // C8BC/C8DC events since last teach-in update
    O_FLG1 = 0x1E6, O_FLG2 = 0x1E8,        // C976/C978 clutch adaptation status word per clutch
    O_EDSOFS = 0x188,                      // C918.. 24 x s16 pressure-offset table, default -1024
    O_TPHIST = 0x336,                      // CAC6.. 5 x K1, then CAD0.. 5 x K2 (history)
    O_ONL1 = 0x506, O_ONL2 = 0x50A,        // CC96/CC9A u32 online touch-point adaptation count
    O_HYD = 0x78A,                         // CF1A..CF24 6 x s16 hydraulic learned points
    O_TPA1 = 0x798, O_TPA2 = 0x79C,        // CF28/CF2C touch-point adaptation (env ID 3034/3035 source)
    O_HYDST = 0x79E,                       // CF2E u32 hydraulic adaptation status bits
    O_NEED = 0x7B0
};
static const int16_t HYD_DEFAULT[6] = {1075, 819, 1313, 819, 0, 0};   // as written by the adaptation reset

// Status word bits (low byte = touch point, high byte = fast-fill time), VERIFIED setters:
//   0x0001/0x0100 learned OK (FUN_a0161f2c)        0x0004/0x0400 step aborted (FUN_a0162042)
//   0x0008/0x0800 deviation counter over limit     0x0010/0x1000 result clamped at max (FUN_a0161cc4)
//   0x0040/0x8000 time-out (0x8040 together, FUN_a00e6d6e)   0x0200 abort, conditions lost (FUN_a00e7816..)
// The firmware treats mask 0x8E8E as "error from last attempt" (FUN_a00e631c).
inline std::string flagText(uint16_t f) {
    std::string s;
    auto add = [&](const char *t) { if (!s.empty()) s += ", "; s += t; };
    add((f & 0x0001) ? "touch point learned" : "touch point NOT learned");
    add((f & 0x0100) ? "fast-fill learned" : "fast-fill NOT learned");
    if (f & 0x0404) add("aborted");
    if (f & 0x0808) add("deviation limit exceeded");
    if (f & 0x1010) add("clamped at limit");
    if (f & 0x8040) add("time-out");
    if (f & 0x0200) add("conditions lost");
    return s;
}

inline std::string clutchReport(const std::vector<uint8_t> &b4) {
    if (b4.size() < (size_t)O_NEED) return "[CLU] block 4 too short\n";
    std::string r; char m[256];
    uint16_t fl[2] = {(uint16_t)s16(b4, O_FLG1), (uint16_t)s16(b4, O_FLG2)};
    int16_t tp[2] = {s16(b4, O_TP1), s16(b4, O_TP2)};
    int16_t tpr[2] = {s16(b4, O_TP1REF), s16(b4, O_TP2REF)};
    int clutchOk = 0;
    for (int k = 0; k < 2; k++) {
        bool ok = (fl[k] & 0x0101) == 0x0101 && tp[k] != -1;
        clutchOk += ok;
        snprintf(m, sizeof m, "[CLU] K%d: status 0x%04X = %s\n", k + 1, fl[k], flagText(fl[k]).c_str()); r += m;
        if (tp[k] == -1) snprintf(m, sizeof m, "[CLU] K%d: touch point -1 (default, not learned)\n", k + 1);
        else snprintf(m, sizeof m, "[CLU] K%d: touch point %d (at teach-in %d, drift %+d) raw\n", k + 1, tp[k], tpr[k], tp[k] - tpr[k]);
        r += m;
        int o = k ? O_FF2 : O_FF1;
        snprintf(m, sizeof m, "[CLU] K%d: fast-fill times %d %d %d %d (raw, index 0..3; limits 200 180 150 120)\n", k + 1,
                 s16(b4, o), s16(b4, o + 2), s16(b4, o + 4), s16(b4, o + 6)); r += m;
        int h = O_TPHIST + 10 * k;
        snprintf(m, sizeof m, "[CLU] K%d: touch-point history %d %d %d %d %d   events since update %d   online adaptations %u\n",
                 k + 1, s16(b4, h), s16(b4, h + 2), s16(b4, h + 4), s16(b4, h + 6), s16(b4, h + 8),
                 s16(b4, k ? O_EV2 : O_EV1), u32(b4, k ? O_ONL2 : O_ONL1)); r += m;
    }
    snprintf(m, sizeof m, "[CLU] touch-point adaptation (kiss point) K1 / K2: %d / %d\n", s16(b4, O_TPA1), s16(b4, O_TPA2)); r += m;

    uint32_t hs = u32(b4, O_HYDST);
    bool hydOk = (hs & 0x60000000u) == 0x20000000u;
    int hydDef = 0;
    std::string pts;
    for (int i = 0; i < 6; i++) { int16_t v = s16(b4, O_HYD + 2 * i); hydDef += (v == HYD_DEFAULT[i]); pts += " " + std::to_string(v); }
    snprintf(m, sizeof m, "[CLU] hydraulics: status 0x%08X = %s; learned points%s%s\n", hs,
             hydOk ? "adaptation VALID" : ((hs & 0x40000000u) ? "adaptation marked INVALID" : "adaptation NOT valid"),
             pts.c_str(), hydDef == 6 ? " (all defaults)" : ""); r += m;
    int learned = 0;
    for (int i = 0; i < 24; i++) learned += (s16(b4, O_EDSOFS + 2 * i) != -1024);
    snprintf(m, sizeof m, "[CLU] pressure-offset table: %d of 24 points moved from default -1024\n", learned); r += m;

    if (!hydOk) r += "[CLU] => run HYDRAULIC teach-in (0x22), then CLUTCH teach-in (0x21). Clutch teach-in is refused without it.\n";
    else if (clutchOk < 2) r += "[CLU] => hydraulics OK, run CLUTCH teach-in (0x21).\n";
    else r += "[CLU] => clutch and hydraulic adaptation present.\n";
    return r;
}

}  // namespace pdkclu
