#!/usr/bin/env python3
"""Fill the 'hint' field of PdkToolWin/dtc_table.h with repair hints (rule-based, English).
Run after gen_dtc.py:  python3 -I dtc_hints.py ../PdkToolWin/dtc_table.h
Hints combine firmware RE findings (PDK_TCU_KNOWLEDGE.md) with standard PDK workshop practice;
they say what to check first and which PdkTool action helps. They are guidance, not a verdict."""
import re, sys
RULES = [  # (regex on pcode, regex on English text or None, hint) - first match wins
 (r'P1870', None, "Shift-rod teach-in missing/invalid: run TEACH-IN rods (0x20), then hydraulics (0x22), then clutches (0x21). Check EEPROM analysis: rods taught?"),
 (r'P1871', None, "Clutch teach-in missing/invalid: run TEACH-IN hydraulics (0x22) first, then clutches (0x21). [CLU] lines in EEPROM analysis show what is learned."),
 (r'P1872', None, "Hydraulic teach-in missing/invalid: run TEACH-IN hydraulics (0x22) at 60..110 C oil, engine 400..1000 rpm, then clutches (0x21)."),
 (r'P174C', None, "Learned data was reset at power-up or block 0x13 does not match the SW: freeze-frame ID 3063 shows WHICH adaptation sub-block was reset -> run that teach-in. CHECK BLOCK 0x13 compares the BIN."),
 (r'P174E', None, "EEPROM content implausible: read the EEPROM (READ FROM DONOR / Full backup) and run Analyze EEPROM file to see which block/CRC fails. Low battery during shutdown is a common cause."),
 (r'P174B', None, "TCU/gearbox pairing: TCU from another gearbox. Learned data is gearbox-specific - run all three teach-ins."),
 (r'P1749|P186F|P18C6', None, "Coding/trim dataset invalid: check the TCU part number/SW against the car; after a donor swap re-code with PIWIS and run all teach-ins."),
 (r'P18CB', None, "On this SW also raised when the vehicle variant code on CAN (freeze-frame ID 3064) is not 8: wrong/donor TCU or wrong coding. Otherwise AWD hang-on clutch overheating."),
 (r'P173[1-4]', None, "Distance-sensor PWM outside 8..92 %: check the sensor plug/wiring and 5 V/8.5 V supply (LIVE DATA). Rod reading stuck = failed sensor (known PDK weak point, sensor inside the gearbox). Re-teach rods after repair."),
 (r'P173[5-8]', None, "Short distance-sensor dropouts: intermittent contact/wiring or failing sensor. Watch the rod in LIVE DATA while shaking the harness."),
 (r'P176[4-9A-F]|P1779|P177[A-F]|P1780', None, "Gear/synchro engagement problem: compare rod positions in LIVE DATA with the learned end/synchro points; re-run rod teach-in. If it returns: distance sensor, shift fork/synchro wear or EDS5/EDS6 valve (Valve test)."),
 (r'P177[1-4]|P1767', None, "Gear valve / multiplexer: run Valve test MV1/MV2 and check currents; check the high-side supply path 3 (P1832/P183A). Valve body fault likely if electrical is OK."),
 (r'P17D[0-9A-F]', None, "Wrong/unrequested shift-rod movement: check distance sensors (LIVE DATA), MV1/MV2 and EDS5/EDS6 (Valve test), oil level/quality. Re-teach rods."),
 (r'P17B[1-2]', None, "Clutch pressure implausible: compare pressure sensor readings in LIVE DATA with the EDS1/EDS2 currents; check sensor plug, then valve body. Re-run hydraulics teach-in."),
 (r'P17B[3-4]', None, "Clutch valve sticks after rapid fill: valve body / EDS1-2 contamination. Oil change + hydraulics teach-in; if repeated, valve body."),
 (r'P17B[5-8]|P1999', None, "Clutch slip/glazing: compare engine rpm with input-shaft speeds (LIVE DATA). Check touch-point drift in EEPROM analysis ([CLU]); re-run hydraulics + clutch teach-in. Persistent slip = worn clutch pack."),
 (r'P17B[B-E]|P17E[2-3]', None, "Clutch does not open: EDS1/EDS2 valve or valve body; check with Valve test and pressures in LIVE DATA. Do not drive - risk of stalling/damage."),
 (r'P17E[0-1]|P17EA', None, "Hydraulic emergency hold (HIS/EDS7): run Valve test EDS7 and check current; valve body if electrical is OK."),
 (r'P17F[0-2]', None, "Gearbox over-temperature: check cooling (EDS4 cooling-oil valve, oil cooler, oil level). Driving style (creeping, launches) - see launch counters in EEPROM analysis."),
 (r'P17A[1-9A-F]', None, "Valve leakage/shunt current: wiring to the valve body or the internal harness/EDS coil. Compare EDS currents in LIVE DATA / Valve test."),
 (r'P09[6-7][0-9]|P27[1-3][0-9]|P07[5-6][0-9]|P098[0-9]|P0979', None, "Valve circuit fault: measure the coil and harness, check HSS supply in LIVE DATA, then Valve test. The internal harness/connector of the valve body is a common cause."),
 (r'P18[34][0-9A-F]', None, "Actuator high-side supply path: short/open on that supply path. The bracket lists the valves on the path - disconnect them one by one; check HSS voltage in LIVE DATA."),
 (r'P17(06|07|0A|0B)', None, "Sensor supply out of range: a shorted sensor (distance, pressure, speed) pulls it down. Watch 5 V/8.5 V in LIVE DATA while unplugging sensors."),
 (r'P173[B-E]|P174[3-4]', None, "Input-shaft speed sensor: wiring/sensor. Compare the K1/K2 shaft speeds with engine rpm in LIVE DATA (freeze-frame IDs 13/14/15)."),
 (r'P1745|P175[7-8]', None, "Input-shaft overspeed: check for a missed shift / clutch drag; read freeze frame (IDs 13/14/15, gear)."),
 (r'P08[4-8][0-9]|P181[A-C]', None, "Pressure sensor: check the sensor plug and 5 V supply; compare pressures at rest in LIVE DATA (both clutches open ~0 mbar)."),
 (r'P071[0-9]|P172[D-F]', None, "Gearbox oil temperature sensor: check wiring; compare with engine temperature after a cold start. It blocks teach-ins (temperature windows)."),
 (r'P0562|P0563|P1752', None, "Supply voltage: check battery, charging and TCU ground/KL30 (LIVE DATA shows KL30). Use a charger during teach-in and backups."),
 (r'P06[0-9][0-9]|P1715|P18B[0-9]|P189[C-D]|P18A0|P1897|P189A', None, "TCU internal / software monitor: clear and recheck. If it returns: Full backup, then check supply/grounds; persistent = TCU fault."),
 (r'P0614', None, "TCU SW does not match the engine: check part number/SW of TCU vs DME after a swap."),
 (r'P0730|P073[1-2]', None, "Ratio monitoring: clutch slip or wrong gear - check input-shaft vs output speeds, rods in LIVE DATA."),
 (r'P0706|P18A5|P09[23][0-9]|P081[7AB]', None, "Selector lever / shift lock / starter inhibit circuit: check the lever, its CAN/serial link and wiring."),
 (r'P18C[0-5]|P18CA|P18D0', None, "AWD hang-on clutch (PTM) system: check the transfer case/PTM and its pressure sensor 3; not the PDK clutches."),
 (r'P1760|P1775', None, "Consequential fault: fix the other stored faults first."),
 (r'P186[0-9]|P199[0-1]', None, "Engine interface (torque intervention / stop manager): check DME faults and CAN to the engine."),
 (r'P070[2-3]', None, "Generic TCU electrical/brake-switch fault: check TCU supply/grounds and the brake light switch signal; read other stored faults."),
 (r'P281[2-5]', None, "Pressure control solenoid G (SAE name, likely EDS7/HIS on this TCU - INFERRED): Valve test EDS7, check its current and wiring."),
 (r'U', None, "Communication fault: check the named control module, CAN wiring and its own fault memory; often from low battery or a module that was unplugged."),
]
def main(path):
    s = open(path, encoding='utf-8').read()
    out=[]; n=0
    for line in s.splitlines(True):
        m = re.match(r'(  \{0x[0-9A-F]+,"(\w+)","((?:[^"\\]|\\.)*)","((?:[^"\\]|\\.)*)",)"((?:[^"\\]|\\.)*)"(,\d\},?\n)', line)
        if m:
            pc, en = m.group(2), m.group(3)
            for rp, rt, h in RULES:
                if re.match(rp, pc) and (rt is None or re.search(rt, en, re.I)):
                    line = m.group(1) + '"' + h.replace('"', '\\"') + '"' + m.group(6); n += 1; break
        out.append(line)
    open(path, 'w', encoding='utf-8').write(''.join(out)); print('hints set:', n)
main(sys.argv[1])
