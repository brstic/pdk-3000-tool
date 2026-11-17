// bits3063.inc - meaning of freeze-frame / measurement ID 3063 (RAM 0xD000B384, u32) for PdkTool.
// Source: firmware RE , image SW G070 (code identical in the owner's int_flash dump).
//
// ID 3063 = "adaptation sub-block re-initialised" word. At power-up the init chain
// FUN_a00e319a -> a00e293c / a00e2b7a / a00e2c9e / a00e2d3e / a00e2de2 / a00e2e86 / a00e2f2e /
// a00e2fd2 / a00e305c checks every sub-block of EEPROM block 4 (RAM 0xD000C790):
//   crc = CRC16-KERMIT(init 0xABCD) over nwords*2 bytes from the sub-block's version word
//   (FUN_a00e2912), compared with the stored CRC. On mismatch FUN_a00e212a either migrates the
//   block (older version, cal-allowed step) or loads calibration defaults; when it loads
//   defaults it returns 1 and the firmware SETS the bit below (bit cleared when CRC is OK).
// Bit 15 = summary: set when any other bit is set (0xA00E31F0).
// Use: (ID3063 & cal 0xA004F5B4 [=0xFFFFFFFF]) != 0  ->  DEM event 0x93 = DTC P174C (0xA00E35CA).
// The masked check (& 0xDA0007FF) at 0xA00E3504 additionally blocks adaptation (0xD000B388 = 0x1FFF).
// Bits not listed are never written by this software (always 0).
//
// Practical meaning: a set bit = that part of the learned data was lost and replaced by
// defaults at the last power-up -> run the matching teach-in (rods / hydraulics / clutches).
#pragma once

static const char *BITS3063[32] = {
    /* 0 */ "sub-block C790 (v4, 0x5A words, CRC @C88E) valve/pressure characteristic curves reset to defaults",
    /* 1 */ "sub-block C890 (v43, 0x33 words, CRC @C90E) clutch/hydraulic adaptation values reset to defaults",
    /* 2 */ "sub-block C910 (v10, 0x1F words, CRC @C972) pressure offset adaptation table reset to defaults",
    /* 3 */ "sub-block CA94 (v1, 1 word, CRC @CAB4) small status block reset to defaults",
    /* 4 */ "sub-block CAF8 (v8, 0xC0 words, CRC @CCF6) large adaptation map reset to defaults",
    /* 5 */ "sub-block C994 (v21, 0x1C words, CRC @CA12) SHIFT-ROD teach-in (neutral/end/synchro) reset to defaults",
    /* 6 */ "sub-block CA14 (v1, 0x16 words, CRC @CA92) shift-rod factors reset to defaults",
    /* 7 */ "sub-block CAB8 (v8, 0x11 words, CRC @CADE) clutch adaptation reset to defaults",
    /* 8 */ "sub-block C974 (v2, 6 words, CRC @C992) teach-in status flags reset to defaults",
    /* 9 */ "sub-block CCF8 (v16, 0x5F words, CRC @CDB6) adaptation block reset to defaults",
    /*10 */ "sub-block CDB8 (v2, 6 words, CRC @CF16) small adaptation block reset to defaults",
    /*11 */ nullptr, /*12*/ nullptr, /*13*/ nullptr, /*14*/ nullptr,
    /*15 */ "summary: at least one adaptation sub-block was reset to defaults",
    /*16 */ nullptr, /*17*/ nullptr, /*18*/ nullptr, /*19*/ nullptr,
    /*20 */ nullptr, /*21*/ nullptr, /*22*/ nullptr, /*23*/ nullptr, /*24*/ nullptr,
    /*25 */ "sub-block CC94 (v13, 0x1D words, CRC @CCF4) adaptation parameters reset to defaults",
    /*26 */ nullptr,
    /*27 */ "sub-block CF18 (v19, 0x16 words, CRC @CF8E) clutch touch-point (kiss-point) adaptation reset to defaults",
    /*28 */ "sub-block CF90 (v4, 5 words, CRC @CF9E) small adaptation block reset to defaults",
    /*29 */ nullptr, /*30*/ nullptr, /*31*/ nullptr,
};
