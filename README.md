<p align="center">
  <img src="assets/banner.png" alt="PdkTool – Porsche Panamera 970 PDK3000 diagnostics" width="100%">
</p>

<p align="center">
  <a href="https://github.com/brstic/pdk-3000-tool/releases/latest"><img src="https://img.shields.io/github/v/release/brstic/pdk-3000-tool?style=for-the-badge&color=00d4e6&label=download" alt="Latest release"></a>
  <a href="https://github.com/brstic/pdk-3000-tool/releases"><img src="https://img.shields.io/github/downloads/brstic/pdk-3000-tool/total?style=for-the-badge&color=00d4e6" alt="Downloads"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/brstic/pdk-3000-tool?style=for-the-badge&color=555" alt="License"></a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-Windows-0078D6?style=flat-square&logo=windows&logoColor=white" alt="Windows">
  <img src="https://img.shields.io/badge/interface-Tactrix%20OpenPort%202.0%20(J2534)-333?style=flat-square" alt="Tactrix J2534">
  <img src="https://img.shields.io/badge/protocol-KWP2000%20on%20CAN-333?style=flat-square" alt="KWP2000 on CAN">
  <img src="https://img.shields.io/badge/TCU-5WP54023%20·%20TC1766-333?style=flat-square" alt="TCU">
  <img src="https://img.shields.io/badge/language-C%2B%2B17-00599C?style=flat-square&logo=cplusplus&logoColor=white" alt="C++17">
  <img src="https://img.shields.io/badge/status-not%20yet%20tested%20on%20a%20car-orange?style=flat-square" alt="Status">
</p>

<p align="center">
  <b>Free diagnostics, teach-in and EEPROM analysis for the Porsche Panamera 970 PDK gearbox controller.</b><br>
  One small Windows exe · a Tactrix cable · no PIWIS needed for diagnosis and teach-in.
</p>

<p align="center">
  <a href="https://github.com/brstic/pdk-3000-tool/releases/latest"><b>⬇ Download PdkTool</b></a> ·
  <a href="docs/PdkTool_Manual.pdf"><b>📖 Manual (PDF)</b></a> ·
  <a href="docs/PDK_TCU_KNOWLEDGE.md"><b>🔬 Reverse-engineering notes</b></a>
</p>

---

## Contents

- [What it is](#what-it-is)
- [Features](#features)
- [Supported hardware](#supported-hardware)
- [Quick start](#quick-start)
- [How to use it](#how-to-use-it)
- [Files it creates](#files-it-creates)
- [The Mitten Bin (not included)](#the-mitten-bin-not-included)
- [Reverse-engineering notes](#reverse-engineering-notes)
- [Building from source](#building-from-source)
- [Repository layout](#repository-layout)
- [FAQ](#faq)
- [Safety and disclaimer](#safety-and-disclaimer)

## What it is

**PdkTool** is a standalone Windows program for the **PDK 3000** transmission control unit used in the
**Porsche Panamera 970 (2010–2016)**. The unit is a Continental/Siemens 5WP54023 with an Infineon TC1766 processor,
controlling a ZF 7DT70 dual-clutch gearbox. PdkTool talks to the TCU over the OBD port with KWP2000 on CAN, through a
Tactrix OpenPort 2.0.

All of its knowledge was reverse-engineered **from the TCU firmware itself** (software G070 and G088): request
formats, sessions, units, thresholds, fault codes and freeze-frame layouts. It is meant for repairing your own car or
TCUs in a workshop. Common jobs are reading what a fault really means, checking the shift-rod distance sensors,
running the basic settings (teach-in) after a repair, and checking a used or donor TCU before you install it.

> [!IMPORTANT]
> **Status: not yet tested on a car.** Everything is derived from static firmware analysis and cross-checked
> against real TCU dumps, but no on-car run has confirmed it yet. Please report results: open an issue and attach the
> log or report file.

## Features

| | Feature | What you get |
|---|---|---|
| 🩺 | **Diagnosis** | All stored DTCs, each with an English description, its status (present / stored / intermittent) and a **"Check:" repair hint**. The catalogue has 218 codes, including all 199 that this firmware can set. |
| 🧊 | **Freeze frames** | The snapshot the TCU saved when each fault happened, fully decoded: mileage, oil temperature, pressures, valve currents, engine and input-shaft speeds, vehicle speed, shift-rod positions, clutch adaptation, and which learned data was lost (ID 3063). |
| 📈 | **Live data** | Distance sensors on all 4 shift rods (PWM duty in % and position in mm), 3 pressure sensors, sensor and actuator supplies, valve currents EDS1–7, engine rpm, vehicle speed. Recorded to CSV, with **MARK** buttons for P/R/N/D and each gear. |
| 🎓 | **Guided teach-in** | Shift rods (0x20) → hydraulics (0x22) → clutches (0x21), in the order the firmware requires. If the TCU refuses, PdkTool lists **every start condition** as OK or FAILED, with the firmware threshold (oil temperature window, engine speed, selector in P…). |
| 💾 | **EEPROM analysis** | Decodes the TCU's EEPROM: VIN, software, whether the shift rods are taught (with the learned end, neutral and synchro points), clutch touch points and their drift, fast-fill times, hydraulic adaptation, launch counters, **operating hours**, odometer, event logs. All CRCs are checked. |
| 🔁 | **Donor / used TCU check** | Reads the learned data, block 0x13 and the full EEPROM over OBD, then tells you what the unit needs before it goes into another car. |
| 🧰 | **Full backup over OBD** | The complete 1.5 MB flash and the EEPROM, read-only, without opening the TCU. The result is the same as a bench boot-mode dump. |
| 🔧 | **Valve test** | Actuates EDS1–7 and MV1–3 one at a time (car standing, selector in P). |
| 🧪 | **Self-test** | Checks the program on your PC without a car: seed/key, protocol logic and decoders. |

## Supported hardware

| Item | Supported |
|---|---|
| Vehicle | Porsche Panamera 970 / 970 II with PDK (TCU 970.618.270.xx, Continental 5WP54023) |
| TCU software | G070 and G088 (others will probably work; live rod data picks the RAM layout automatically) |
| Interface | Tactrix OpenPort 2.0 with its J2534 driver installed |
| PC | Windows (7 or newer). The exe is 32-bit on purpose, because the Tactrix `op20pt32.dll` is a 32-bit DLL. |
| CAN IDs | Request 0x70F, response 0x779 (from the Porsche diagnostic database) |

## Quick start

1. **Install the Tactrix OpenPort 2.0 drivers** (they register the J2534 DLL; PdkTool finds it through the registry).
2. **Download** `PdkTool_v1.0.zip` from the [latest release](https://github.com/brstic/pdk-3000-tool/releases/latest)
   and unzip it anywhere. There is nothing to install.
3. **Run `PdkTool.exe`** and press **Self-Test (no car)**. It must end with `ALL PASSED`.
4. **Connect** the cable to the OBD port and switch the ignition ON. Connect a battery charger for anything longer
   than a quick read. The status line turns **green** when the Tactrix is detected.
5. Press **1. DIAGNOSE**.

## How to use it

The main window has five big buttons. Each one runs a whole job from start to end: session, unlock, read, decode
and report. You never type KWP commands.

<details>
<summary><b>1. DIAGNOSE</b> (read-only): what is wrong, and what to check first</summary>

<br>

- Reads identification, every stored DTC and the freeze frame of each fault.
- Each fault shows its P-code, English text, status and a **Check:** line that says what to inspect first and which
  PdkTool action helps. For example:
  - distance-sensor faults → *watch the rod in LIVE DATA*;
  - P1870/1871/1872 → *run that teach-in*;
  - P174C → *read ID 3063 to see which learned data was lost*.
- Everything is saved to `PDK_diag_<time>.txt` next to the exe.
- To clear the fault memory, use **Clear DTCs** (Advanced).
</details>

<details>
<summary><b>2. READ FROM DONOR</b> (read-only): everything the TCU has learned</summary>

<br>

- Reads the learned data from RAM, then enters the TCU's flasher and reads block 0x13 and the complete EEPROM.
- Prints the VIN, software, market and part number. It also prints whether the shift rods are taught (with all
  learned positions in mm), the clutch touch points and their drift since the teach-in, the hydraulic adaptation, and
  **operating hours / odometer / average speed**.
- It ends with a verdict on which teach-in is still needed.
- **Afterwards switch the ignition OFF for 10 s**, because the TCU is still in the flasher.
- The same analysis works offline: **Analyze EEPROM file** decodes a `.bin` from a bench programmer.
</details>

<details>
<summary><b>3. CHECK BLOCK 0x13</b> (read-only): is the teach-in parameter block correct?</summary>

<br>

- Reads calibration block 0x13 and recognises the official Porsche *KD Mitten Bin* by CRC32. It also compares the
  block with a file you choose.
- Nothing is written. On the TCUs examined so far, the block was always intact; the learned data lives in the EEPROM.
</details>

<details>
<summary><b>4. TEACH-IN (guided)</b>: the basic settings after a repair</summary>

<br>

- Runs, in this order: **shift rods → hydraulics → clutches**. The firmware refuses the clutch teach-in until the
  hydraulic adaptation is valid.
- Typical conditions: engine running, selector in **P**, accelerator released, oil temperature in the
  window (rods 35–100 °C, hydraulics and clutches 60–100/110 °C), no other active faults.
- If a step is refused or fails, every start condition is printed as `[OK]` or `[!!]`, with the firmware limit.
</details>

<details>
<summary><b>5. LIVE DATA</b>: shift-rod sensors, pressures, valves</summary>

<br>

- Shows the 4 shift-rod distance sensors live: PWM duty (valid window **8–92 %**) and position in mm. Pressures,
  supplies, valve currents, engine rpm and vehicle speed are shown alongside.
- Press **MARK** at each selector position or gear. **STOP** finishes the recording and prints min/max per channel.
- Everything goes to `PDK_rods_<time>.csv`, which you can open in Excel.
</details>

<details>
<summary><b>Advanced</b>: manual buttons</summary>

<br>

| Button | Purpose |
|---|---|
| Identify / Seed/Key / Unlock | Single protocol steps, for experts |
| EEPROM / Blk13 | Raw reads of the EEPROM and block 0x13 |
| Teach rods / clutch / hydr. | One teach-in routine on its own |
| Clear DTCs | Clears the fault memory (after confirmation) |
| Analyze EEPROM file | Offline decode of an EEPROM dump (32 KB or 128 KB) |
| Valve test | Actuates one valve: EDS1–7 or MV1–3 |
| Full backup | 1.5 MB flash + EEPROM over OBD (several minutes, read-only) |
</details>

## Files it creates

All files are written next to `PdkTool.exe`:

| File | Created by |
|---|---|
| `PDK_diag_<time>.txt` | DIAGNOSE: full report with freeze frames |
| `PDK_rods_<time>.csv` | LIVE DATA recording |
| `PDK_donor_<time>_ram.bin`, `_block13.bin`, `_EEprom.bin` | READ FROM DONOR |
| `PDK_<time>_block13.bin` | CHECK BLOCK 0x13 |
| `PDK_full_<time>_IntFl.bin`, `_EEprom.bin` | Full backup |

## The Mitten Bin (not included)

The *KD Mitten Bin* is Porsche's default teach-in parameter block (logical block 0x13). It is Porsche property and is
**not** in this repository or the release. The German PIWIS fault texts are left out for the same reason.

To use the comparison in button 3, put your own copy next to `PdkTool.exe`. It can come from your PIWIS installation
or from a TCU backup, and may be 15872 or 16384 bytes. PdkTool recognises it by CRC32, whatever the file is called:

| Mitten Bin | Model years | CRC32 (first 15872 bytes) |
|---|---|---|
| 970.618.970.00 | MY 2010–2013 and 970 II GTS | `0x811C0A13` |
| 970.618.971.00 | MY 2014+ | `0xF6C74A85` |

## Reverse-engineering notes

The [`docs/`](docs) folder holds what was learned about this TCU. Each finding is marked **VERIFIED**, **INFERRED** or
**OPEN**.

| Document | Topic |
|---|---|
| [PDK_TCU_KNOWLEDGE.md](docs/PDK_TCU_KNOWLEDGE.md) | Summary of everything below |
| [teachin_thresholds.md](docs/re_findings/teachin_thresholds.md) | Teach-in routines, steps, limits, start conditions |
| [eeprom_map.md](docs/re_findings/eeprom_map.md) | EEPROM emulation format, CRCs, block map, adaptation sub-blocks |
| [clutch_adaptation.md](docs/re_findings/clutch_adaptation.md) | Clutch touch points, fast-fill, hydraulic adaptation |
| [usage_stats.md](docs/re_findings/usage_stats.md) | Odometer, operating hours, event and reset logs |
| [dtc_and_live.md](docs/re_findings/dtc_and_live.md) | Fault table, DTC → freeze-frame map, live RAM map |
| [env_signals.md](docs/re_findings/env_signals.md) | Freeze-frame measurement IDs, units and scaling |
| [kwp_services.md](docs/re_findings/kwp_services.md) | KWP2000 services per session, memory reads, the flasher |
| [units_ids_routines.md](docs/re_findings/units_ids_routines.md) | Units, routine IDs, hidden routines |
| [sw_diff_G070_G088.md](docs/re_findings/sw_diff_G070_G088.md) | What changed between software G070 and G088 |

Highlights:
- The distance sensor's PWM duty is valid only between 8.00 % and 92.00 %. Position = curve(duty) − 13.75 mm.
- Synchro distance limit: 0.75–5 mm.
- The TCU does not check the VIN, so a donor swap needs only the teach-ins.

Python tool: [`tools/eeprom_decode.py`](tools/eeprom_decode.py) decodes and checks an EEPROM dump.

## Building from source

```sh
# MinGW-w64 on Linux/macOS, or MSYS2 on Windows
./build.sh            # → PdkTool.exe (32-bit, static, no DLLs needed)
```

All of PdkTool is one C++17 file, [`src/pdktool.cpp`](src/pdktool.cpp), using plain Win32 with no frameworks. The
decoders are header-only (`src/*.h`).

## Repository layout

```
├── src/            PdkTool source (pdktool.cpp + decoder headers)
├── docs/           Manual (PDF), knowledge base, detailed RE notes
├── tools/          eeprom_decode.py, dtc_hints.py
├── assets/         README banner
└── build.sh        one-line MinGW build
```

## FAQ

**Do I need PIWIS?** Not for diagnosis, live data, teach-in or the EEPROM analysis. PIWIS is only needed for coding,
or if you want the original Mitten Bin file.

**Will it work with other cables?** It is written for the Tactrix OpenPort 2.0. Other J2534 cables may work if their
driver registers under `PassThruSupport.04.04`, but none has been tested.

**Can it write to the TCU?** It writes only through official diagnostic routines: teach-in, valve test and clear DTCs.
It does **not** flash firmware, write raw EEPROM or write block 0x13.

**My TCU says "shift rods NOT taught". What now?** Run **4. TEACH-IN** with the engine running, the selector in P and
warm oil. The rods come first, then hydraulics, then clutches.

## Safety and disclaimer

- Car standing, parking brake on, selector in **P**, battery charger connected.
- After any flasher read (READ FROM DONOR, CHECK BLOCK 0x13, Full backup), switch the **ignition OFF for 10 s**.
- Valve tests and teach-ins move hydraulics inside the gearbox. Keep clear and follow the prompts.

This project has **no connection** to Porsche AG, ZF Friedrichshafen AG or Continental AG. All names and part
numbers are used only to identify parts. Working on transmission controllers can leave a car unable to drive. The
software is provided **as is, with no warranty** ([MIT License](LICENSE)).

<p align="center"><sub>Made for mechanics who fix PDKs instead of replacing them.</sub></p>
