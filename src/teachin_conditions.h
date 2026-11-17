// teachin_conditions.inc - PDK3000 (TC1766 / 5WP54023) teach-in routine knowledge for PdkTool.
// Source: firmware RE of SW G070 and G088 (identical thresholds). See teachin_thresholds.md.
//
// Routine LIDs (KWP 31/32/33 <LID>, request data = exactly 1 byte):
//   0x20 shift rods, 0x21 clutches, 0x22 hydraulics.
//   0xD0 = actuator diagnosis (Stellglieddiagnose; 31 D0 <flags_hi> <flags_lo>), not a teach-in.
// 33 <LID> response: 73 LID status cond[4 BE] steps[4 BE] progress v1[2] v2[2] v3[2] v4[2]
//   status: 0 not started, 1 running, 2 finished/idle, 3 start conditions not met, 7 aborted/error.
//   cond bit i (1 = OK) is in byte r[6 - i/8], bit (i % 8).
// Recommended order on this firmware: rods (0x20) -> hydraulics (0x22) -> clutches (0x21),
// because clutch condition bit 12 requires a valid hydraulic adaptation.
#pragma once

struct TeachCond { int bit; const char *text; };

static const unsigned char TEACH_LID_RODS   = 0x20;
static const unsigned char TEACH_LID_CLUTCH = 0x21;
static const unsigned char TEACH_LID_HYDR   = 0x22;

static const char *teachStatusText(unsigned char s) {
    switch (s) {
    case 0: return "not started";
    case 1: return "running";
    case 2: return "finished";
    case 3: return "start conditions not met";
    case 7: return "aborted / error";
    default: return "unknown status";
    }
}

// Shift rods (LID 0x20). Word = ~RAM 0xD0009CE8. Start is refused (NRC 0x22) if any bit in 0x0018009E fails.
static const TeachCond TEACH_RODS[] = {
    { 0, "Shift rods taught (marker 0xA55A in EEPROM block 4, RAM 0xD000C9C8); fails while no valid shift-rod teach-in is stored" },
    { 1, "No shift inhibit active (internal inhibit flag clear)" },
    { 2, "Engine running: engine speed >= 350 rpm (cal 0xA004CBAA)" },
    { 3, "Car standing: output speed <= 100 (cal 0xA004CBAC)" },
    { 4, "Selector lever in P" },
    { 5, "Both clutches open" },
    { 6, "TCU has access to clutch actuation" },
    { 7, "Cooling-oil volume OK" },
    { 8, "End and neutral positions found in time (counter < 1280, cal 0xA004CBA4)" },
    { 9, "End positions valid: 6.00..12.00 mm each side, travel 12..24 mm (cal 0xA004CBEA/E8/E4/E2)" },
    {10, "End/neutral learning: regulating the rod into neutral did not complete (step 6 failed)" },
    {11, "Rod can be moved out of neutral (> 1.00 mm, cal 0xA004CBE0)" },
    {12, "Neutral position valid (within 1.50 mm; fault if > 3.00 mm, cal 0xA004CBDE/E6)" },
    {13, "Synchro point found (search window +/-6.25 mm, cal 0xA004CBCE)" },
    {14, "Synchro learning: regulating the rod back into neutral did not complete (step 6 failed)" },
    {15, "Synchro point valid: confirmed within 0.50 mm of the learned point before time-out 5000 (cal 0xA004CC04); also set on synchro step 8 failure" },
    {16, "Synchro distance valid: |rod reference (neutral) position - synchro point found| must be > 0.75 and < 5.00 mm (FUN_a0113550, cal 0xA004CBC8/0xA004CBC6)" },
    {17, "End-position search succeeded within the retry budget: each failed round (rod end < 4.00 mm, or travel between ends <= 8.00 mm) adds 100, error above 1000 (cal 0xA004CBC2/CBC0/CBBA/CBBE)" },
    {18, "Gearbox oil temperature 35..100 C (alt. window 30..50 C, cal 0xA004CBB6/B4/B2/B0)" },
    {20, "Shift program in a teach-in state" },
    {24, "Routine finished without an unknown abort" },
};

// Clutches (LID 0x21). Word = ~RAM 0xD000829C, mask cal 0xA0049372 = 0xFFFF.
static const TeachCond TEACH_CLUTCH[] = {
    { 0, "Clutch teach-in enabled in calibration (cal 0xA00493DE)" },
    { 1, "Request from tester / prerequisite OK" },
    { 2, "Gearbox oil temperature 60..100 C (cal 0xA00493B8/BA)" },
    { 3, "Engine speed 300..900 rpm (cal 0xA00493AE/B0)" },
    { 4, "Filtered engine torque within -400..+400 raw (~ +/-40 Nm, cal 0xA00493C0/C2)" },
    { 5, "Selector lever in P" },
    { 6, "No active diagnostic fault / adaptation request" },
    { 7, "Accelerator pedal released (< 20 raw, cal 0xA0049370)" },
    { 8, "No high clutch load (load index < 1, cal 0xA00493DB)" },
    { 9, "No emergency (Notfahr) dataset flashed" },
    {10, "No oil filling in progress" },
    {12, "Hydraulic adaptation valid - run HYDRAULIC teach-in first" },
    {13, "Clutch thermal/load value < 32 (cal 0xA004939C)" },
};

// Hydraulics (LID 0x22). Word = ~RAM 0xD0008752, mask cal 0xA0049176 = 0xFFFF.
static const TeachCond TEACH_HYDR[] = {
    { 2, "Engine speed 400..1000 rpm, while running target +/-150 rpm (cal 0xA00492E4/E6, 0xA00491E4)" },
    { 3, "Selector lever in P" },
    { 4, "All adaptations switched off / no adaptation request" },
    { 5, "Accelerator pedal released (<= 205 raw, cal 0xA004920A)" },
    { 6, "No high clutch load (<= 1, cal 0xA00492ED)" },
    { 7, "Gearbox oil temperature 60..110 C, +/-10 C hysteresis while running (cal 0xA00492E8/EA, 0xA0049244)" },
    { 8, "Function enabled / tester request present" },
    { 9, "Sensor OK (pressure/temperature/speed sensor group A)" },
    {10, "Sensor OK (group B)" },
    {11, "Sensor OK (input shaft speed group)" },
    {12, "Sensor OK (group D)" },
    {13, "Sensor OK (group E)" },
    {14, "Clutch thermal/load value <= 32 (cal 0xA0049246)" },
    {15, "Sensor OK (group F)" },
    {16, "Pressure-regulator (EDS) configuration consistent" },
};

#define TEACH_COUNT(a) (int)(sizeof(a)/sizeof((a)[0]))

// Example: print every condition from a 33-response r (r[0] = 0x73).
//   uint32_t c = ((uint32_t)r[3]<<24)|(r[4]<<16)|(r[5]<<8)|r[6];
//   for (int i=0;i<TEACH_COUNT(TEACH_RODS);i++){ bool ok=(c>>TEACH_RODS[i].bit)&1; ... }
