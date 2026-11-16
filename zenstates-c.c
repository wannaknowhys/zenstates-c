/* SPDX-License-Identifier: GPL-3.0-or-later
 * zenstates-c.c -- pure C re-implementation of the ZenStates boost-cap path
 * for Ryzen desktop CPUs (Zen 2 / Zen 3 / Zen 4), 0.2.2.
 *
 * Modes:
 *   (no args)  Apply the INI config (the boost cap + optional power limits).
 *   --read     Read-only: read the current config from the CPU (HSMP socket
 *              cap + fused stock max) and write it into the INI, then exit
 *              without applying anything. Boost keys get the current socket
 *              cap; Power keys are written as EMPTY entries, because the
 *              currently-applied PPT/TDC/EDC cannot be read back on Zen 2
 *              (no Get command in the sourced table) -- an empty entry
 *              means "unset" (left as the BIOS set them), and both the main
 *              program and the GUI treat an empty value as absent.
 *
 * What it does (cap, NOT a fixed-frequency lock):
 *   - single-core boost cap (default 4000 MHz) and all-core boost cap
 *     (default 3900 MHz), each clamped to the CPU's own stock maximum,
 *     read back from the SMU at startup (see Fmax below), never assumed.
 *   - PPT/TDC/EDC are OPTIONAL: sent only when set in the INI. With no
 *     [Power] keys the power limits are left exactly as the BIOS set
 *     them. (0.1.0 always sent Ryzen 5 3600 stock 88 W / 60 A / 90 A;
 *     that default is wrong for other CPUs, so 0.2.0 does not send it.)
 *   - Boost stays dynamic below the cap; idle still downclocks and drops voltage.
 *   - It NEVER writes a fixed frequency, a fixed voltage, or a VID.
 *
 * How it works (see SLOW_MODE.md for the evidence chain):
 *   0.2.1 changed the cap transport: the RSMU boost-limit SET commands
 *   (0x29/0x2B, restored from old ZenStates 0.x) are REJECTED by SMU
 *   firmware 46.73.0 on a Ryzen 5 3600 (win32=1359, and a 0x6E read-back
 *   proves the limit never moved). But Ryzen Master CAN set an all-core
 *   cap on Zen 2, so the working path is a THIRD mailbox this project
 *   had never tried: HSMP (Host System Management Port).
 *   PawnIO driver -> load RyzenSMU.bin / AMDFamily17.bin modules ->
 *   the generic mailbox protocol (SMU.cs SendSmuCommand) re-implemented
 *   in C over the module's raw register read/write
 *   (ioctl_read_smu_register / ioctl_write_smu_register), against the
 *   HSMP mailbox (Zen 2/3/4: MSG 0x3B10534, RSP 0x3B10980, ARG 0x3B109E0):
 *     0x3  GetInterfaceVersion      (HSMP exists? returns version, 2 on Matisse)
 *     0x9  SetBoostLimitSocket      (socket-wide cap, arg = MHz) -- THE cap
 *     0xA  GetBoostLimit            (arg = APIC ID, returns limit in MHz)
 *     0x8  SetBoostLimit            (per-core cap, arg = apic<<16 | MHz; NOT used)
 *   (Linux kernel amd_hsmp.h; ZenStates-Core HSMPMailbox.cs. Verified
 *   2026-11-16 on a Ryzen 5 3600: interface version 2, set 4000 MHz,
 *   HSMP read-back 4000 MHz, CPU caps at ~4000 MHz in HWMonitor.)
 *   HSMP provides a SINGLE socket-wide cap, so the two INI values collapse
 *   to one effective cap: min(SingleCoreMaxMHz, AllCoreMaxMHz). The
 *   single/all-core distinction of the RSMU era does not exist in HSMP.
 *   Power limits still go over RSMU DPTC (unchanged, OPTIONAL):
 *     0x53  SetFastLimit / PPT             (arg = mW)
 *     0x54  SetTDCVDDLimit                 (arg = mA)
 *     0x55  SetEDCVDDLimit                 (arg = mA)
 *   and 0x6E GetBoostLimitFrequency is still the stock-max (Fmax) read:
 *   it reads the FUSED max, which the HSMP socket cap does NOT change,
 *   so it stays the correct ceiling even when a cap is already active.
 *   Old code also set MSR_HWCR (0xC0010015) bit 21 on every thread before
 *   sending the boost-limit commands; this program does the same via the
 *   AMDFamily17 module, but treats a failure there as a warning, not fatal
 *   (ZenStates 2.0's core does not do it at all).
 *
 * Family dispatch (sources: ZenStates-Core 2.0, Hardware/SMU.cs tables
 * and Cpu.cs codename map; see SLOW_MODE.md for the evidence chain):
 *   The boost cap is HSMP for every supported family (same mailbox and
 *   same 0x9 message on Zen 2/3/4). The RSMU cap codes are gone:
 *   0x29/0x2B are rejected on current firmware and Zen 4's RSMU 0x70 is
 *   no longer needed. Power limits still use the per-family RSMU DPTC
 *   codes (PPT/TDC/EDC), and 0x6E is still the stock-max read.
 *   Matisse / Castle Peak (17h/0x71, 17h/0x31) -- Zen2Settings
 *     HSMP cap 0x9; PPT 0x53, TDC 0x54, EDC 0x55. Verified on a 3600.
 *   Vermeer / Chagall / Milan (19h/0x21, 0x08, 0x01) -- Zen3Settings
 *     HSMP cap 0x9; PPT 0x53, TDC 0x54, EDC 0x55. HSMP on Zen 3 is
 *     UNVERIFIED (same mailbox/messages as Zen 2, but never run there);
 *     the interface-version gate decides at runtime.
 *   Raphael / Dragon Range / Genoa / Storm Peak (19h/0x61, 0x11, 0x18)
 *     -- Zen4Settings: HSMP cap 0x9; PPT 0x56, TDC 0x57, EDC 0x58.
 *     HSMP on Zen 4 is UNVERIFIED too.
 *   Zen 1 / Zen+ / Rome / APUs / Zen 5: the sourced tables configure no
 *   HSMP mailbox for them, so the program detects the CPU and refuses
 *   with a reason instead of sending a code that means something else
 *   (or nothing) on that family. (Zen 5 uses a different HSMP MSG
 *   address, 0x3B10934, and is refused for that reason.)
 *   Family 17h Model 31h is ambiguous: Rome (EPYC) and Castle Peak
 *   (Threadripper 3000) share the CPUID. The Matisse table is attempted;
 *   the HSMP interface-version gate decides: if the SMU does not
 *   implement HSMP, the program stops before setting anything (exit 3).
 *   Mailbox: the HSMP addresses (MSG 0x3B10534 / RSP 0x3B10980 /
 *   ARG 0x3B109E0) are driven by the generic protocol in this file over
 *   the module's raw register read/write -- the C code DOES touch these
 *   addresses (unlike the old RSMU path, where the module hid them).
 *
 * Fmax (stock maximum, no hardcoded 4200 any more):
 *   At startup, before sending any cap, the program reads 0x6E
 *   GetBoostLimitFrequency -- the same call upstream's Cpu.GetFMax()
 *   uses (Cpu.cs:1867). That value is the ceiling for the INI caps.
 *   If the read fails or returns an implausible value (< 3000 or
 *   > 6000 MHz) the program aborts BEFORE sending any SET command
 *   (exit 1). 0x6E reads the FUSED max, which the HSMP socket cap does
 *   NOT change -- so unlike the old RSMU cap, the ceiling stays the
 *   factory max even when a cap from a previous run is still active.
 *
 * PawnIO wire format (from ZenStates-Core PawnIo/PawnIo.cs, verified against
 * the decompiled 2.0 binary -- see SLOW_MODE.md):
 *   device: \\\\?\\GLOBALROOT\\Device\\PawnIO, GENERIC_READ|GENERIC_WRITE
 *   IOCTL_PIO_LOAD_BINARY = (41394<<16) | (0x821<<2) = 0xA1B22084
 *   IOCTL_PIO_EXECUTE_FN  = (41394<<16) | (0x841<<2) = 0xA1B22104
 *   execute input  = 32-byte ASCII function name (NUL padded) + int64 args
 *   execute output = int64 array
 *   NOTE: some AI-generated summaries quote 0x0A148208 / 0x0A148210 for these
 *   IOCTLs. That contradicts the source arithmetic above and is NOT used here.
 *   If a future PawnIO version changes the device type, LoadBinary will fail
 *   and this program logs it instead of guessing.
 *
 * Safety:
 *   - Refuses to run on CPUs whose sourced SMU table has no boost-limit
 *     command (Zen 1, Zen+, Rome, APUs, Zen 5): it detects the CPU first
 *     and says why, instead of sending a code that means something else
 *     (or nothing) on that family.
 *   - Refuses to run without Administrator rights.
 *   - Every failure is a logged user-mode return. Nothing here can BSOD by
 *     itself; the only kernel work is done by the signed PawnIO driver and
 *     its signed modules, exactly as ZenStates does it.
 *   - No hardware was available in the build VM, so the SMU sequence is
 *     STATICALLY verified only (compile + protocol review). First run on
 *     real hardware is a TEST: check HWMonitor max frequency afterwards.
 *
 * Config: zenstates-c.ini next to the exe (see zenstates-c.ini for fields).
 *   Missing file / missing key -> log a warning, use defaults, keep going.
 *
 * Build (Linux cross-compile, verified):
 *   x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o zenstates-c.exe zenstates-c.c zenstates-c-res.o
 * Build (Windows, MinGW-w64): same command. With MSVC cl: see build.bat.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <cpuid.h>

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

#define DEFAULT_SINGLE_MHZ  4000
#define DEFAULT_ALL_MHZ     3900
#define DEFAULT_PPT_W       88
#define DEFAULT_TDC_A       60
#define DEFAULT_EDC_A       90

/* Wide sanity clamps: never let a typo in the INI send a dangerous value.
 * The frequency ceiling is NOT here any more -- it is the Fmax read back
 * from the SMU at startup (see main); these are only typo guards.
 * FREQ_FLOOR 3000 MHz is below every supported CPU's stock maximum.
 * Power ranges cover AM5 stock limits (7950X class: ~200 W PPT) as well
 * as the 3600's 88/60/90; they are typo guards, not per-CPU stock. */
#define FREQ_FLOOR 3000
#define FREQ_CEIL  6000
#define PPT_MIN    45
#define PPT_MAX    300
#define TDC_MIN    30
#define TDC_MAX    200
#define EDC_MIN    45
#define EDC_MAX    300

typedef struct {
    int single_mhz;   /* [Boost] SingleCoreMaxMHz */
    int all_mhz;      /* [Boost] AllCoreMaxMHz    */
    int ppt_w;        /* [Power] PPT_W (only sent if ppt_set) */
    int tdc_a;        /* [Power] TDC_A (only sent if tdc_set) */
    int edc_a;        /* [Power] EDC_A (only sent if edc_set) */
    int ppt_set;      /* [Power] PPT_W was present in the INI */
    int tdc_set;      /* [Power] TDC_A was present in the INI */
    int edc_set;      /* [Power] EDC_A was present in the INI */
    int auto_apply;   /* [Startup] AutoApply: read, not acted on by this exe */
    char log_path[MAX_PATH];
    char ini_path[MAX_PATH];
    int ini_found;
} Config;

static FILE *g_log = NULL;
static char g_exe_dir[MAX_PATH] = "";

static void log_msg(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    SYSTEMTIME st;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    GetLocalTime(&st);
    if (g_log) {
        fprintf(g_log, "[%04u-%02u-%02u %02u:%02u:%02u] %s\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, buf);
        fflush(g_log);
    }
    printf("[%02u:%02u:%02u] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
    fflush(stdout);
}

static int clamp_int(int v, int lo, int hi, const char *name)
{
    if (v < lo) { log_msg("WARN: %s=%d below %d, clamped", name, v, lo); return lo; }
    if (v > hi) { log_msg("WARN: %s=%d above %d, clamped", name, v, hi); return hi; }
    return v;
}

/* Join dir + "\\" + name into out (size out_size). Returns 1 on success,
 * 0 if it would not fit (callers fall back to the bare name). This avoids
 * snprintf("%s\\%s") into a MAX_PATH buffer, which newer gcc flags with
 * -Wformat-truncation because the directory part can be up to MAX_PATH. */
static int path_join(char *out, size_t out_size, const char *dir, const char *name)
{
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl + 1 + nl + 1 > out_size) return 0;
    memcpy(out, dir, dl);
    out[dl] = '\\';
    memcpy(out + dl + 1, name, nl + 1);
    return 1;
}

static void get_exe_dir(void)
{
    char path[MAX_PATH];
    char *slash;
    GetModuleFileNameA(NULL, path, sizeof(path));
    slash = strrchr(path, '\\');
    if (slash) {
        size_t n = (size_t)(slash - path);
        if (n >= sizeof(g_exe_dir)) n = sizeof(g_exe_dir) - 1;
        memcpy(g_exe_dir, path, n);
        g_exe_dir[n] = '\0';
    } else {
        strcpy(g_exe_dir, ".");
    }
}

/* INI path resolution: exe dir first, then %ProgramData%\ZenStates-C. */
static void resolve_ini_path(Config *c)
{
    char cand[MAX_PATH], cand2[MAX_PATH];
    if (path_join(cand, sizeof(cand), g_exe_dir, "zenstates-c.ini") &&
        GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) {
        strcpy(c->ini_path, cand);
        c->ini_found = 1;
        return;
    }
    if (GetEnvironmentVariableA("ProgramData", cand, sizeof(cand)) > 0 &&
        path_join(cand2, sizeof(cand2), cand, "ZenStates-C") &&
        path_join(cand, sizeof(cand), cand2, "zenstates-c.ini") &&
        GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) {
        strcpy(c->ini_path, cand);
        c->ini_found = 1;
        return;
    }
    /* Not found: point at the exe-dir path anyway so the GUI knows where
       to create it, and run on defaults. */
    if (!path_join(c->ini_path, sizeof(c->ini_path), g_exe_dir, "zenstates-c.ini"))
        strcpy(c->ini_path, "zenstates-c.ini");
    c->ini_found = 0;
}

static void load_config(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->single_mhz = DEFAULT_SINGLE_MHZ;
    c->all_mhz    = DEFAULT_ALL_MHZ;
    c->ppt_w      = DEFAULT_PPT_W;
    c->tdc_a      = DEFAULT_TDC_A;
    c->edc_a      = DEFAULT_EDC_A;
    c->auto_apply = 1;
    if (!path_join(c->log_path, sizeof(c->log_path), g_exe_dir, "zenstates-c.log"))
        strcpy(c->log_path, "zenstates-c.log");

    resolve_ini_path(c);
    if (!c->ini_found) {
        log_msg("WARN: no INI found at %s; using defaults "
                "single=%d all=%d, power limits untouched",
                c->ini_path, c->single_mhz, c->all_mhz);
        return;
    }
    log_msg("INI: %s", c->ini_path);
    c->single_mhz = GetPrivateProfileIntA("Boost", "SingleCoreMaxMHz",
                                          DEFAULT_SINGLE_MHZ, c->ini_path);
    c->all_mhz    = GetPrivateProfileIntA("Boost", "AllCoreMaxMHz",
                                          DEFAULT_ALL_MHZ, c->ini_path);
    /* Power limits are OPTIONAL (0.2.0): only sent when the key is present
     * in the INI. Absent keys leave the limit exactly as the BIOS set it.
     * GetPrivateProfileStringA returns 0 when the key is missing. */
    {
        char s[32];
        if (GetPrivateProfileStringA("Power", "PPT_W", "", s, sizeof(s), c->ini_path) > 0 && s[0]) {
            c->ppt_set = 1;
            c->ppt_w = GetPrivateProfileIntA("Power", "PPT_W", DEFAULT_PPT_W, c->ini_path);
        }
        if (GetPrivateProfileStringA("Power", "TDC_A", "", s, sizeof(s), c->ini_path) > 0 && s[0]) {
            c->tdc_set = 1;
            c->tdc_a = GetPrivateProfileIntA("Power", "TDC_A", DEFAULT_TDC_A, c->ini_path);
        }
        if (GetPrivateProfileStringA("Power", "EDC_A", "", s, sizeof(s), c->ini_path) > 0 && s[0]) {
            c->edc_set = 1;
            c->edc_a = GetPrivateProfileIntA("Power", "EDC_A", DEFAULT_EDC_A, c->ini_path);
        }
    }
    c->auto_apply = GetPrivateProfileIntA("Startup", "AutoApply",
                                          1, c->ini_path);
    {
        char lp[MAX_PATH];
        if (GetPrivateProfileStringA("Logging", "LogPath", "",
                                     lp, sizeof(lp), c->ini_path) > 0 && lp[0])
            strcpy(c->log_path, lp);
    }
    /* Wide sanity clamps only; the Fmax ceiling is applied in main after
     * the SMU read-back. */
    c->single_mhz = clamp_int(c->single_mhz, FREQ_FLOOR, FREQ_CEIL, "SingleCoreMaxMHz");
    c->all_mhz    = clamp_int(c->all_mhz, FREQ_FLOOR, FREQ_CEIL, "AllCoreMaxMHz");
    if (c->all_mhz > c->single_mhz) {
        log_msg("WARN: AllCoreMaxMHz (%d) > SingleCoreMaxMHz (%d); clamping all-core to single-core",
                c->all_mhz, c->single_mhz);
        c->all_mhz = c->single_mhz;
    }
    if (c->ppt_set) c->ppt_w = clamp_int(c->ppt_w, PPT_MIN, PPT_MAX, "PPT_W");
    if (c->tdc_set) c->tdc_a = clamp_int(c->tdc_a, TDC_MIN, TDC_MAX, "TDC_A");
    if (c->edc_set) c->edc_a = clamp_int(c->edc_a, EDC_MIN, EDC_MAX, "EDC_A");
    {
        char pb[32], tb[32], eb[32];
        if (c->ppt_set) snprintf(pb, sizeof(pb), "%d W", c->ppt_w); else strcpy(pb, "untouched");
        if (c->tdc_set) snprintf(tb, sizeof(tb), "%d A", c->tdc_a); else strcpy(tb, "untouched");
        if (c->edc_set) snprintf(eb, sizeof(eb), "%d A", c->edc_a); else strcpy(eb, "untouched");
        log_msg("config: single=%d MHz all=%d MHz ppt=%s tdc=%s edc=%s auto_apply=%d",
                c->single_mhz, c->all_mhz, pb, tb, eb, c->auto_apply);
    }
}

/* ------------------------------------------------------------------ */
/* Preconditions: admin + Matisse                                      */
/* ------------------------------------------------------------------ */

static bool is_admin(void)
{
    BOOL ok = FALSE;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID admin = NULL;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admin))
        return false;
    CheckTokenMembership(NULL, admin, &ok);
    FreeSid(admin);
    return ok == TRUE;
}

/* ------------------------------------------------------------------ */
/* CPU detection + per-family SMU table (0.2.0)                        */
/* ------------------------------------------------------------------ */

/* One row per supported family. The boost cap is HSMP 0x9 (socket-wide)
 * for every supported family, so there is no per-family cap command in
 * this table any more. cmd_ppt/tdc/edc are the RSMU DPTC power codes
 * (still RSMU, still OPTIONAL). cmd_get_limit (0x6E) reads the fused
 * stock max. Sources: ZenStates-Core 2.0 Hardware/SMU.cs tables and
 * Cpu.cs codename map. */
typedef struct {
    const char *name;        /* e.g. "Matisse (Zen 2)" */
    uint32_t cmd_ppt;
    uint32_t cmd_tdc;
    uint32_t cmd_edc;
    uint32_t cmd_get_limit;  /* 0x6E: fused stock max (ceiling) */
    const char *note;        /* printed at startup, may be NULL */
} FamilyTable;

static const FamilyTable FAM_MATISSE = {
    "Matisse / Castle Peak (Zen 2)",
    0x53, 0x54, 0x55, 0x6E, NULL
};
static const FamilyTable FAM_ZEN3 = {
    "Vermeer / Chagall / Milan (Zen 3)",
    0x53, 0x54, 0x55, 0x6E,
    "NOTE: HSMP on Zen 3 is UNVERIFIED (same mailbox/messages as Zen 2, "
    "but never run there). The interface-version gate decides at runtime."
};
static const FamilyTable FAM_ZEN4 = {
    "Raphael / Dragon Range / Genoa / Storm Peak (Zen 4)",
    0x56, 0x57, 0x58, 0x6E,
    "NOTE: HSMP on Zen 4 is UNVERIFIED (same mailbox/messages as Zen 2, "
    "but never run there). The interface-version gate decides at runtime."
};

/* Codename lookup for logging and the refuse message (Cpu.cs). NULL if unknown. */
static const char *codename_for(unsigned int family, unsigned int model)
{
    if (family == 0x17) {
        switch (model) {
        case 0x01: return "Summit Ridge (Zen 1)";
        case 0x08: return "Pinnacle Ridge (Zen+)";
        case 0x11: return "Raven Ridge (Zen 1 APU)";
        case 0x18: return "Picasso (Zen+ APU)";
        case 0x31: return "Rome (EPYC) or Castle Peak (Threadripper 3000)";
        case 0x60: return "Renoir (Zen 2 APU)";
        case 0x68: return "Lucienne (Zen 2 APU)";
        case 0x71: return "Matisse (Zen 2)";
        case 0x90: case 0x91: return "Van Gogh (Zen 2 APU)";
        case 0x98: return "Mero (Zen 2 APU)";
        case 0xA0: return "Mendocino (Zen 2 APU)";
        }
    } else if (family == 0x19) {
        switch (model) {
        case 0x01: return "Milan (Zen 3)";
        case 0x08: return "Chagall (Zen 3)";
        case 0x11: return "Genoa (Zen 4)";
        case 0x18: return "Storm Peak (Zen 4)";
        case 0x21: return "Vermeer (Zen 3)";
        case 0x44: return "Rembrandt (Zen 3+ APU)";
        case 0x50: return "Cezanne (Zen 3 APU)";
        case 0x61: return "Raphael / Dragon Range (Zen 4)";
        }
    } else if (family == 0x1A) {
        return "Zen 5";
    }
    return NULL;
}

/* Detects the CPU and returns its family table, or NULL (with a logged
 * reason) when the CPU is not supported. Also logs the CPUID. */
static const FamilyTable *detect_cpu(void)
{
    unsigned int eax, ebx, ecx, edx;
    char vendor[13];
    unsigned int family, model;
    const char *name;
    if (!__get_cpuid(0, &eax, &ebx, &ecx, &edx)) {
        log_msg("ERROR: CPUID leaf 0 failed");
        return NULL;
    }
    memcpy(vendor + 0, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        log_msg("ERROR: CPUID leaf 1 failed");
        return NULL;
    }
    family = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);
    model  = ((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4);
    name = codename_for(family, model);
    log_msg("CPU: vendor=%s family=0x%X model=0x%X cpuid=0x%08X",
            vendor, family, model, eax);
    if (name) log_msg("CPU: %s", name);
    if (strcmp(vendor, "AuthenticAMD") != 0) {
        log_msg("ERROR: refusing to run: not an AMD CPU (vendor=%s).", vendor);
        return NULL;
    }
    if (family == 0x17) {
        switch (model) {
        case 0x71:
            return &FAM_MATISSE;
        case 0x31:
            /* Rome (EPYC) or Castle Peak (Threadripper 3000): same CPUID,
             * package type would tell them apart. Attempt the Matisse
             * table; Rome's SMU has no such commands and will reject them. */
            log_msg("CPU: Family 17h Model 31h is Rome (EPYC, no cap support) or "
                    "Castle Peak (Threadripper 3000, Matisse table). Attempting "
                    "the Matisse table; if the SMU rejects the commands, it is Rome.");
            return &FAM_MATISSE;
        default:
            log_msg("ERROR: refusing to run: %s has no boost-limit command in "
                    "the sourced ZenStates table, so there is no safe cap to send.",
                    name ? name : "this CPU");
            return NULL;
        }
    }
    if (family == 0x19) {
        switch (model) {
        case 0x21: case 0x08: case 0x01:
            return &FAM_ZEN3;
        case 0x61: case 0x11: case 0x18:
            return &FAM_ZEN4;
        default:
            log_msg("ERROR: refusing to run: %s has no boost-limit command in "
                    "the sourced ZenStates table, so there is no safe cap to send.",
                    name ? name : "this CPU");
            return NULL;
        }
    }
    log_msg("ERROR: refusing to run: family 0x%X is not a supported Zen generation "
            "(Zen 2 / Zen 3 / Zen 4 desktop).", family);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* PawnIO driver access                                                */
/* ------------------------------------------------------------------ */

/* From ZenStates-Core PawnIo/PawnIo.cs:
 *   DEVICE_TYPE = 41394u << 16; IOCTL_PIO_LOAD_BINARY = 0x821 << 2;
 *   IOCTL_PIO_EXECUTE_FN = 0x841 << 2.
 * 41394 = 0xA1B2, so LOAD = 0xA1B22084, EXECUTE = 0xA1B22104. */
#define PAWNIO_DEVICE_TYPE   (41394u << 16)
#define PAWNIO_IOCTL_LOAD    (PAWNIO_DEVICE_TYPE | (0x821u << 2))
#define PAWNIO_IOCTL_EXECUTE (PAWNIO_DEVICE_TYPE | (0x841u << 2))
#define PAWNIO_FN_NAME_LEN   32

static HANDLE pawn_open(void)
{
    HANDLE h = CreateFileA("\\\\?\\GLOBALROOT\\Device\\PawnIO",
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        log_msg("ERROR: cannot open PawnIO device (GetLastError=%lu). "
                "Is PawnIO installed and running? (https://pawnio.eu/) "
                "This program must run as Administrator.",
                (unsigned long)GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

/* Load one .bin module on its own handle (same as ZenStates: one PawnIo
 * instance / handle per module). Returns handle or INVALID_HANDLE_VALUE. */
static HANDLE pawn_load_module(const char *bin_path)
{
    FILE *f;
    long size;
    unsigned char *buf = NULL;
    HANDLE h;
    DWORD done = 0;
    BOOL ok;

    f = fopen(bin_path, "rb");
    if (!f) {
        log_msg("ERROR: cannot read module file %s", bin_path);
        return INVALID_HANDLE_VALUE;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (long)(16u * 1024u * 1024u)) {
        log_msg("ERROR: implausible module size %ld for %s", size, bin_path);
        fclose(f);
        return INVALID_HANDLE_VALUE;
    }
    buf = (unsigned char *)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        log_msg("ERROR: cannot read module file %s", bin_path);
        free(buf);
        fclose(f);
        return INVALID_HANDLE_VALUE;
    }
    fclose(f);

    h = pawn_open();
    if (h == INVALID_HANDLE_VALUE) { free(buf); return INVALID_HANDLE_VALUE; }
    ok = DeviceIoControl(h, PAWNIO_IOCTL_LOAD, buf, (DWORD)size,
                         NULL, 0, &done, NULL);
    free(buf);
    if (!ok) {
        log_msg("ERROR: IOCTL_PIO_LOAD_BINARY (0x%08X) failed for %s, GetLastError=%lu. "
                "PawnIO version mismatch is the usual cause; check the installed "
                "PawnIO version against the modules shipped here.",
                PAWNIO_IOCTL_LOAD, bin_path, (unsigned long)GetLastError());
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    log_msg("PawnIO: loaded module %s (%ld bytes)", bin_path, size);
    return h;
}

/* Execute one named function in a loaded module.
 * in[]/out[] are int64 arrays, like PawnIo.ExecuteHr in C#.
 * Returns 0 on success (DeviceIoControl OK), else GetLastError(). */
static int pawn_execute(HANDLE h, const char *fn,
                        const int64_t *in, unsigned int in_count,
                        int64_t *out, unsigned int out_count)
{
    unsigned char inbuf[PAWNIO_FN_NAME_LEN + 16 * 8];
    unsigned char outbuf[16 * 8];
    DWORD returned = 0;
    BOOL ok;
    size_t name_len;

    if (!h || h == INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
    memset(inbuf, 0, sizeof(inbuf));
    name_len = strlen(fn);
    if (name_len >= PAWNIO_FN_NAME_LEN) return ERROR_INVALID_PARAMETER;
    memcpy(inbuf, fn, name_len);
    if (in_count > 16) return ERROR_INVALID_PARAMETER;
    if (in && in_count > 0)
        memcpy(inbuf + PAWNIO_FN_NAME_LEN, in, in_count * sizeof(int64_t));
    memset(outbuf, 0, sizeof(outbuf));
    ok = DeviceIoControl(h, PAWNIO_IOCTL_EXECUTE,
                         inbuf, (DWORD)(PAWNIO_FN_NAME_LEN + in_count * 8),
                         outbuf, (DWORD)(out_count * 8),
                         &returned, NULL);
    if (!ok) return (int)GetLastError();
    if (out && out_count)
        memcpy(out, outbuf, returned < out_count * 8 ? returned : out_count * 8);
    return 0;
}

static HANDLE g_smu = INVALID_HANDLE_VALUE;  /* RyzenSMU.bin */
static HANDLE g_msr = INVALID_HANDLE_VALUE;  /* AMDFamily17.bin */

static bool find_module(const char *name, char *out, size_t out_size)
{
    /* 1) exe dir \ modules \ name   2) exe dir \ name */
    char moddir[MAX_PATH];
    if (path_join(moddir, sizeof(moddir), g_exe_dir, "modules") &&
        path_join(out, out_size, moddir, name) &&
        GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return true;
    if (path_join(out, out_size, g_exe_dir, name) &&
        GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return true;
    return false;
}

/* SMU: ioctl_send_smu_command, in = [cmd, arg0..arg5], out = [arg0..arg5].
 * The RyzenSMU module runs the mailbox protocol itself (poll RSP, clear
 * RSP, write args, write MSG, poll RSP) against the Matisse RSMU mailbox
 * MSG 0x03B10524 / RSP 0x03B10570 / ARG 0x03B10A40. Returns 0 on SMU OK. */
static int smu_send(uint32_t cmd, uint32_t arg0)
{
    int64_t in[7] = { (int64_t)cmd, (int64_t)arg0, 0, 0, 0, 0, 0 };
    int64_t out[6] = { 0 };
    int rc = pawn_execute(g_smu, "ioctl_send_smu_command", in, 7, out, 6);
    if (rc != 0) {
        log_msg("ERROR: SMU cmd 0x%02X arg %u failed, win32=%d (0x%X)",
                cmd, arg0, rc, rc);
        return rc;
    }
    log_msg("SMU: cmd 0x%02X arg %u -> OK (returned arg0=%lld)",
            cmd, arg0, (long long)out[0]);
    return 0;
}

static int smu_get_version(uint32_t *ver)
{
    int64_t out[1] = { 0 };
    int rc = pawn_execute(g_smu, "ioctl_get_smu_version", NULL, 0, out, 1);
    if (rc != 0) return rc;
    *ver = (uint32_t)(out[0] & 0xFFFFFFFFu);
    return 0;
}

/* ------------------------------------------------------------------ */
/* HSMP (Host System Management Port) -- the boost-cap transport       */
/* ------------------------------------------------------------------ */

/* HSMP mailbox (Zen 2/3/4; ZenStates-Core Zen2Settings/Zen3Settings/
 * Zen4Settings). Zen 5 uses a different MSG address and is refused. */
#define HSMP_MSG  0x3B10534u
#define HSMP_RSP  0x3B10980u
#define HSMP_ARG  0x3B109E0u
#define HSMP_MAX_ARGS 8

/* HSMP messages (Linux kernel amd_hsmp.h; HSMPMailbox.cs). */
#define HSMP_GET_IFACE_VERSION   0x3u
#define HSMP_SET_BOOST_LIMIT     0x8u
#define HSMP_SET_BOOST_LIMIT_ALL 0x9u
#define HSMP_GET_BOOST_LIMIT     0xAu

/* SMU status codes (SMU.cs). */
#define SMU_OK               0x01u
#define SMU_FAILED           0xFFu
#define SMU_UNKNOWN_CMD      0xFEu
#define SMU_REJECTED_PREREQ  0xFDu
#define SMU_REJECTED_BUSY    0xFCu

/* Raw SMU register access (RyzenSmu.cs: ioctl_read/write_smu_register).
 * These read/write ANY SMU register by address -- unlike
 * ioctl_send_smu_command, they are not tied to the RSMU mailbox. */
static int smu_reg_read(uint32_t reg, uint32_t *val)
{
    int64_t in[1] = { (int64_t)reg };
    int64_t out[1] = { 0 };
    int rc = pawn_execute(g_smu, "ioctl_read_smu_register", in, 1, out, 1);
    if (rc != 0) return rc;
    *val = (uint32_t)(out[0] & 0xFFFFFFFFu);
    return 0;
}

static int smu_reg_write(uint32_t reg, uint32_t val)
{
    int64_t in[2] = { (int64_t)reg, (int64_t)val };
    return pawn_execute(g_smu, "ioctl_write_smu_register", in, 2, NULL, 0);
}

/* Wait until the HSMP response register is non-zero (previous command
 * done). Mirrors SmuWaitDoneNoLock: 8192 iterations, 500 ms hang guard. */
static int hsmp_wait_done(void)
{
    unsigned int timeout = 8192;
    ULONGLONG deadline = GetTickCount64() + 500;
    uint32_t data = 0;
    for (;;) {
        if (smu_reg_read(HSMP_RSP, &data) != 0) return 0;
        if (data != 0) return 1;
        if (--timeout == 0) return 0;
        if (GetTickCount64() >= deadline) return 0;
    }
}

/* Send one HSMP message. args_in[0..n_in) are written (padded to 8 with
 * zero); on SMU OK, args_out[0..n_out) are read back. Returns the SMU
 * status byte (0x01 = OK), or -1 on transport failure (PCI/timeout).
 * The caller holds the PCI lock. */
static int hsmp_send(uint32_t msg, const uint32_t *args_in, unsigned int n_in,
                     uint32_t *args_out, unsigned int n_out)
{
    unsigned int i;
    uint32_t status = 0;

    if (!hsmp_wait_done()) return -1;
    if (smu_reg_write(HSMP_RSP, 0) != 0) return -1;
    for (i = 0; i < HSMP_MAX_ARGS; i++) {
        uint32_t v = (i < n_in) ? args_in[i] : 0;
        if (smu_reg_write(HSMP_ARG + i * 4u, v) != 0) return -1;
    }
    if (smu_reg_write(HSMP_MSG, msg) != 0) return -1;
    if (!hsmp_wait_done()) return -1;
    if (smu_reg_read(HSMP_RSP, &status) != 0) return -1;
    if (status == SMU_OK) {
        for (i = 0; i < n_out; i++) {
            if (smu_reg_read(HSMP_ARG + i * 4u, &args_out[i]) != 0) return -1;
        }
    }
    return (int)status;
}

static const char *smu_status_name(int st)
{
    switch (st) {
    case (int)SMU_OK: return "OK";
    case (int)SMU_FAILED: return "FAILED";
    case (int)SMU_UNKNOWN_CMD: return "UNKNOWN_CMD";
    case (int)SMU_REJECTED_PREREQ: return "CMD_REJECTED_PREREQ";
    case (int)SMU_REJECTED_BUSY: return "CMD_REJECTED_BUSY";
    case -1: return "TRANSPORT_ERROR (PCI/timeout)";
    default: return "unknown status";
    }
}

/* Initial APIC ID of logical core `core` (CPUID leaf 1, EBX[31:24]). */
static int apic_id_of_core(int core)
{
    DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(), ((DWORD_PTR)1) << core);
    unsigned int eax, ebx, ecx, edx;
    int apic = -1;
    if (old == 0) return -1;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        apic = (int)((ebx >> 24) & 0xFFu);
    SetThreadAffinityMask(GetCurrentThread(), old);
    return apic;
}

/* MSR via AMDFamily17 module: ioctl_read_msr [index] -> [value],
 * ioctl_write_msr [index, value] -> []. */
static int msr_read(uint32_t index, uint64_t *value)
{
    int64_t in[1] = { (int64_t)index };
    int64_t out[1] = { 0 };
    int rc = pawn_execute(g_msr, "ioctl_read_msr", in, 1, out, 1);
    if (rc != 0) return rc;
    *value = (uint64_t)out[0];
    return 0;
}

static int msr_write(uint32_t index, uint64_t value)
{
    int64_t in[2] = { (int64_t)index, (int64_t)value };
    int rc = pawn_execute(g_msr, "ioctl_write_msr", in, 2, NULL, 0);
    return rc;
}

/* Old ZenStates set MSR_HWCR (0xC0010015) bit 21 on every thread before
 * sending boost-limit commands. Best effort: a failure here is a warning,
 * not fatal -- ZenStates 2.0's core does not do this step at all. */
#define MSR_HWCR 0xC0010015u
static void hwcr_fix(void)
{
    SYSTEM_INFO si;
    DWORD n, i;
    GetSystemInfo(&si);
    n = si.dwNumberOfProcessors;
    if (n == 0 || n > 64) n = 64;
    log_msg("HWCR fix: %lu logical processors, MSR 0xC0010015 bit 21", (unsigned long)n);
    for (i = 0; i < n; i++) {
        DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1u << i);
        uint64_t v = 0;
        int rc;
        if (old == 0) {
            log_msg("WARN: HWCR: cannot pin thread to core %lu, skipping", (unsigned long)i);
            continue;
        }
        rc = msr_read(MSR_HWCR, &v);
        if (rc != 0) {
            log_msg("WARN: HWCR: read MSR on core %lu failed (win32=%d)", (unsigned long)i, rc);
        } else if ((v & 0x200000u) == 0) {
            rc = msr_write(MSR_HWCR, v | 0x200000u);
            log_msg("HWCR: core %lu bit21 was clear, set -> %s (win32=%d)",
                    (unsigned long)i, rc == 0 ? "OK" : "FAILED", rc);
        }
        SetThreadAffinityMask(GetCurrentThread(), old);
    }
}

/* Global\Access_PCI mutex, as in ZenStates-Core Mutexes.cs. Best effort. */
static HANDLE g_pci_mutex = NULL;
static void pci_lock(void)
{
    g_pci_mutex = OpenMutexA(SYNCHRONIZE, FALSE, "Global\\Access_PCI");
    if (!g_pci_mutex)
        g_pci_mutex = CreateMutexA(NULL, FALSE, "Global\\Access_PCI");
    if (g_pci_mutex) {
        DWORD w = WaitForSingleObject(g_pci_mutex, 5000);
        if (w != WAIT_OBJECT_0) {
            log_msg("WARN: could not acquire Global\\Access_PCI within 5 s (wait=%lu); continuing without the lock",
                    (unsigned long)w);
            CloseHandle(g_pci_mutex);
            g_pci_mutex = NULL;
        }
    } else {
        log_msg("WARN: could not open/create Global\\Access_PCI; continuing without the lock");
    }
}
static void pci_unlock(void)
{
    if (g_pci_mutex) {
        ReleaseMutex(g_pci_mutex);
        CloseHandle(g_pci_mutex);
        g_pci_mutex = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    Config cfg;
    char smu_bin[MAX_PATH], msr_bin[MAX_PATH];
    uint32_t smu_ver = 0;
    int failures = 0;
    const FamilyTable *fam = NULL;
    int stock_max = 0;
    int effective_cap = 0;  /* HSMP socket cap = min(single, all) */
    int read_mode = 0;      /* --read: read the CPU config into the INI, apply nothing */
    {
        int i;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--read") == 0) read_mode = 1;
        }
    }

    get_exe_dir();
    /* Log file is opened after the INI is read in a second pass below;
     * start with stdout only, then reopen once LogPath is known. */
    load_config(&cfg);  /* logs to stdout for now (g_log is NULL) */
    g_log = fopen(cfg.log_path, "a");
    if (g_log) {
        log_msg("zenstates-c starting, log=%s ini=%s (found=%d)",
                cfg.log_path, cfg.ini_path, cfg.ini_found);
    } else {
        printf("WARN: cannot open log file %s; logging to stdout only\n", cfg.log_path);
    }

    if (!is_admin()) {
        log_msg("ERROR: not running as Administrator. Right-click -> Run as administrator, "
                "or use the scheduled-task install (install-autostart.bat), which runs as SYSTEM.");
        if (g_log) fclose(g_log);
        return 2;
    }
    fam = detect_cpu();
    if (!fam) {
        if (g_log) fclose(g_log);
        return 2;
    }
    log_msg("family table: %s", fam->name);
    if (fam->note) log_msg("%s", fam->note);

    if (!find_module("RyzenSMU.bin", smu_bin, sizeof(smu_bin)) ||
        !find_module("AMDFamily17.bin", msr_bin, sizeof(msr_bin))) {
        log_msg("ERROR: RyzenSMU.bin / AMDFamily17.bin not found next to the exe "
                "(looked in .\\modules\\ and exe dir). They ship in the modules\\ folder.");
        if (g_log) fclose(g_log);
        return 1;
    }
    g_smu = pawn_load_module(smu_bin);
    g_msr = pawn_load_module(msr_bin);
    if (g_smu == INVALID_HANDLE_VALUE || g_msr == INVALID_HANDLE_VALUE) {
        log_msg("ERROR: failed to load PawnIO modules. See errors above.");
        if (g_log) fclose(g_log);
        return 1;
    }

    if (smu_get_version(&smu_ver) != 0 || smu_ver == 0) {
        log_msg("ERROR: ioctl_get_smu_version failed or returned 0; aborting without sending any SMU command.");
        if (g_log) fclose(g_log);
        return 1;
    }
    log_msg("SMU version: 0x%08X", smu_ver);

    hwcr_fix();

    /* Fmax: read the stock boost limit BEFORE sending any cap. This is the
     * ceiling for the INI caps; upstream's Cpu.GetFMax() is the same 0x6E
     * call. Abort before sending anything if it fails or is implausible. */
    pci_lock();
    {
        int64_t in[7] = { (int64_t)fam->cmd_get_limit, 0, 0, 0, 0, 0, 0 };
        int64_t out[6] = { 0 };
        int rc = pawn_execute(g_smu, "ioctl_send_smu_command", in, 7, out, 6);
        if (rc != 0 || out[0] < FREQ_FLOOR || out[0] > FREQ_CEIL) {
            log_msg("ERROR: stock boost limit read-back (0x%02X) failed or implausible "
                    "(win32=%d, value=%lld MHz). Aborting before sending any cap.",
                    fam->cmd_get_limit, rc, (long long)out[0]);
            pci_unlock();
            if (g_smu != INVALID_HANDLE_VALUE) CloseHandle(g_smu);
            if (g_msr != INVALID_HANDLE_VALUE) CloseHandle(g_msr);
            if (g_log) fclose(g_log);
            return 1;
        }
        stock_max = (int)out[0];
        log_msg("stock boost limit (Fmax, 0x6E read-back) = %d MHz", stock_max);
    }

    /* --read mode: read the current config from the CPU and write it into
     * the INI, then exit without applying anything. Read-only: no SMU
     * command here writes a setting. Runs inside the PCI lock (hsmp_send
     * requires it), same as the apply path. */
    if (read_mode) {
        int apic0 = apic_id_of_core(0);
        uint32_t outarg[1] = { 0 };
        int st;
        int cur_cap = 0;
        char b[16];
        if (apic0 < 0) {
            log_msg("ERROR: --read: could not read APIC ID of core 0.");
            pci_unlock();
            if (g_smu != INVALID_HANDLE_VALUE) CloseHandle(g_smu);
            if (g_msr != INVALID_HANDLE_VALUE) CloseHandle(g_msr);
            if (g_log) fclose(g_log);
            return 1;
        }
        st = hsmp_send(HSMP_GET_IFACE_VERSION, NULL, 0, outarg, 1);
        log_msg("HSMP GetInterfaceVersion (0x3): status=0x%02X (%s), version=%u",
                (unsigned)st, smu_status_name(st), outarg[0]);
        if (st != (int)SMU_OK || outarg[0] == 0) {
            log_msg("ERROR: --read: HSMP NOT SUPPORTED on this CPU/firmware; cannot read the cap.");
            pci_unlock();
            if (g_smu != INVALID_HANDLE_VALUE) CloseHandle(g_smu);
            if (g_msr != INVALID_HANDLE_VALUE) CloseHandle(g_msr);
            if (g_log) fclose(g_log);
            return 1;
        }
        {
            uint32_t inarg[1] = { (uint32_t)apic0 };
            st = hsmp_send(HSMP_GET_BOOST_LIMIT, inarg, 1, outarg, 1);
            log_msg("HSMP GetBoostLimit (0xA, apic %d): status=0x%02X (%s), limit=%u MHz",
                    apic0, (unsigned)st, smu_status_name(st), outarg[0]);
            if (st != (int)SMU_OK || (int)outarg[0] < FREQ_FLOOR || (int)outarg[0] > FREQ_CEIL) {
                log_msg("ERROR: --read: current cap read failed or implausible.");
                pci_unlock();
                if (g_smu != INVALID_HANDLE_VALUE) CloseHandle(g_smu);
                if (g_msr != INVALID_HANDLE_VALUE) CloseHandle(g_msr);
                if (g_log) fclose(g_log);
                return 1;
            }
            cur_cap = (int)outarg[0];
        }
        /* Write the INI: Boost keys = the current socket cap (HSMP is
         * single-valued, so both keys get the same value). Power keys are
         * written as EMPTY entries -- they cannot be read back on Zen 2,
         * and an empty value means "unset" for both the main program and
         * the GUI. [Startup]/[Logging] are not touched. */
        snprintf(b, sizeof(b), "%d", cur_cap);
        WritePrivateProfileStringA("Boost", "SingleCoreMaxMHz", b, cfg.ini_path);
        WritePrivateProfileStringA("Boost", "AllCoreMaxMHz", b, cfg.ini_path);
        WritePrivateProfileStringA("Power", "PPT_W", "", cfg.ini_path);
        WritePrivateProfileStringA("Power", "TDC_A", "", cfg.ini_path);
        WritePrivateProfileStringA("Power", "EDC_A", "", cfg.ini_path);
        log_msg("--read: wrote %s: [Boost] SingleCoreMaxMHz=%d, AllCoreMaxMHz=%d "
                "(current HSMP socket cap); [Power] PPT_W/TDC_A/EDC_A left empty "
                "(cannot be read back on this CPU). Stock max (fused) = %d MHz. "
                "Nothing was applied.",
                cfg.ini_path, cur_cap, cur_cap, stock_max);
        pci_unlock();
        if (g_smu != INVALID_HANDLE_VALUE) CloseHandle(g_smu);
        if (g_msr != INVALID_HANDLE_VALUE) CloseHandle(g_msr);
        if (g_log) fclose(g_log);
        return 0;
    }

    /* Ceiling pass: clamp the INI caps to [FREQ_FLOOR, stock_max]. */
    if (cfg.single_mhz > stock_max) {
        log_msg("WARN: SingleCoreMaxMHz=%d above stock max %d; clamping to %d",
                cfg.single_mhz, stock_max, stock_max);
        cfg.single_mhz = stock_max;
    }
    if (cfg.all_mhz > stock_max) {
        log_msg("WARN: AllCoreMaxMHz=%d above stock max %d; clamping to %d",
                cfg.all_mhz, stock_max, stock_max);
        cfg.all_mhz = stock_max;
    }
    if (cfg.all_mhz > cfg.single_mhz) {
        log_msg("WARN: AllCoreMaxMHz (%d) > SingleCoreMaxMHz (%d); clamping all-core to single-core",
                cfg.all_mhz, cfg.single_mhz);
        cfg.all_mhz = cfg.single_mhz;
    }
    log_msg("caps to apply: single<=%d MHz, all-core<=%d MHz (stock max %d MHz)",
            cfg.single_mhz, cfg.all_mhz, stock_max);

    /* The boost cap: HSMP 0x9 (socket-wide). HSMP has a SINGLE socket cap,
     * so the two INI values collapse to one effective cap:
     * min(SingleCoreMaxMHz, AllCoreMaxMHz). The all<=single clamp above
     * already makes that the all-core value; compute it explicitly. */
    effective_cap = cfg.all_mhz < cfg.single_mhz ? cfg.all_mhz : cfg.single_mhz;
    log_msg("HSMP socket cap is single-valued, so the effective cap is "
            "min(single, all) = %d MHz", effective_cap);
    {
        int apic0 = apic_id_of_core(0);
        if (apic0 < 0) {
            log_msg("ERROR: could not read APIC ID of core 0; cannot verify the HSMP cap. Nothing was set.");
            failures++;
        } else {
            uint32_t inarg[1];
            uint32_t outarg[1] = { 0 };
            int st;
            /* Gate: does this SMU implement HSMP? */
            st = hsmp_send(HSMP_GET_IFACE_VERSION, NULL, 0, outarg, 1);
            log_msg("HSMP GetInterfaceVersion (0x3): status=0x%02X (%s), version=%u",
                    (unsigned)st, smu_status_name(st), outarg[0]);
            if (st != (int)SMU_OK || outarg[0] == 0) {
                log_msg("ERROR: HSMP NOT SUPPORTED on this CPU/firmware; cannot set the cap. Nothing was applied.");
                failures++;
            } else {
                /* Baseline: the HSMP limit currently set (0xA). */
                inarg[0] = (uint32_t)apic0;
                st = hsmp_send(HSMP_GET_BOOST_LIMIT, inarg, 1, outarg, 1);
                log_msg("HSMP GetBoostLimit (0xA, apic %d): status=0x%02X (%s), limit=%u MHz",
                        apic0, (unsigned)st, smu_status_name(st), outarg[0]);
                /* Set the socket cap. */
                inarg[0] = (uint32_t)effective_cap;
                st = hsmp_send(HSMP_SET_BOOST_LIMIT_ALL, inarg, 1, NULL, 0);
                log_msg("HSMP SetBoostLimitSocket (0x9, arg %d MHz): status=0x%02X (%s)",
                        effective_cap, (unsigned)st, smu_status_name(st));
                if (st != (int)SMU_OK) {
                    log_msg("ERROR: HSMP set failed; the cap was NOT applied.");
                    failures++;
                } else {
                    /* Verify via 0xA. */
                    inarg[0] = (uint32_t)apic0;
                    st = hsmp_send(HSMP_GET_BOOST_LIMIT, inarg, 1, outarg, 1);
                    log_msg("HSMP GetBoostLimit read-back (0xA): status=0x%02X (%s), limit=%u MHz%s",
                            (unsigned)st, smu_status_name(st), outarg[0],
                            (st == (int)SMU_OK && (int)outarg[0] == effective_cap) ? " (SET TOOK EFFECT)" : " (SET DID NOT TAKE EFFECT)");
                    if (st != (int)SMU_OK || (int)outarg[0] != effective_cap) {
                        log_msg("ERROR: HSMP read-back does not match the target; the cap was NOT applied.");
                        failures++;
                    }
                }
            }
        }
    }

    /* Power limits (only the ones set in the INI). Still RSMU DPTC. */
    if (cfg.ppt_set) {
        if (smu_send(fam->cmd_ppt, (uint32_t)cfg.ppt_w * 1000u) != 0) failures++; /* PPT, mW */
    } else {
        log_msg("PPT left untouched (PPT_W not set in the INI)");
    }
    if (cfg.tdc_set) {
        if (smu_send(fam->cmd_tdc, (uint32_t)cfg.tdc_a * 1000u) != 0) failures++; /* TDC, mA */
    } else {
        log_msg("TDC left untouched (TDC_A not set in the INI)");
    }
    if (cfg.edc_set) {
        if (smu_send(fam->cmd_edc, (uint32_t)cfg.edc_a * 1000u) != 0) failures++; /* EDC, mA */
    } else {
        log_msg("EDC left untouched (EDC_A not set in the INI)");
    }

    /* Final reference read (best effort, do not fail the run on it):
     * RSMU 0x6E GetBoostLimitFrequency reads the FUSED stock max, which
     * the HSMP socket cap does NOT change -- so expect the stock max here,
     * not the cap. The cap itself was already verified via HSMP 0xA above. */
    {
        int64_t in[7] = { (int64_t)fam->cmd_get_limit, 0, 0, 0, 0, 0, 0 };
        int64_t out[6] = { 0 };
        int rc = pawn_execute(g_smu, "ioctl_send_smu_command", in, 7, out, 6);
        if (rc == 0)
            log_msg("reference: GetBoostLimitFrequency (0x6E) returned %lld MHz "
                    "(fused stock max; the HSMP cap does not change it)",
                    (long long)out[0]);
        else
            log_msg("WARN: reference read-back 0x6E failed (win32=%d); check HWMonitor instead", rc);
    }
    pci_unlock();

    if (failures == 0) {
        log_msg("OK: HSMP socket boost cap applied on %s: effective cap %d MHz "
                "(min of single<=%d and all-core<=%d; HSMP is single-valued). "
                "Boost stays dynamic below the cap; verify in HWMonitor "
                "(max core clock should stop at ~%d MHz).",
                fam->name, effective_cap, cfg.single_mhz, cfg.all_mhz, effective_cap);
    } else {
        log_msg("DONE WITH %d FAILED SMU COMMAND(S) (see above). "
                "The CPU was left in whatever state the successful commands set; "
                "a reboot restores stock behaviour (nothing here is persistent).",
                failures);
    }
    if (g_smu != INVALID_HANDLE_VALUE) CloseHandle(g_smu);
    if (g_msr != INVALID_HANDLE_VALUE) CloseHandle(g_msr);
    if (g_log) fclose(g_log);
    return failures == 0 ? 0 : 3;
}
