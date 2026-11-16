/* SPDX-License-Identifier: GPL-3.0-or-later
 * zenstates-vf.c -- empirical V/F sweep for Ryzen desktop CPUs (Zen 2 / Zen 3), 0.2.0.
 *
 * What it measures (and what it does NOT):
 *   There is NO SMU command that dumps the fused V/F table: ZenStates-Core
 *   has no such command, and TransferTableToDram (RSMU 0x05) exports the
 *   PowerTable telemetry (live frequency / voltage / power), not a curve.
 *   The V/F curve lives inside SMU firmware as fused FIT data.
 *   So this tool measures the curve instead of reading it: for each target
 *   frequency it caps Boost to that frequency (the same cap messages
 *   zenstates-c.c uses), pins a scalar spin-load to one core so that core
 *   actually boosts to the cap, and reads the VID that SMU requests at that
 *   point. One CSV row per sample; the median per target is the V/F point.
 *
 * Scope: Matisse / Castle Peak and Vermeer / Chagall / Milan only -- the
 *   sweep needs BOTH the single-core and the all-core cap, and Zen 4 has
 *   no single-core cap command in the sourced table, so Zen 4 is refused
 *   (the all-core cap alone cannot constrain a single-core load). Zen 1 /
 *   Zen+ / Rome / APUs / Zen 5 have no cap command at all and are refused
 *   too. See zenstates-c.c for the family table and sources.
 *
 * Fmax (no hardcoded 4200 any more): at startup the tool reads 0x6E
 *   GetBoostLimitFrequency -- the same call upstream's Cpu.GetFMax() uses.
 *   That value is the sweep's start (unless the INI sets StartMHz) and the
 *   restore value at the end. The sweep is therefore non-destructive: if a
 *   cap was already installed (e.g. by zenstates-c's autostart), the sweep
 *   restores exactly that cap at the end. If the read fails or returns an
 *   implausible value the tool aborts before sending any cap.
 *
 * Read path (all from ZenStates-Core source, same as the main program):
 *   MSR 0xC0010293 HW_PSTATE_STATUS, read on the measured core via PawnIO
 *   AMDFamily17 ioctl_read_msr. The calling thread is pinned to the core
 *   first (the C equivalent of Cpu.ReadMsrTx / GroupAffinity).
 *   EAX bits: [7:0] CurCpuFid, [13:8] CurCpuDfsId, [21:14] CurCpuVid.
 *     multiplier = 2.0 * Fid / DfsId            (Cpu.GetCoreMulti)
 *     frequency  = multiplier * 100 MHz          (Cpu.GetCoreClockMhz, BCLK=100)
 *     voltage    = 1.55 - Vid * 0.00625 V        (Utils.VidToVoltage, SVI2)
 *   The spin-load runs on logical core LoadCore, the reader pins itself to
 *   the SMT sibling (LoadCore^1) of the same physical core, so both observe
 *   the same core clock / VID. The load is a plain scalar integer loop
 *   (no AVX): the curve measured is the scalar-boost curve.
 *
 * What it writes: ONLY the two Boost-limit SMU commands (per the family
 *   table in zenstates-c.c) and, at the end, a restore of both caps to the
 *   value 0x6E reported before the sweep (the pre-sweep limit, not a
 *   constant). It never writes an MSR, never sets a fixed frequency,
 *   never sets a fixed voltage or VID. A reboot also restores stock.
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

/* StartMHz 0 (or absent) = start at the Fmax read back from the SMU. */
#define DEFAULT_START_MHZ   0
#define DEFAULT_END_MHZ     3600
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
    c->end_mhz   = clamp_int(c->end_mhz, FREQ_FLOOR, FREQ_CEIL, "EndMHz");
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
/* CPU detection + per-family SMU table (0.2.0, copied from            */
/* zenstates-c.c; the sweep needs BOTH cap commands, so Zen 4 -- which */
/* has only the all-core one -- is refused here, not just noted)       */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    uint32_t cmd_set_single;
    uint32_t cmd_set_all;
    uint32_t cmd_get_limit;
    const char *note;
} FamilyTable;

static const FamilyTable FAM_MATISSE = {
    "Matisse / Castle Peak (Zen 2)",
    0x29, 0x2B, 0x6E, NULL
};
static const FamilyTable FAM_ZEN3 = {
    "Vermeer / Chagall / Milan (Zen 3)",
    0x29, 0x2B, 0x6E,
    "NOTE: Zen 3 inherits the Zen 2 RSMU table, so 0x29/0x2B are attempted "
    "but UNVERIFIED on Zen 3. Failures are reported; a failed command applies nothing."
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
            /* The sweep needs a single-core cap to constrain the one loaded
             * core; Zen 4 only has the all-core code (0x70), which cannot
             * constrain a single-core load. Refusing is the honest answer. */
            log_msg("ERROR: refusing to run: %s has no single-core boost-limit "
                    "command in the sourced table (only all-core 0x70), and the "
                    "sweep needs one to constrain its single-core load. The main "
                    "program (zenstates-c.exe) does support Zen 4, all-core only.",
                    name ? name : "this CPU (Zen 4)");
            return NULL;
        default:
            log_msg("ERROR: refusing to run: %s has no boost-limit command in "
                    "the sourced ZenStates table, so there is no safe cap to send.",
                    name ? name : "this CPU");
            return NULL;
        }
    }
    log_msg("ERROR: refusing to run: family 0x%X is not a supported Zen generation "
            "(Zen 2 / Zen 3 desktop).", family);
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

int main(void)
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

    /* Baseline: the 0x6E read-back BEFORE the sweep. This is the stock
     * boost limit (Fmax) -- the sweep's start (unless the INI sets
     * StartMHz) and the restore value at the end. Abort before sending
     * anything if the read fails or is implausible. */
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
        restore_mhz = stock_max;
        log_msg("stock boost limit (Fmax, 0x6E read-back) = %d MHz; "
                "caps will be restored to this value at the end", stock_max);
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
    if (cfg.end_mhz > stock_max) {
        log_msg("WARN: EndMHz=%d above stock max %d; clamping to %d",
                cfg.end_mhz, stock_max, stock_max);
        cfg.end_mhz = stock_max;
    }
    log_msg("sweep: start=%d end=%d step=%d", cfg.start_mhz, cfg.end_mhz, cfg.step_mhz);

    g_csv = fopen(cfg.csv_path, "w");
    if (!g_csv) {
        log_msg("ERROR: cannot open CSV output %s", cfg.csv_path);
        if (g_log) fclose(g_log);
        return 1;
    }
    fprintf(g_csv, "TargetMHz,Sample,Fid,DfsId,Vid,FreqMHz,VoltV\n");
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
    log_msg("load thread spinning on logical core %d (reader uses sibling core %d)",
            cfg.load_core, cfg.load_core ^ 1);

    dir = (cfg.end_mhz >= cfg.start_mhz) ? 1 : -1;
    {
        int target = cfg.start_mhz;
        for (;;) {
            double freqs[20], volts[20];
            int ok_samples = 0, i;
            PState ps;

            pci_lock();
            if (smu_send(fam->cmd_set_all, (uint32_t)target) != 0) failures++;
            if (smu_send(fam->cmd_set_single, (uint32_t)target) != 0) failures++;
            pci_unlock();
            if (failures > 0 && ok_samples == 0 && target == cfg.start_mhz) {
                /* First step already failed to set the cap; still try to
                 * sample (the reading is then just the current state),
                 * but do not pretend the sweep is valid. */
                log_msg("WARN: cap set failed at %d MHz; samples below are NOT at a known cap", target);
            }
            Sleep((DWORD)cfg.settle_ms);

            for (i = 0; i < cfg.samples; i++) {
                if (read_pstate_on_core(cfg.load_core ^ 1, &ps) == 0) {
                    freqs[ok_samples] = ps.freq_mhz;
                    volts[ok_samples] = ps.volt_v;
                    fprintf(g_csv, "%d,%d,%u,%u,%u,%.1f,%.4f\n",
                            target, i + 1, ps.fid, ps.dfsid, ps.vid,
                            ps.freq_mhz, ps.volt_v);
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
                log_msg("point: target=%d MHz -> median %.0f MHz @ %.3f V (%d/%d samples)",
                        target, med_f, med_v, ok_samples, cfg.samples);
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

    /* Restore: both caps back to the value 0x6E reported before the sweep
     * (the pre-sweep limit). If a cap was installed before the sweep, this
     * puts it back -- the sweep is non-destructive. */
    pci_lock();
    if (smu_send(fam->cmd_set_all, (uint32_t)restore_mhz) != 0) failures++;
    if (smu_send(fam->cmd_set_single, (uint32_t)restore_mhz) != 0) failures++;
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
