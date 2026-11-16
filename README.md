# ZenStates-C

[中文版](README.zh-CN.md)

Pure C Boost cap for AMD Ryzen 3000 / 5000 / 7000 series desktop CPUs (Zen 2 / Zen 3 / Zen 4), 0.2.1.

It caps the maximum Boost frequency by sending HSMP commands. The CPU keeps boosting dynamically below the cap, and still drops frequency and voltage at idle. This is a cap, not a fixed-frequency lock: the program never sets a fixed frequency, a fixed voltage, or a fixed VID. HSMP provides a single socket-wide cap, so the two INI values (single-core and all-core) collapse to one effective cap: min(SingleCoreMaxMHz, AllCoreMaxMHz).

## Supported CPUs

| Family | CPUs | Cap support |
|---|---|---|
| Zen 2 | Matisse (Ryzen 3000), Castle Peak (Threadripper 3000) | **Works via HSMP** (see Troubleshooting): RSMU 0x29/0x2B are rejected on SMU 46.73.0, but HSMP WriteBoostLimitAllCores (0x9) sets the cap — verified 2026-11-16, interface version 2, set 4000 MHz, HSMP read-back 4000 MHz, CPU caps at ~4000 MHz. RSMU 0x6E still reads the fused 4200 MHz (expected: 0x6E reads the fused max, HSMP is a separate socket cap). Cap persists until changed or reboot |
| Zen 3 | Vermeer (Ryzen 5000), Chagall (Threadripper 5000), Milan (EPYC 7003) | HSMP 0x9, attempted but UNVERIFIED on Zen 3 (same mailbox/messages as Zen 2); the interface-version gate decides at runtime |
| Zen 4 | Raphael / Dragon Range (Ryzen 7000), Genoa, Storm Peak | HSMP 0x9, attempted but UNVERIFIED on Zen 4 (same mailbox/messages as Zen 2); the interface-version gate decides at runtime |
| Zen 1, Zen+, Rome, APUs, Zen 5 | — | Not supported: the sourced tables contain no boost-limit command for them, so the program detects the CPU and refuses with a reason instead of sending a code that means something else (or nothing) on that family |

Family 17h Model 31h is ambiguous: Rome (EPYC, no cap support) and Castle Peak (Threadripper 3000, Matisse table) share the CPUID. The Matisse table is attempted; the HSMP interface-version gate decides — if the SMU does not implement HSMP, the program stops before setting anything (nothing was applied, exit code 3).

## How it works

- Frequency cap: HSMP message `SetBoostLimitSocket` (0x9, socket-wide, arg = MHz) on Zen 2/3/4 — the same mailbox and message for all three families. The cap is verified by reading it back with HSMP `GetBoostLimit` (0xA, arg = APIC ID). Power limits (only when set in the INI): PPT/TDC/EDC (0x53/0x54/0x55 on Zen 2/3, 0x56/0x57/0x58 on Zen 4) over RSMU. The stock maximum is read back with RSMU 0x6E (see below). The old RSMU cap codes (0x29/0x2B, 0x70) are NOT used any more: 0x29/0x2B are rejected by current SMU firmware (see Troubleshooting).
- Stock maximum is read, not assumed: at startup, before sending any cap, the program reads 0x6E `GetBoostLimitFrequency` — the same call upstream's `Cpu.GetFMax()` uses. 0x6E reads the FUSED max, which the HSMP socket cap does NOT change, so it stays the correct ceiling even when a cap from a previous run is still active. That value is the ceiling for the INI caps. If the read fails or returns an implausible value, the program aborts before sending any cap. No frequency is hardcoded any more.
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
| `[Boost]` | `SingleCoreMaxMHz` | 4000 | 3000–stock max (read from CPU) | Single-core Boost cap, MHz. Effective cap = min(single, all) — see note below |
| `[Boost]` | `AllCoreMaxMHz` | 3900 | 3000–stock max (read from CPU) | All-core Boost cap, MHz (clamped to the single-core value if higher). Effective cap = min(single, all) |
| `[Power]` | `PPT_W` | (unset) | 45–300 | PPT, W (sent to the SMU in mW). Only sent when set; unset = left as the BIOS set it |
| `[Power]` | `TDC_A` | (unset) | 30–200 | TDC, A (sent in mA). Only sent when set |
| `[Power]` | `EDC_A` | (unset) | 45–300 | EDC, A (sent in mA). Only sent when set |
| `[Startup]` | `AutoApply` | 1 | 0/1 | Install the boot task; the program itself always applies the config when run |
| `[Logging]` | `LogPath` | (empty) | — | Empty = `zenstates-c.log` next to the EXE. Do not point a SYSTEM task at a path under a user profile. |

Power limits are optional since 0.2.0: 0.1.0 always sent Ryzen 5 3600 stock (88 W / 60 A / 90 A), which is wrong for other CPUs, so unset keys are now left alone. If you want to pin them, set them to your own CPU's stock values (check HWMonitor or your BIOS). An INI written by 0.1.0 still has the keys, so its behaviour is unchanged.

Note on the two Boost keys (0.2.1): HSMP provides a single socket-wide cap, so the effective cap is min(SingleCoreMaxMHz, AllCoreMaxMHz). With the defaults above (4000 / 3900) the cap is 3900 MHz. To get a single 4000 MHz cap, set both to 4000 (or AllCoreMaxMHz=4000 with SingleCoreMaxMHz at 4000 or above). The old RSMU two-command split (0x29 single / 0x2B all-core) no longer exists.

Read the current config (0.2.2): `zenstates-c.exe --read` reads the CPU's current config and writes it into the INI without applying anything — Boost keys get the current HSMP socket cap, Power keys are written as empty entries (the applied PPT/TDC/EDC cannot be read back on Zen 2, so they stay unset). The GUI has a "Read current from CPU" button for the same thing; after it runs, the GUI refreshes its fields from the INI. Power fields show EMPTY when unset (an empty value means "left as the BIOS set it"); saving with a Power field empty deletes that key.

GUI rewrite (0.2.3): the GUI no longer uses one child window per tab page. An earlier version positioned a "ZSC_Page" child window over the tab's display area with TabCtrl_AdjustRect, and on the user's machine those page windows never appeared, leaving every tab blank (the tab strip, Apply/Save/Close and the status bar all showed; only the page content was missing). All controls are now direct children of the main window and are shown/hidden by tab, so there is no page-window layer to go blank.

GUI layout (0.2.4): the window is widened to 560x400 and labels are wide enough that their text is never clipped (0.2.3 clipped "Single-core boost cap (MHz):" and "Log path (empty = exe dir):" because the labels were narrower than the text and the edit boxes started too far left). A second build, `zenstates-c-gui-xl.exe`, doubles every dimension and the font (same source, `-DGUI_SCALE=2`), for users who want a larger layout.

GUI fixes (0.2.5): the XL build shipped with an invalid manifest — its XML comment contained `--` inside the comment body, which XML forbids, so Windows refused to start it ("side-by-side configuration is incorrect"). Fixed by rewording the comment; all five manifests are now XML-validated. The two frequency spinners (MHz) now step by 25 per click (UDN_DELTAPOS); the power spinners (W/A) still step by 1.

The GUI (`zenstates-c-gui.exe`) edits the same INI: tabs for CPU/Boost, Power, and Settings. Apply Now saves the INI and runs the main program elevated; Save only saves; Close closes. The GUI runs unelevated, so each privileged action shows one UAC prompt. Exit codes: 0 = success; 1 = driver/module/config error; 2 = not administrator or unsupported CPU; 3 = at least one SMU command failed. The log file is authoritative.

## Verify

1. Before the first run, open HWMonitor and note the current maximum frequency and maximum voltage with stock settings (on a Ryzen 5 3600: ~4200 MHz and ~1.45 V).
2. Run `zenstates-c.exe` as administrator. The log should show `SET TOOK EFFECT` for the HSMP socket cap (plus any power limits set in the INI).
3. Check HWMonitor again: maximum frequency should stop at the cap (on a 3600 with defaults: ~3900 MHz, the min of 4000/3900) and maximum voltage should drop (on a 3600: to ~1.35 V), while idle frequency and voltage still drop — that is the evidence that this is a cap, not a lock.

If the cap does not apply (HSMP commands fail in the log, or frequency still reaches the stock maximum): confirm the PawnIO driver is installed and matches the modules, confirm the program really ran as administrator, and check which command failed in the log. The old RSMU cap route (0x29/0x2B) is dead on current firmware and is no longer used: observed 2026-11-16, a Ryzen 5 3600 with SMU firmware 46.73.0 refused both (win32=1359), and a 0x6E read-back proved the limit never moved. The CPU was stuck at ~800 MHz / 0.6 V after the very first RSMU run only — a one-time transient, cleared by reboot and not reproduced on re-runs (cause unknown); a reboot is the recovery if it happens. If the HSMP interface-version query (0x3) fails, the CPU/firmware does not implement HSMP and the program stops without setting anything (exit 3). Upstream 2.0 defines no boost-limit SET command for Zen 2 in RSMU/MP1 at all (only the 0x6E read), which is consistent with the RSMU refusal.

The cap is set over HSMP (WriteBoostLimitAllCores = 0x9, "Set socket maximum frequency level", arg = MHz; Linux kernel amd_hsmp.h; ZenStates-Core HSMPMailbox.cs). Ryzen Master sets the all-core cap, and HSMP is the mailbox AMD's own interface documents for it. The RSMU module's built-in command cannot reach HSMP (it hardcodes RSMU), so the program re-implements the generic mailbox protocol (SMU.cs SendSmuCommand) over the module's raw register read/write (ioctl_read/write_smu_register) to send HSMP 0x9. It first queries the HSMP interface version (0x3); if HSMP is not implemented on the CPU, it stops without setting anything. Verified 2026-11-16 on a Ryzen 5 3600 (SMU 46.73.0): HSMP interface version 2, set 4000 MHz, HSMP read-back 4000 MHz, CPU caps at ~4000 MHz in HWMonitor. RSMU 0x6E still reads the fused 4200 MHz after an HSMP cap — expected, not a contradiction: 0x6E reads the fused max, HSMP is a separate socket cap, and the SMU enforces the lower. The cap persists until changed or reboot. `zenstates-hsmp.exe` is a standalone probe for the same HSMP path (target MHz as argument; restore with the stock max, e.g. 4200; 0.2.1 fixed its restore hint to use the fused max from 0x6E).

## V/F sweep (`zenstates-vf.exe`)

The SMU has no command to dump its V/F table, so this tool measures the curve empirically: for each frequency step it sets the Boost cap to that frequency, runs a scalar integer load on one core so the core actually hits the cap, and reads the core's VID from MSR `0xC0010293` (HW_PSTATE_STATUS: Fid, DfsId, Vid — frequency = 2 × Fid / DfsId × 100 MHz, voltage = 1.55 − Vid × 0.00625 V, SVI2).

Usage: run `zenstates-vf.exe` as administrator. By default it sweeps from the stock maximum (read from the CPU via RSMU 0x6E, the fused max) down to Fmax − 600 MHz in 100 MHz steps, settles 1.5 s per step, and takes 5 samples. After each cap set it reads the limit back (HSMP 0xA) and logs whether the new limit actually took effect. Results go to `zenstates-vf.csv` (one row per sample, with both the reader core and the load core) and the per-step median is printed in the log. The reader core is the load core's true SMT sibling (found via APIC ID). Settings (range, step, settle time, sample count, load core) are in `zenstates-vf.ini`.

Notes: the sweep restores the socket cap to the HSMP limit (0xA) reported before the sweep, so if a cap was already installed it is put back — the sweep is non-destructive. The sweep supports Zen 2, Zen 3 and Zen 4 (HSMP 0x9 constrains any loaded core); Zen 3 and Zen 4 via HSMP are unverified. The curve is measured under the current power limits, so the top points may sag if PPT/TDC/EDC bind. BCLK is assumed to be 100 MHz. The load is a scalar integer loop (no AVX).

Hold a load (0.2.2): `zenstates-vf.exe --hold [core]` pins a scalar spin-load to one core and prints its frequency and voltage every second until Ctrl+C. It sets no cap — it holds a load against whatever cap is currently installed, so you can watch HWMonitor and confirm the cap holds under sustained load. The sweep already spins the same load thread while it runs; `--hold` is the standalone version.

## Build from source

Windows (MinGW-w64 gcc + windres, or MSVC cl + rc): run `build.bat`. Linux cross-compile: `./build.sh` (MinGW-w64 gcc 13.2). Both produce `zenstates-c.exe`, `zenstates-c-gui.exe`, `zenstates-vf.exe`, `zenstates-hsmp.exe` with 0 warnings. Every push and pull request is built by CI on `windows-latest` (see `.github/workflows/build.yml`); pushing a `v*` tag builds a Release zip (see `release.yml`).

## Limits

- SMU commands come from reverse engineering (AMD publishes no documentation for them). Behaviour may differ across BIOS/AGESA versions.
- Only Zen 2 / Zen 3 / Zen 4 desktop CPUs are supported as in the table above. Zen 1, Zen+, Rome, APUs and Zen 5 are refused. HSMP on Zen 3 and Zen 4 is unverified. The RSMU cap route (0x29/0x2B) was observed rejected on SMU firmware 46.73.0 (Ryzen 5 3600, 2026-11-16) and is no longer used; the cap is HSMP 0x9.
- All settings are lost on reboot (by design — a reboot is the recovery path).
- First run on real hardware is a test: verify with HWMonitor as described above.

## License / attribution

- License: GPLv3 — see `LICENSE`. This project is a derivative rewrite, not a fork: no code was copied from [irusanov/ZenStates](https://github.com/irusanov/ZenStates); the C code was written from scratch, implementing the same SMU protocol derived from the upstream GPL sources and decompilation. See `ATTRIBUTION.md` for the full list of what was used and what was not.
- The two PawnIO modules in `modules/` are LGPL-2.1, unmodified and signed, from PawnIO.Modules 0.2.11. The PawnIO driver itself is not distributed here; install it from <https://pawnio.eu/>.
