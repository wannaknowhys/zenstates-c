# ZenStates-C

[中文版](README.zh-CN.md)

Pure C Boost cap for AMD Ryzen 3000 / 5000 / 7000 series desktop CPUs (Zen 2 / Zen 3 / Zen 4).

It caps the maximum Boost frequency — single-core and all-core separately — by sending SMU commands. The CPU keeps boosting dynamically below the cap, and still drops frequency and voltage at idle. This is a cap, not a fixed-frequency lock: the program never sets a fixed frequency, a fixed voltage, or a fixed VID.

## Supported CPUs

| Family | CPUs | Cap support |
|---|---|---|
| Zen 2 | Matisse (Ryzen 3000), Castle Peak (Threadripper 3000) | Single-core + all-core |
| Zen 3 | Vermeer (Ryzen 5000), Chagall (Threadripper 5000), Milan (EPYC 7003) | Single-core + all-core, attempted — the 0x29/0x2B codes are inherited from the Zen 2 table and are unverified on Zen 3; failures are reported, nothing is applied by a failed command |
| Zen 4 | Raphael / Dragon Range (Ryzen 7000), Genoa, Storm Peak | All-core only (0x70) — there is no single-core cap command for Zen 4 in the sourced table, so `SingleCoreMaxMHz` is ignored |
| Zen 1, Zen+, Rome, APUs, Zen 5 | — | Not supported: the sourced tables contain no boost-limit command for them, so the program detects the CPU and refuses with a reason instead of sending a code that means something else (or nothing) on that family |

Family 17h Model 31h is ambiguous: Rome (EPYC, no cap support) and Castle Peak (Threadripper 3000, Matisse table) share the CPUID. The Matisse table is attempted; if the SMU rejects the commands, that means Rome (nothing was applied, exit code 3).

## How it works

- Frequency cap: SMU messages `SetBoostLimitFrequency` (0x29, single-core) and `SetBoostLimitFrequencyAllCores` (0x2B) on Zen 2/3; `SetBoostLimitFrequencyAllCores` (0x70) on Zen 4. Power limits (only when set in the INI): PPT/TDC/EDC (0x53/0x54/0x55 on Zen 2/3, 0x56/0x57/0x58 on Zen 4). The current limit is read back with 0x6E.
- Stock maximum is read, not assumed: at startup, before sending any cap, the program reads 0x6E `GetBoostLimitFrequency` — the same call upstream's `Cpu.GetFMax()` uses. That value is the ceiling for the INI caps. If the read fails or returns an implausible value, the program aborts before sending any cap. No frequency is hardcoded any more.
- Voltage follows automatically: in Boost mode the SMU picks the VID from its own V/F table for the requested frequency. Capping the frequency means the SMU never requests the VID of the top frequency step, so peak voltage drops as well. There is no separate "cap VID" SMU command in Boost mode; the only VID command belongs to manual-overclock mode (a fixed VID), which would defeat the purpose of a cap.
- Nothing is written to BIOS and nothing persists across reboot. Restarting restores stock behaviour; to re-apply at every boot, install the scheduled task (see below).

## Requirements

- Windows 10 / 11, x64.
- A supported Ryzen desktop CPU (see the table above). Other CPUs are refused by design.
- The PawnIO driver (<https://pawnio.eu/>), installed separately. It is NOT included in this project.
- Administrator rights to run `zenstates-c.exe` / `zenstates-vf.exe`.

## Files

| File | Description |
|---|---|
| `zenstates-c.exe` | Main program: reads the INI and applies the cap + power limits |
| `zenstates-c-gui.exe` | Config GUI (pure Win32): edits the INI only, never touches the driver |
| `zenstates-vf.exe` | V/F sweep tool: measures the real voltage/frequency curve (see below) |
| `zenstates-c.ini` | Config for the main program |
| `zenstates-vf.ini` | Config for the sweep tool |
| `modules/RyzenSMU.bin`, `modules/AMDFamily17.bin` | Signed PawnIO modules (LGPL-2.1), required at runtime |
| `install-autostart.bat` / `uninstall-autostart.bat` | Install / remove the boot-time scheduled task (run as administrator) |
| `build.bat` / `build.sh` | Build scripts (Windows MinGW / Linux cross-compile) |
| `SLOW_MODE.md` | Notes on ZenStates' slow mode, and why it is unrelated to the cap |
| `LICENSE`, `ATTRIBUTION.md` | GPLv3 license text; attribution and sources |

## Install

1. Download the zip from the Releases page and extract it. Keep all files in the same folder — the EXEs, the INI files, and the `modules/` subfolder must stay together. Any folder works; there is no required install path.
2. Install the PawnIO driver from <https://pawnio.eu/> if it is not installed yet.
3. Test once by hand: right-click `zenstates-c.exe` → Run as administrator. Check `zenstates-c.log`: the last line should read `OK: boost cap applied`.
4. Verify with HWMonitor (see below). If the cap works, optionally run `install-autostart.bat` as administrator to re-apply it at every boot (scheduled task `ZenStates-C`, runs at startup as SYSTEM, 30 s delay, exits after applying). `uninstall-autostart.bat` removes the task; the cap itself does not persist across reboot.

## Configuration (`zenstates-c.ini`)

The program looks for `zenstates-c.ini` next to the EXE first, then in `%ProgramData%\ZenStates-C\`. A missing file is not an error: a warning is logged and defaults are used. Out-of-range values are clamped to the allowed range and logged.

| Section | Key | Default | Range | Meaning |
|---|---|---|---|---|
| `[Boost]` | `SingleCoreMaxMHz` | 4000 | 3000–stock max (read from CPU) | Single-core Boost cap, MHz. Ignored on Zen 4 (no such command) |
| `[Boost]` | `AllCoreMaxMHz` | 3900 | 3000–stock max (read from CPU) | All-core Boost cap, MHz (clamped to the single-core value if higher) |
| `[Power]` | `PPT_W` | (unset) | 45–300 | PPT, W (sent to the SMU in mW). Only sent when set; unset = left as the BIOS set it |
| `[Power]` | `TDC_A` | (unset) | 30–200 | TDC, A (sent in mA). Only sent when set |
| `[Power]` | `EDC_A` | (unset) | 45–300 | EDC, A (sent in mA). Only sent when set |
| `[Startup]` | `AutoApply` | 1 | 0/1 | Install the boot task; the program itself always applies the config when run |
| `[Logging]` | `LogPath` | (empty) | — | Empty = `zenstates-c.log` next to the EXE. Do not point a SYSTEM task at a path under a user profile. |

Power limits are optional since 0.2.0: 0.1.0 always sent Ryzen 5 3600 stock (88 W / 60 A / 90 A), which is wrong for other CPUs, so unset keys are now left alone. If you want to pin them, set them to your own CPU's stock values (check HWMonitor or your BIOS). An INI written by 0.1.0 still has the keys, so its behaviour is unchanged.

The GUI (`zenstates-c-gui.exe`) edits the same INI: tabs for CPU/Boost, Power, and Settings. Apply Now saves the INI and runs the main program elevated; Save only saves; Close closes. The GUI runs unelevated, so each privileged action shows one UAC prompt. Exit codes: 0 = success; 1 = driver/module/config error; 2 = not administrator or unsupported CPU; 3 = at least one SMU command failed. The log file is authoritative.

## Verify

1. Before the first run, open HWMonitor and note the current maximum frequency and maximum voltage with stock settings (on a Ryzen 5 3600: ~4200 MHz and ~1.45 V).
2. Run `zenstates-c.exe` as administrator. The log should show `-> OK` for the SMU commands it sent (two cap commands, plus any power limits set in the INI).
3. Check HWMonitor again: maximum frequency should stop at the cap (on a 3600 with defaults: ~4000 MHz) and maximum voltage should drop (on a 3600: to ~1.35 V), while idle frequency and voltage still drop — that is the evidence that this is a cap, not a lock.

If the cap does not apply (SMU commands fail in the log, or frequency still reaches the stock maximum): confirm the PawnIO driver is installed and matches the modules, confirm the program really ran as administrator, and check which command failed in the log. The 0x29/0x2B commands are restored from the old 0.x ZenStates service build; current AGESA firmware may refuse them — that is the main known uncertainty, and it can only be tested on real hardware.

## V/F sweep (`zenstates-vf.exe`)

The SMU has no command to dump its V/F table, so this tool measures the curve empirically: for each frequency step it sets the Boost cap to that frequency, runs a scalar integer load on one core so the core actually hits the cap, and reads the core's VID from MSR `0xC0010293` (HW_PSTATE_STATUS: Fid, DfsId, Vid — frequency = 2 × Fid / DfsId × 100 MHz, voltage = 1.55 − Vid × 0.00625 V, SVI2).

Usage: run `zenstates-vf.exe` as administrator. By default it sweeps from the stock maximum (read from the CPU) down to 3600 MHz in 100 MHz steps, settles 1.5 s per step, and takes 5 samples. Results go to `zenstates-vf.csv` (one row per sample) and the per-step median is printed in the log. Settings (range, step, settle time, sample count, load core) are in `zenstates-vf.ini`.

Notes: the sweep restores both caps to the value 0x6E reported before the sweep, so if a cap was already installed it is put back — the sweep is non-destructive. The sweep only supports Zen 2 and Zen 3: Zen 4 has no single-core cap command, and the sweep needs one to constrain its single-core load, so Zen 4 is refused (the main program still supports it, all-core only). The curve is measured under the current power limits, so the top points may sag if PPT/TDC/EDC bind. BCLK is assumed to be 100 MHz. The load is a scalar integer loop (no AVX).

## Build from source

Windows (MinGW-w64 gcc + windres, or MSVC cl + rc): run `build.bat`. Linux cross-compile: `./build.sh` (MinGW-w64 gcc 13.2). Both produce `zenstates-c.exe`, `zenstates-c-gui.exe`, `zenstates-vf.exe` with 0 warnings. Every push and pull request is built by CI on `windows-latest` (see `.github/workflows/build.yml`); pushing a `v*` tag builds a Release zip (see `release.yml`).

## Limits

- SMU commands come from reverse engineering (AMD publishes no documentation for them). Behaviour may differ across BIOS/AGESA versions.
- Only Zen 2 / Zen 3 / Zen 4 desktop CPUs are supported as in the table above. Zen 1, Zen+, Rome, APUs and Zen 5 are refused. Zen 3's cap codes are unverified; Zen 4 has no single-core cap.
- All settings are lost on reboot (by design — a reboot is the recovery path).
- First run on real hardware is a test: verify with HWMonitor as described above.

## License / attribution

- License: GPLv3 — see `LICENSE`. This project is a derivative rewrite, not a fork: no code was copied from [irusanov/ZenStates](https://github.com/irusanov/ZenStates); the C code was written from scratch, implementing the same SMU protocol derived from the upstream GPL sources and decompilation. See `ATTRIBUTION.md` for the full list of what was used and what was not.
- The two PawnIO modules in `modules/` are LGPL-2.1, unmodified and signed, from PawnIO.Modules 0.2.11. The PawnIO driver itself is not distributed here; install it from <https://pawnio.eu/>.
