/* SPDX-License-Identifier: GPL-3.0-or-later
 * zenstates-vf.c -- empirical V/F sweep for Ryzen desktop CPUs (Zen 2 / Zen 3 / Zen 4), 0.2.2.
 *
 * Modes:
 *   (no args)  Sweep (see below).
 *   --hold [core]  Hold a load: pin a scalar spin-load to one core and
 *              print its frequency and voltage every second until Ctrl+C.
 *              Does NOT set any cap -- it holds a load against whatever cap
 *              is currently installed (set it first with zenstates-c.exe or
 *              zenstates-hsmp.exe), so you can watch HWMonitor and confirm
 *              the cap holds under sustained load. The sweep already spins
 *              the same load thread while it runs; --hold is the standalone
 *              version for when you just want to hold a core at load.
 *
 * What it measures (and what it does NOT):
 *   There is NO SMU command that dumps the fused V/F table: ZenStates-Core
 *   has no such command, and TransferTableToDram (RSMU 0x05) exports the
 *   PowerTable telemetry (live frequency / voltage / power), not a curve.
 *   The V/F curve lives inside SMU firmware as fused FIT data.
 *   So this tool measures the curve instead of reading it: for each target
 *   frequency it caps Boost to that frequency (the HSMP socket cap, the
 *   same cap zenstates-c.c uses), pins a scalar spin-load to one core so
 *   that core actually boosts to the cap, and reads the VID that SMU
 *   requests at that point. One CSV row per sample; the median per target
 *   is the V/F point.
 *
 * Scope: Matisse / Castle Peak, Vermeer / Chagall / Milan, and Raphael /
 *   Dragon Range / Genoa / Storm Peak. 0.2.1 moved the cap to HSMP
 *   (0x9 socket-wide), which is a single cap that constrains any loaded
 *   core -- so Zen 4 is now sweepable too (it was refused before because
 *   the old RSMU route had no single-core cap for it). Zen 1 / Zen+ /
 *   Rome / APUs / Zen 5 have no HSMP mailbox in the sourced table and are
 *   refused. Zen 3 and Zen 4 via HSMP are UNVERIFIED (same mailbox and
 *   messages as Zen 2, but never run there); the interface-version gate
 *   decides at runtime. See zenstates-c.c for the family table and sources.
 *
 * Fmax (no hardcoded 4200 any more): at startup the tool reads 0x6E
 *   GetBoostLimitFrequency -- the same call upstream's Cpu.GetFMax() uses.
 *   0x6E reads the FUSED stock max, which the HSMP socket cap does NOT
 *   change, so it stays the correct ceiling (and the sweep's start,
 *   unless the INI sets StartMHz) even when a cap is already installed.
 *   The RESTORE value is different: it is the HSMP limit (0xA) read
 *   before the sweep, i.e. the cap that was active when the tool started
 *   (e.g. zenstates-c's autostart cap) -- the sweep restores exactly
 *   that cap at the end and is therefore non-destructive. EndMHz 0 means
 *   Fmax - 600. If the 0x6E read fails or returns an implausible value
 *   the tool aborts before sending any cap. After every cap set the tool
 *   reads HSMP 0xA back and logs whether the new limit actually took
 *   effect -- a rejected command and a silently-ignored one look
 *   different in that read-back. (0x6E is NOT used for the per-target
 *   read-back: it reads the fused max, not the HSMP cap.)
 *
 * Read path (all from ZenStates-Core source, same as the main program):
 *   MSR 0xC0010293 HW_PSTATE_STATUS, read on the measured core via PawnIO
 *   AMDFamily17 ioctl_read_msr. The calling thread is pinned to the core
 *   first (the C equivalent of Cpu.ReadMsrTx / GroupAffinity).
 *   EAX bits: [7:0] CurCpuFid, [13:8] CurCpuDfsId, [21:14] CurCpuVid.
 *     multiplier = 2.0 * Fid / DfsId            (Cpu.GetCoreMulti)
 *     frequency  = multiplier * 100 MHz          (Cpu.GetCoreClockMhz, BCLK=100)
 *     voltage    = 1.55 - Vid * 0.00625 V        (Utils.VidToVoltage, SVI2)
 *   The spin-load runs on logical core LoadCore; the reader pins itself to
 *   the load core's true SMT sibling (found via the APIC ID, not assumed
 *   to be LoadCore^1 -- logical-processor numbering need not interleave
 *   siblings), so both observe the same core clock / VID. If no sibling
 *   exists (SMT off) the reader measures the load core itself. Both the
 *   reader core and the load core are sampled, so a wrong-core reading
 *   shows up as a mismatch instead of a silent bad curve. The load is a
 *   plain scalar integer loop (no AVX): the curve measured is the
 *   scalar-boost curve.
 *
 * What it writes: ONLY the HSMP socket-cap message (0x9) per target and,
 *   at the end, a restore of the socket cap to the HSMP limit (0xA)
 *   reported before the sweep (the pre-sweep limit, not a constant). It
 *   never writes an MSR, never sets a fixed frequency, never sets a fixed
 *   voltage or VID. A reboot also restores stock.
 *
 * Plumbing (PawnIO device, module loading, SMU send, MSR read, HWCR fix,
 * PCI lock, admin + CPU checks) is copied from zenstates-c.c in this
 * same repo, which documents the evidence chain in SLOW_MODE.md.
 *
 * Config: zenstates-vf.ini next to the exe (see zenstates-vf.ini).
 * Output: CSV next to the exe by default (zenstates-vf.csv).
 *
 * Build (Linux cross-compile, verified):
 *   x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o zenstates-vf.exe zenstates-vf.c zenstates-vf-res.o
 * Build (Windows, MinGW-w64): see build.bat.
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

/* StartMHz 0 (or absent) = start at the Fmax read back from the SMU.
 * EndMHz 0 (or absent) = end at Fmax - 600 MHz (resolved in main after
 * the Fmax read-back); an explicit value pins the floor instead. */
#define DEFAULT_START_MHZ   0
#define DEFAULT_END_MHZ     0
#define DEFAULT_STEP_MHZ    100
#define DEFAULT_SETTLE_MS   1500
#define DEFAULT_SAMPLES     5
#define DEFAULT_SAMPLE_INT_MS 200
#define DEFAULT_LOAD_CORE   0

/* Wide sanity clamps only; the real ceiling is the Fmax read back from
 * the SMU at startup (see main). */
#define FREQ_FLOOR 3000
#define FREQ_CEIL  6000

#define MSR_HW_PSTATE_STATUS 0xC0010293u
#define MSR_HWCR             0xC0010015u

typedef struct {
    int start_mhz;
    int end_mhz;
    int step_mhz;
    int settle_ms;
    int samples;
    int sample_int_ms;
    int load_core;
    char csv_path[MAX_PATH];
    char log_path[MAX_PATH];
    char ini_path[MAX_PATH];
    int ini_found;
} Config;

static FILE *g_log = NULL;
static FILE *g_csv = NULL;
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

static void load_config(Config *c)
{
    char cand[MAX_PATH];
    memset(c, 0, sizeof(*c));
    c->start_mhz = DEFAULT_START_MHZ;
    c->end_mhz   = DEFAULT_END_MHZ;
    c->step_mhz  = DEFAULT_STEP_MHZ;
    c->settle_ms = DEFAULT_SETTLE_MS;
    c->samples   = DEFAULT_SAMPLES;
    c->sample_int_ms = DEFAULT_SAMPLE_INT_MS;
    c->load_core = DEFAULT_LOAD_CORE;
    if (path_join(c->csv_path, sizeof(c->csv_path), g_exe_dir, "zenstates-vf.csv") == 0)
        strcpy(c->csv_path, "zenstates-vf.csv");
    if (path_join(c->log_path, sizeof(c->log_path), g_exe_dir, "zenstates-vf.log") == 0)
        strcpy(c->log_path, "zenstates-vf.log");

    if (path_join(cand, sizeof(cand), g_exe_dir, "zenstates-vf.ini") == 0)
        strcpy(cand, "zenstates-vf.ini");
    if (GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) {
        strcpy(c->ini_path, cand);
        c->ini_found = 1;
    } else {
        snprintf(c->ini_path, sizeof(c->ini_path), "%s\\zenstates-vf.ini", g_exe_dir);
        c->ini_found = 0;
    }
    if (!c->ini_found) {
        log_msg("WARN: no INI found at %s; using defaults "
                "start=%d end=%d step=%d settle=%dms samples=%d load_core=%d",
                c->ini_path, c->start_mhz, c->end_mhz, c->step_mhz,
                c->settle_ms, c->samples, c->load_core);
        return;
    }
    log_msg("INI: %s", c->ini_path);
    c->start_mhz = GetPrivateProfileIntA("Sweep", "StartMHz", DEFAULT_START_MHZ, c->ini_path);
    c->end_mhz   = GetPrivateProfileIntA("Sweep", "EndMHz", DEFAULT_END_MHZ, c->ini_path);
    c->step_mhz  = GetPrivateProfileIntA("Sweep", "StepMHz", DEFAULT_STEP_MHZ, c->ini_path);
    c->settle_ms = GetPrivateProfileIntA("Sweep", "SettleMs", DEFAULT_SETTLE_MS, c->ini_path);
    c->samples   = GetPrivateProfileIntA("Sweep", "Samples", DEFAULT_SAMPLES, c->ini_path);
    c->sample_int_ms = GetPrivateProfileIntA("Sweep", "SampleIntervalMs", DEFAULT_SAMPLE_INT_MS, c->ini_path);
    c->load_core = GetPrivateProfileIntA("Sweep", "LoadCore", DEFAULT_LOAD_CORE, c->ini_path);
    {
        char s[MAX_PATH];
        if (GetPrivateProfileStringA("Output", "CsvPath", "", s, sizeof(s), c->ini_path) > 0 && s[0])
            strcpy(c->csv_path, s);
        if (GetPrivateProfileStringA("Logging", "LogPath", "", s, sizeof(s), c->ini_path) > 0 && s[0])
            strcpy(c->log_path, s);
    }
    /* Wide sanity clamps only; the Fmax ceiling is applied in main after
     * the SMU read-back. StartMHz 0 means "start at Fmax" (resolved there). */
    if (c->start_mhz != 0)
        c->start_mhz = clamp_int(c->start_mhz, FREQ_FLOOR, FREQ_CEIL, "StartMHz");
    if (c->end_mhz != 0)
        c->end_mhz = clamp_int(c->end_mhz, FREQ_FLOOR, FREQ_CEIL, "EndMHz");
    c->step_mhz  = clamp_int(c->step_mhz, 25, 200, "StepMHz");
    c->settle_ms = clamp_int(c->settle_ms, 500, 10000, "SettleMs");
    c->samples   = clamp_int(c->samples, 1, 20, "Samples");
    c->sample_int_ms = clamp_int(c->sample_int_ms, 50, 2000, "SampleIntervalMs");
    {
        /* LoadCore range depends on the CPU (0..nproc-1), not on the 3600's
         * 12 logical cores. */
        SYSTEM_INFO si;
        DWORD nproc;
        GetSystemInfo(&si);
        nproc = si.dwNumberOfProcessors;
        if (nproc == 0) nproc = 1;
        c->load_core = clamp_int(c->load_core, 0, (int)nproc - 1, "LoadCore");
    }
    log_msg("config: start=%d end=%d step=%d settle=%dms samples=%d interval=%dms load_core=%d csv=%s",
            c->start_mhz, c->end_mhz, c->step_mhz, c->settle_ms,
            c->samples, c->sample_int_ms, c->load_core, c->csv_path);
}

/* ------------------------------------------------------------------ */
/* Preconditions: admin + Matisse (same as zenstates-c.c)              */
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
/* CPU detection + per-family SMU table (0.2.1, copied from            */
/* zenstates-c.c; the cap is HSMP 0x9 for every supported family, so   */
/* Zen 4 is sweepable now -- the socket cap constrains any loaded core)*/
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    uint32_t cmd_get_limit;  /* 0x6E: fused stock max (ceiling) */
    const char *note;
} FamilyTable;

static const FamilyTable FAM_MATISSE = {
    "Matisse / Castle Peak (Zen 2)",
    0x6E, NULL
};
static const FamilyTable FAM_ZEN3 = {
    "Vermeer / Chagall / Milan (Zen 3)",
    0x6E,
    "NOTE: HSMP on Zen 3 is UNVERIFIED (same mailbox/messages as Zen 2, "
    "but never run there). The interface-version gate decides at runtime."
};
static const FamilyTable FAM_ZEN4 = {
    "Raphael / Dragon Range / Genoa / Storm Peak (Zen 4)",
    0x6E,
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
 * reason) when the CPU is not supported by the sweep. */
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
    log_msg("CPU: vendor=%s family=0x%X model=0x%X (want AuthenticAMD family 0x17 model 0x71 Matisse)",
            vendor, family, model);
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
            /* HSMP 0x9 is a single socket-wide cap, so it constrains the
             * one loaded core exactly like it does on Zen 2/3. */
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
/* PawnIO driver access (copied from zenstates-c.c)                    */
/* ------------------------------------------------------------------ */

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
        log_msg("ERROR: IOCTL_PIO_LOAD_BINARY (0x%08X) failed for %s, GetLastError=%lu.",
                PAWNIO_IOCTL_LOAD, bin_path, (unsigned long)GetLastError());
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    log_msg("PawnIO: loaded module %s (%ld bytes)", bin_path, size);
    return h;
}

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
/* (same as zenstates-c.c / zenstates-hsmp.c)                          */
/* ------------------------------------------------------------------ */

#define HSMP_MSG  0x3B10534u
#define HSMP_RSP  0x3B10980u
#define HSMP_ARG  0x3B109E0u
#define HSMP_MAX_ARGS 8

#define HSMP_GET_IFACE_VERSION   0x3u
#define HSMP_SET_BOOST_LIMIT     0x8u
#define HSMP_SET_BOOST_LIMIT_ALL 0x9u
#define HSMP_GET_BOOST_LIMIT     0xAu

#define SMU_OK               0x01u
#define SMU_FAILED           0xFFu
#define SMU_UNKNOWN_CMD      0xFEu
#define SMU_REJECTED_PREREQ  0xFDu
#define SMU_REJECTED_BUSY    0xFCu

/* Raw SMU register access (RyzenSmu.cs: ioctl_read/write_smu_register). */
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

/* Send one HSMP message. Returns the SMU status byte (0x01 = OK), or -1
 * on transport failure. The caller holds the PCI lock. */
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

/* Read the current boost limit via cmd_get_limit (0x6E). Returns the
 * Win32 error from the send (0 on success); *mhz is set only on success.
 * Used for the startup Fmax read, the read-back after every cap set, and
 * the read-back after restore. */
static int smu_get_limit(uint32_t cmd, int *mhz)
{
    int64_t in[7] = { (int64_t)cmd, 0, 0, 0, 0, 0, 0 };
    int64_t out[6] = { 0 };
    int rc = pawn_execute(g_smu, "ioctl_send_smu_command", in, 7, out, 6);
    if (rc != 0) return rc;
    *mhz = (int)out[0];
    return 0;
}

/* MSR read on the CURRENT thread's core: the caller must pin its affinity
 * to the core it wants to measure first (see read_pstate_on_core). */
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
    return pawn_execute(g_msr, "ioctl_write_msr", in, 2, NULL, 0);
}

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
/* P-state read + spin load                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned int fid;
    unsigned int dfsid;
    unsigned int vid;
    double freq_mhz;   /* 2*Fid/DfsId * 100 MHz (BCLK assumed 100 MHz) */
    double volt_v;     /* 1.55 - Vid*0.00625 (SVI2) */
} PState;

/* Read HW_PSTATE_STATUS on logical core `core` (pins this thread there,
 * reads, restores affinity). Returns 0 on success. */
static int read_pstate_on_core(int core, PState *ps)
{
    DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1u << core);
    uint64_t v = 0;
    int rc;
    if (old == 0) return ERROR_INVALID_PARAMETER;
    rc = msr_read(MSR_HW_PSTATE_STATUS, &v);
    SetThreadAffinityMask(GetCurrentThread(), old);
    if (rc != 0) return rc;
    ps->fid   = (unsigned int)(v & 0xFFu);
    ps->dfsid = (unsigned int)((v >> 8) & 0x3Fu);
    ps->vid   = (unsigned int)((v >> 14) & 0xFFu);
    if (ps->dfsid == 0) return ERROR_INVALID_DATA;
    ps->freq_mhz = 2.0 * (double)ps->fid / (double)ps->dfsid * 100.0;
    ps->volt_v   = 1.55 - (double)ps->vid * 0.00625;
    return 0;
}

/* Initial APIC ID of logical core `core` (CPUID leaf 1, EBX[31:24]).
 * The calling thread is pinned there for the read. Returns -1 on failure. */
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

/* The true SMT sibling of logical core `load_core`: the logical processor
 * whose APIC ID differs from the load core's only in bit 0 (2-way SMT).
 * Returns -1 when there is none (SMT off) or the topology cannot be read.
 * Do NOT assume sibling == load_core ^ 1: logical-processor numbering is
 * not guaranteed to interleave siblings. */
static int find_smt_sibling(int load_core)
{
    SYSTEM_INFO si;
    DWORD n, i;
    int base;
    GetSystemInfo(&si);
    n = si.dwNumberOfProcessors;
    if (n == 0 || n > 64) n = 64;
    base = apic_id_of_core(load_core);
    if (base < 0) return -1;
    for (i = 0; i < n; i++) {
        int a;
        if ((int)i == load_core) continue;
        a = apic_id_of_core((int)i);
        if (a >= 0 && (a ^ base) == 1) return (int)i;
    }
    return -1;
}

static volatile LONG g_load_stop = 0;
static volatile unsigned long long g_load_counter = 0;

static DWORD WINAPI load_thread(LPVOID param)
{
    int core = (int)(intptr_t)param;
    DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1u << core);
    unsigned long long x = 0x9E3779B97F4A7C15ull;
    (void)old;
    /* Scalar integer spin: keeps this core at its Boost cap without AVX. */
    while (InterlockedCompareExchange(&g_load_stop, 0, 0) == 0) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        x ^= x >> 29;
        g_load_counter = x;
    }
    return 0;
}

static BOOL WINAPI hold_ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&g_load_stop, 1);
        return TRUE;
    }
    return FALSE;
}

/* --hold mode: pin a scalar spin-load to one core and print its frequency
 * and voltage every second until Ctrl+C. Does NOT set any cap -- it holds a
 * load against whatever cap is currently installed, so you can watch
 * HWMonitor and confirm the cap holds under sustained load. Returns 0 on
 * a clean stop (Ctrl+C), 1 on setup failure. */
static int run_hold(int core)
{
    HANDLE hload;
    int reader;
    int nproc = 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    nproc = (int)si.dwNumberOfProcessors;
    if (core < 0 || core >= nproc) {
        log_msg("ERROR: --hold: core %d out of range (0..%d).", core, nproc - 1);
        return 1;
    }
    /* Best-effort: report the HSMP cap currently installed (informational
     * only -- the hold works even if this read fails). */
    pci_lock();
    {
        uint32_t outarg[1] = { 0 };
        int st = hsmp_send(HSMP_GET_IFACE_VERSION, NULL, 0, outarg, 1);
        if (st == (int)SMU_OK && outarg[0] != 0) {
            int apic = apic_id_of_core(core);
            uint32_t inarg[1] = { (uint32_t)(apic < 0 ? 0 : apic) };
            st = hsmp_send(HSMP_GET_BOOST_LIMIT, inarg, 1, outarg, 1);
            if (st == (int)SMU_OK)
                log_msg("--hold: current HSMP socket cap = %u MHz", outarg[0]);
            else
                log_msg("--hold: HSMP cap read failed (status=0x%02X, %s).",
                        (unsigned)st, smu_status_name(st));
        } else {
            log_msg("--hold: HSMP not available; cap unknown.");
        }
    }
    pci_unlock();
    reader = find_smt_sibling(core);
    if (reader < 0) reader = core;
    log_msg("--hold: spinning a scalar load on logical core %d (reader on core %d). "
            "Press Ctrl+C to stop.", core, reader);
    g_load_stop = 0;
    hload = CreateThread(NULL, 0, load_thread, (LPVOID)(intptr_t)core, 0, NULL);
    if (!hload) {
        log_msg("ERROR: cannot start load thread (GetLastError=%lu)", (unsigned long)GetLastError());
        return 1;
    }
    SetConsoleCtrlHandler(hold_ctrl_handler, TRUE);
    for (;;) {
        PState ps;
        if (InterlockedCompareExchange(&g_load_stop, 0, 0) != 0) break;
        if (read_pstate_on_core(reader, &ps) == 0) {
            log_msg("hold: core %d %.0f MHz @ %.3f V (Fid=%u DfsId=%u Vid=%u)",
                    core, ps.freq_mhz, ps.volt_v, ps.fid, ps.dfsid, ps.vid);
        } else {
            log_msg("hold: MSR read failed on core %d", reader);
        }
        Sleep(1000);
    }
    InterlockedExchange(&g_load_stop, 1);
    WaitForSingleObject(hload, 5000);
    CloseHandle(hload);
    SetConsoleCtrlHandler(hold_ctrl_handler, FALSE);
    log_msg("--hold: stopped.");
    return 0;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median_of(double *vals, int n)
{
    qsort(vals, (size_t)n, sizeof(double), cmp_double);
    if (n % 2) return vals[n / 2];
    return (vals[n / 2 - 1] + vals[n / 2]) / 2.0;
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
    int step_count = 0, dir;
    HANDLE hload = NULL;
    const FamilyTable *fam = NULL;
    int stock_max = 0;
    int restore_mhz = 0;
    int reader_core = 0;
    int apic_load = -1;  /* APIC ID of the load core, for HSMP 0xA read-back */
    int hold_mode = 0;   /* --hold: hold a load on one core until Ctrl+C */
    int hold_core = -1;  /* core for --hold; -1 = use the INI LoadCore */
    {
        int i;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--hold") == 0) {
                hold_mode = 1;
                if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                    hold_core = atoi(argv[i + 1]);
                    i++;
                }
            }
        }
    }

    get_exe_dir();
    load_config(&cfg);
    g_log = fopen(cfg.log_path, "a");
    if (g_log) {
        log_msg("zenstates-vf starting, log=%s ini=%s (found=%d)",
                cfg.log_path, cfg.ini_path, cfg.ini_found);
    } else {
        printf("WARN: cannot open log file %s; logging to stdout only\n", cfg.log_path);
    }

    if (!is_admin()) {
        log_msg("ERROR: not running as Administrator. Right-click -> Run as administrator.");
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

    /* --hold mode: hold a load on one core until Ctrl+C. Branches here,
     * before the sweep baseline -- it needs no cap set, no restore, and
     * no sweep config. */
    if (hold_mode) {
        int rc = run_hold(hold_core < 0 ? cfg.load_core : hold_core);
        if (g_smu != INVALID_HANDLE_VALUE) CloseHandle(g_smu);
        if (g_msr != INVALID_HANDLE_VALUE) CloseHandle(g_msr);
        if (g_log) fclose(g_log);
        return rc;
    }

    /* Baseline, two reads that mean different things:
     *   - RSMU 0x6E: the FUSED stock max (Fmax). The ceiling for the sweep
     *     range (and StartMHz). Unaffected by any HSMP cap already set.
     *   - HSMP 0xA: the socket cap CURRENTLY set. The RESTORE value at the
     *     end (so a cap installed before the sweep, e.g. by zenstates-c's
     *     autostart, is put back exactly). Abort before sending anything
     *     if either read fails or is implausible, or if HSMP is not
     *     implemented on this CPU. */
    pci_lock();
    {
        int limit = 0;
        int rc = smu_get_limit(fam->cmd_get_limit, &limit);
        if (rc != 0 || limit < FREQ_FLOOR || limit > FREQ_CEIL) {
            log_msg("ERROR: stock boost limit read-back (0x%02X) failed or implausible "
                    "(win32=%d, value=%d MHz). Aborting before sending any cap.",
                    fam->cmd_get_limit, rc, limit);
            pci_unlock();
            if (g_smu != INVALID_HANDLE_VALUE) CloseHandle(g_smu);
            if (g_msr != INVALID_HANDLE_VALUE) CloseHandle(g_msr);
            if (g_log) fclose(g_log);
            return 1;
        }
        stock_max = limit;
        log_msg("stock boost limit (Fmax, 0x6E read-back) = %d MHz", stock_max);
        {
            uint32_t outarg[1] = { 0 };
            int st = hsmp_send(HSMP_GET_IFACE_VERSION, NULL, 0, outarg, 1);
            log_msg("HSMP GetInterfaceVersion (0x3): status=0x%02X (%s), version=%u",
                    (unsigned)st, smu_status_name(st), outarg[0]);
            if (st != (int)SMU_OK || outarg[0] == 0) {
                log_msg("ERROR: HSMP NOT SUPPORTED on this CPU/firmware; cannot set the cap. Aborting.");
                pci_unlock();
                if (g_smu != INVALID_HANDLE_VALUE) CloseHandle(g_smu);
                if (g_msr != INVALID_HANDLE_VALUE) CloseHandle(g_msr);
                if (g_log) fclose(g_log);
                return 1;
            }
        }
        {
            int apic = apic_id_of_core(cfg.load_core);
            uint32_t inarg[1];
            uint32_t outarg[1] = { 0 };
            int st;
            if (apic < 0) {
                log_msg("ERROR: could not read APIC ID of load core %d; aborting.", cfg.load_core);
                pci_unlock();
                if (g_smu != INVALID_HANDLE_VALUE) CloseHandle(g_smu);
                if (g_msr != INVALID_HANDLE_VALUE) CloseHandle(g_msr);
                if (g_log) fclose(g_log);
                return 1;
            }
            inarg[0] = (uint32_t)apic;
            st = hsmp_send(HSMP_GET_BOOST_LIMIT, inarg, 1, outarg, 1);
            if (st != (int)SMU_OK || (int)outarg[0] < FREQ_FLOOR || (int)outarg[0] > FREQ_CEIL) {
                log_msg("ERROR: pre-sweep HSMP limit read-back (0xA) failed or implausible "
                        "(status=0x%02X, value=%u MHz). Aborting before sending any cap.",
                        (unsigned)st, outarg[0]);
                pci_unlock();
                if (g_smu != INVALID_HANDLE_VALUE) CloseHandle(g_smu);
                if (g_msr != INVALID_HANDLE_VALUE) CloseHandle(g_msr);
                if (g_log) fclose(g_log);
                return 1;
            }
            restore_mhz = (int)outarg[0];
            log_msg("pre-sweep HSMP socket cap (0xA read-back) = %d MHz; "
                    "it will be restored at the end", restore_mhz);
        }
    }
    pci_unlock();

    /* Resolve the sweep range against Fmax. */
    if (cfg.start_mhz == 0) {
        cfg.start_mhz = stock_max;
        log_msg("sweep start: StartMHz not set, using Fmax %d MHz", stock_max);
    } else if (cfg.start_mhz > stock_max) {
        log_msg("WARN: StartMHz=%d above stock max %d; clamping to %d",
                cfg.start_mhz, stock_max, stock_max);
        cfg.start_mhz = stock_max;
    }
    if (cfg.end_mhz == 0) {
        cfg.end_mhz = stock_max - 600;
        if (cfg.end_mhz < FREQ_FLOOR) cfg.end_mhz = FREQ_FLOOR;
        log_msg("sweep end: EndMHz not set, using Fmax-600 = %d MHz", cfg.end_mhz);
    }
    if (cfg.end_mhz > stock_max) {
        log_msg("WARN: EndMHz=%d above stock max %d; clamping to %d",
                cfg.end_mhz, stock_max, stock_max);
        cfg.end_mhz = stock_max;
    }
    log_msg("sweep: start=%d end=%d step=%d", cfg.start_mhz, cfg.end_mhz, cfg.step_mhz);

    /* Reader core: the true SMT sibling of the load core (APIC ID), not
     * the assumed LoadCore^1. Falls back to the load core itself. */
    reader_core = find_smt_sibling(cfg.load_core);
    if (reader_core >= 0) {
        log_msg("reader core: %d (SMT sibling of load core %d, via APIC ID)",
                reader_core, cfg.load_core);
    } else {
        reader_core = cfg.load_core;
        log_msg("reader core: %d (no SMT sibling found; reading the load core itself)",
                reader_core);
    }

    /* APIC ID of the load core, for the HSMP 0xA read-back after every cap
     * set (the socket cap is the same for every core, so any APIC ID
     * would do; the baseline already proved this one reads). */
    apic_load = apic_id_of_core(cfg.load_core);
    if (apic_load < 0) {
        log_msg("ERROR: could not read APIC ID of load core %d; aborting.", cfg.load_core);
        if (g_log) fclose(g_log);
        return 1;
    }

    g_csv = fopen(cfg.csv_path, "w");
    if (!g_csv) {
        log_msg("ERROR: cannot open CSV output %s", cfg.csv_path);
        if (g_log) fclose(g_log);
        return 1;
    }
    fprintf(g_csv, "TargetMHz,Sample,Fid,DfsId,Vid,FreqMHz,VoltV,LoadFreqMHz,LoadVoltV\n");
    fflush(g_csv);

    /* Spin the load for the whole sweep so the measured core stays boosted. */
    g_load_stop = 0;
    hload = CreateThread(NULL, 0, load_thread, (LPVOID)(intptr_t)cfg.load_core, 0, NULL);
    if (!hload) {
        log_msg("ERROR: cannot start load thread (GetLastError=%lu)", (unsigned long)GetLastError());
        fclose(g_csv);
        if (g_log) fclose(g_log);
        return 1;
    }
    log_msg("load thread spinning on logical core %d (reader on core %d)",
            cfg.load_core, reader_core);

    dir = (cfg.end_mhz >= cfg.start_mhz) ? 1 : -1;
    {
        int target = cfg.start_mhz;
        for (;;) {
            double freqs[20], volts[20], load_freqs[20], load_volts[20];
            int ok_samples = 0, ok_load = 0, i;
            PState ps;

            pci_lock();
            {
                uint32_t inarg[1] = { (uint32_t)target };
                int st = hsmp_send(HSMP_SET_BOOST_LIMIT_ALL, inarg, 1, NULL, 0);
                if (st != (int)SMU_OK) {
                    log_msg("ERROR: HSMP SetBoostLimitSocket (0x9, arg %d MHz) failed, status=0x%02X (%s)",
                            target, (unsigned)st, smu_status_name(st));
                    failures++;
                }
                inarg[0] = (uint32_t)apic_load;
                {
                    uint32_t outarg[1] = { 0 };
                    st = hsmp_send(HSMP_GET_BOOST_LIMIT, inarg, 1, outarg, 1);
                    if (st == (int)SMU_OK)
                        log_msg("read-back after set to %d MHz: HSMP limit=%u MHz%s",
                                target, outarg[0], (int)outarg[0] == target ? "" : " (SET DID NOT TAKE EFFECT)");
                    else
                        log_msg("read-back after set to %d MHz failed (status=0x%02X, %s)",
                                target, (unsigned)st, smu_status_name(st));
                    if (st != (int)SMU_OK || (int)outarg[0] != target) failures++;
                }
            }
            pci_unlock();
            if (failures > 0 && target == cfg.start_mhz) {
                /* First step already failed to set the cap; still try to
                 * sample (the reading is then just the current state),
                 * but do not pretend the sweep is valid. */
                log_msg("WARN: cap set failed at %d MHz; samples below are NOT at a known cap", target);
            }
            Sleep((DWORD)cfg.settle_ms);

            for (i = 0; i < cfg.samples; i++) {
                if (read_pstate_on_core(reader_core, &ps) == 0) {
                    PState pls;
                    double lf = 0.0, lv = 0.0;
                    freqs[ok_samples] = ps.freq_mhz;
                    volts[ok_samples] = ps.volt_v;
                    if (reader_core != cfg.load_core &&
                        read_pstate_on_core(cfg.load_core, &pls) == 0) {
                        lf = pls.freq_mhz; lv = pls.volt_v;
                        load_freqs[ok_load] = lf; load_volts[ok_load] = lv; ok_load++;
                    } else if (reader_core == cfg.load_core) {
                        lf = ps.freq_mhz; lv = ps.volt_v;
                        load_freqs[ok_load] = lf; load_volts[ok_load] = lv; ok_load++;
                    }
                    fprintf(g_csv, "%d,%d,%u,%u,%u,%.1f,%.4f,%.1f,%.4f\n",
                            target, i + 1, ps.fid, ps.dfsid, ps.vid,
                            ps.freq_mhz, ps.volt_v, lf, lv);
                    ok_samples++;
                } else {
                    log_msg("WARN: MSR read failed at %d MHz sample %d", target, i + 1);
                }
                if (i + 1 < cfg.samples) Sleep((DWORD)cfg.sample_int_ms);
            }
            fflush(g_csv);
            if (ok_samples > 0) {
                double med_f = median_of(freqs, ok_samples);
                double med_v = median_of(volts, ok_samples);
                if (ok_load > 0) {
                    double med_lf = median_of(load_freqs, ok_load);
                    double med_lv = median_of(load_volts, ok_load);
                    log_msg("point: target=%d MHz -> reader(core %d) median %.0f MHz @ %.3f V; load core(%d) median %.0f MHz @ %.3f V (%d/%d samples)",
                            target, reader_core, med_f, med_v, cfg.load_core, med_lf, med_lv, ok_samples, cfg.samples);
                } else {
                    log_msg("point: target=%d MHz -> median %.0f MHz @ %.3f V (%d/%d samples)",
                            target, med_f, med_v, ok_samples, cfg.samples);
                }
            } else {
                log_msg("point: target=%d MHz -> NO valid samples", target);
                failures++;
            }
            step_count++;
            if (target == cfg.end_mhz) break;
            target += dir * cfg.step_mhz;
            if ((dir > 0 && target > cfg.end_mhz) || (dir < 0 && target < cfg.end_mhz))
                target = cfg.end_mhz;
        }
    }

    InterlockedExchange(&g_load_stop, 1);
    WaitForSingleObject(hload, 5000);
    CloseHandle(hload);
    fclose(g_csv);

    /* Restore: the HSMP socket cap back to the value 0xA reported before
     * the sweep (the pre-sweep limit). If a cap was installed before the
     * sweep, this puts it back -- the sweep is non-destructive. */
    pci_lock();
    {
        uint32_t inarg[1] = { (uint32_t)restore_mhz };
        uint32_t outarg[1] = { 0 };
        int st = hsmp_send(HSMP_SET_BOOST_LIMIT_ALL, inarg, 1, NULL, 0);
        if (st != (int)SMU_OK) {
            log_msg("ERROR: HSMP restore set (0x9, arg %d MHz) failed, status=0x%02X (%s)",
                    restore_mhz, (unsigned)st, smu_status_name(st));
            failures++;
        }
        inarg[0] = (uint32_t)apic_load;
        st = hsmp_send(HSMP_GET_BOOST_LIMIT, inarg, 1, outarg, 1);
        if (st == (int)SMU_OK)
            log_msg("read-back after restore: HSMP limit=%u MHz (want %d MHz)%s",
                    outarg[0], restore_mhz, (int)outarg[0] == restore_mhz ? "" : " -- MISMATCH, reboot to restore stock");
        else
            log_msg("read-back after restore failed (status=0x%02X, %s); reboot to restore stock",
                    (unsigned)st, smu_status_name(st));
        if (st != (int)SMU_OK || (int)outarg[0] != restore_mhz) failures++;
    }
    pci_unlock();

    if (failures == 0) {
        log_msg("OK: sweep done (%d points), CSV=%s. "
                "Medians are the measured V/F points. "
                "Caps restored to %d MHz (the pre-sweep limit).",
                step_count, cfg.csv_path, restore_mhz);
    } else {
        log_msg("DONE WITH %d FAILURE(S) (see above). CSV=%s still holds the "
                "samples that succeeded; caps were restored to %d MHz as far as "
                "the SMU accepted it. A reboot restores stock behaviour.",
                failures, cfg.csv_path, restore_mhz);
    }
    if (g_smu != INVALID_HANDLE_VALUE) CloseHandle(g_smu);
    if (g_msr != INVALID_HANDLE_VALUE) CloseHandle(g_msr);
    if (g_log) fclose(g_log);
    return failures == 0 ? 0 : 3;
}
