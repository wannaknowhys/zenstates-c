/* SPDX-License-Identifier: GPL-3.0-or-later
 * zenstates-c.c -- pure C re-implementation of the ZenStates boost-cap path
 * for Ryzen desktop CPUs (Zen 2 / Zen 3 / Zen 4), 0.2.0.
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
 *   PawnIO driver -> load RyzenSMU.bin / AMDFamily17.bin modules ->
 *   ioctl_send_smu_command (RSMU mailbox) with the OLD ZenStates 0.x
 *   Matisse message codes. ZenStates 2.0's own core does NOT carry these
 *   codes for Matisse any more (SetFMax is a dead end there, cmd 0 ->
 *   UNKNOWN_CMD), so this program restores them from the 0.x source
 *   (Zen2CPUSettings, tag 2.0-beta / commit d35a93bb):
 *     RSMU MSG 0x03B10524, RSP 0x03B10570, ARG 0x03B10A40   (module-internal,
 *       handled inside RyzenSMU.bin; the C code never touches these addresses
 *       directly -- it only passes cmd + args to the module)
 *     0x29  SetBoostLimitFrequency         (single-core cap, arg = MHz)
 *     0x2B  SetBoostLimitFrequencyAllCores (all-core cap,    arg = MHz)
 *     0x53  SetFastLimit / PPT             (arg = mW)
 *     0x54  SetTDCVDDLimit                 (arg = mA)
 *     0x55  SetEDCVDDLimit                 (arg = mA)
 *     0x6E  GetBoostLimitFrequency         (verification read-back, best effort)
 *   Old code also set MSR_HWCR (0xC0010015) bit 21 on every thread before
 *   sending the boost-limit commands; this program does the same via the
 *   AMDFamily17 module, but treats a failure there as a warning, not fatal
 *   (ZenStates 2.0's core does not do it at all).
 *
 * Family dispatch (sources: ZenStates-Core 2.0, Hardware/SMU.cs tables
 * and Cpu.cs codename map; see SLOW_MODE.md for the evidence chain):
 *   Matisse / Castle Peak (17h/0x71, 17h/0x31) -- Zen2Settings
 *     0x29 single-core cap, 0x2B all-core cap, PPT 0x53, TDC 0x54, EDC 0x55
 *   Vermeer / Chagall / Milan (19h/0x21, 0x08, 0x01) -- Zen3Settings
 *     inherits the Zen 2 RSMU table unchanged, so the same codes are
 *     attempted. NOTE: 0x29/0x2B on Zen 3 are UNVERIFIED (no source
 *     evidence either way); failures are reported, nothing is applied
 *     by a failed command.
 *   Raphael / Dragon Range / Genoa / Storm Peak (19h/0x61, 0x11, 0x18)
 *     -- Zen4Settings: all-core cap 0x70 (a real SET code in the 2.0
 *     source), PPT 0x56, TDC 0x57, EDC 0x58. There is NO single-core cap
 *     code for Zen 4, so SingleCoreMaxMHz is ignored on these CPUs (logged).
 *   Zen 1 / Zen+ / Rome / APUs / Zen 5: the sourced tables contain no
 *     boost-limit command for them, so the program detects the CPU and
 *     refuses with a reason instead of sending a code that means
 *     something else (or nothing) on that family.
 *   Family 17h Model 31h is ambiguous: Rome (EPYC, no cap support) and
 *   Castle Peak (Threadripper 3000, Matisse table) share the CPUID.
 *   The Matisse table is attempted; if the SMU rejects the commands,
 *   that means Rome (nothing was applied, exit 3).
 *   Mailbox: the RyzenSMU.bin module carries both the Matisse and the
 *   Zen 1 RSMU mailbox sets and selects its own; Zen 2/3/4 share the
 *   Matisse mailbox (MSG 0x03B10524 / RSP 0x03B10570 / ARG 0x03B10A40),
 *   so the module path works for all three. The C code never touches
 *   these addresses directly -- it only passes cmd + args to the module.
 *   (0.1.0's 0x29/0x2B came from the OLD ZenStates 0.x Matisse codes,
 *   restored because ZenStates 2.0's own core does not carry them for
 *   Matisse any more; the Zen 4 0x70 code comes from the 2.0 source.)
 *
 * Fmax (stock maximum, no hardcoded 4200 any more):
 *   At startup, before sending any cap, the program reads 0x6E
 *   GetBoostLimitFrequency -- the same call upstream's Cpu.GetFMax()
 *   uses (Cpu.cs:1867). That value is the ceiling for the INI caps.
 *   If the read fails or returns an implausible value (< 3000 or
 *   > 6000 MHz) the program aborts BEFORE sending any SET command
 *   (exit 1). Note: if a cap from a previous run is still active, 0x6E
 *   returns the capped value, not the factory maximum -- the ceiling
 *   then is the current limit, which is still safe (caps only ever go
 *   down from it).
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

/* One row per supported family. cmd_* = 0 means "no such command in the
 * sourced table". Sources: ZenStates-Core 2.0 Hardware/SMU.cs tables
 * (Zen2Settings / Zen3Settings / Zen4Settings) and Cpu.cs codename map. */
typedef struct {
    const char *name;        /* e.g. "Matisse (Zen 2)" */
    uint32_t cmd_set_single; /* single-core boost cap, 0 = none */
    uint32_t cmd_set_all;    /* all-core boost cap, 0 = none */
    uint32_t cmd_ppt;
    uint32_t cmd_tdc;
    uint32_t cmd_edc;
    uint32_t cmd_get_limit;  /* 0x6E read-back */
    const char *note;        /* printed at startup, may be NULL */
} FamilyTable;

static const FamilyTable FAM_MATISSE = {
    "Matisse / Castle Peak (Zen 2)",
    0x29, 0x2B, 0x53, 0x54, 0x55, 0x6E, NULL
};
static const FamilyTable FAM_ZEN3 = {
    "Vermeer / Chagall / Milan (Zen 3)",
    0x29, 0x2B, 0x53, 0x54, 0x55, 0x6E,
    "NOTE: Zen 3 inherits the Zen 2 RSMU table, so 0x29/0x2B are attempted "
    "but UNVERIFIED on Zen 3. Failures are reported; a failed command applies nothing."
};
static const FamilyTable FAM_ZEN4 = {
    "Raphael / Dragon Range / Genoa / Storm Peak (Zen 4)",
    0, 0x70, 0x56, 0x57, 0x58, 0x6E,
    "NOTE: Zen 4 has no single-core boost-limit command in the sourced table "
    "(only all-core 0x70); SingleCoreMaxMHz is ignored on this CPU."
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

int main(void)
{
    Config cfg;
    char smu_bin[MAX_PATH], msr_bin[MAX_PATH];
    uint32_t smu_ver = 0;
    int failures = 0;
    const FamilyTable *fam = NULL;
    int stock_max = 0;

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

    /* Order: all-core cap first, then single-core cap, then power limits
     * (only the ones set in the INI). All are RSMU commands; the
     * RyzenSMU module only supports the RSMU mailbox, which is exactly
     * what these codes need. */
    if (smu_send(fam->cmd_set_all, (uint32_t)cfg.all_mhz) != 0) failures++;    /* all-core boost cap */
    if (fam->cmd_set_single != 0) {
        if (smu_send(fam->cmd_set_single, (uint32_t)cfg.single_mhz) != 0) failures++; /* single-core boost cap */
    } else {
        log_msg("single-core cap skipped: no single-core boost-limit command for %s; "
                "SingleCoreMaxMHz is ignored on this CPU", fam->name);
    }
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

    /* Verification read-back (best effort, do not fail the run on it):
     * 0x6E GetBoostLimitFrequency. Unconfirmed whether it returns the
     * single-core or the all-core limit, so log it, don't judge. */
    {
        int64_t in[7] = { (int64_t)fam->cmd_get_limit, 0, 0, 0, 0, 0, 0 };
        int64_t out[6] = { 0 };
        int rc = pawn_execute(g_smu, "ioctl_send_smu_command", in, 7, out, 6);
        if (rc == 0)
            log_msg("verify: GetBoostLimitFrequency (0x6E) returned %lld MHz "
                    "(expect ~%d; which limit this reports is unconfirmed)",
                    (long long)out[0], cfg.all_mhz);
        else
            log_msg("WARN: verify read-back 0x6E failed (win32=%d); check HWMonitor instead", rc);
    }
    pci_unlock();

    if (failures == 0) {
        if (fam->cmd_set_single != 0) {
            log_msg("OK: boost cap applied on %s: single<=%d MHz, all-core<=%d MHz. "
                    "Boost stays dynamic below the cap; verify in HWMonitor "
                    "(max core clock should stop at ~%d MHz).",
                    fam->name, cfg.single_mhz, cfg.all_mhz, cfg.single_mhz);
        } else {
            log_msg("OK: all-core boost cap applied on %s: all-core<=%d MHz "
                    "(no single-core cap command exists for this family). "
                    "Boost stays dynamic below the cap; verify in HWMonitor "
                    "(max core clock should stop at ~%d MHz).",
                    fam->name, cfg.all_mhz, cfg.all_mhz);
        }
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
