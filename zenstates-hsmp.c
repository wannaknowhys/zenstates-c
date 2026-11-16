/* SPDX-License-Identifier: GPL-3.0-or-later
 * zenstates-hsmp.c -- HSMP boost-limit probe for Ryzen desktop CPUs.
 *
 * Why this tool exists: the RSMU boost-limit SET commands (0x29/0x2B,
 * restored from old ZenStates 0.x) are rejected by SMU firmware 46.73.0
 * on a Ryzen 5 3600 (win32=1359, and a 0x6E read-back proves the limit
 * never moved). But Ryzen Master CAN set an all-core boost cap on Zen 2,
 * so a working path must exist. The path is a THIRD mailbox this project
 * never tried: HSMP (Host System Management Port).
 *
 * Evidence chain (see SLOW_MODE.md):
 *   - ZenStates.Core 2.0, Hardware/Smu/Mailboxes/HSMPMailbox.cs:
 *       WriteBoostLimit          = 0x8  (per-core)
 *       WriteBoostLimitAllCores  = 0x9  (socket-wide = all-core cap)
 *       ReadBoostLimit           = 0xA
 *     and Zen2Settings.cs configures the HSMP mailbox for Matisse:
 *       MSG 0x3B10534, RSP 0x3B10980, ARG 0x3B109E0
 *     (same addresses for Zen 3 and Zen 4; Zen 5 differs: MSG 0x3B10934,
 *     so Zen 5 is refused here).
 *   - Linux kernel UAPI header arch/x86/include/uapi/asm/amd_hsmp.h
 *     (amd_hsmp.h, confirmed across several independent mirrors):
 *       HSMP_SET_BOOST_LIMIT_SOCKET (09h) "Set socket maximum frequency
 *         level": num_args = 1, input args[0] = boost limit value in MHz.
 *       HSMP_GET_BOOST_LIMIT (0Ah) "Get current frequency limit":
 *         num_args = 1, input args[0] = apic id,
 *         output args[0] = boost limit value in MHz.
 *       HSMP_SET_BOOST_LIMIT (08h) per-core: args[0] =
 *         apic id[31:16] + boost limit in MHz[15:0].
 *   - Transport: ZenStates.Core's SMU.SendSmuCommand is a GENERIC mailbox
 *     driver (Hardware/SMU.cs). It does the wait-done / clear-RSP /
 *     write-args / write-MSG / wait-done / read-status protocol over raw
 *     PCI register reads/writes (SmuReadRegNoLock / SmuWriteRegNoLock ->
 *     PawnIO RyzenSmu.cs ioctl_read_smu_register /
 *     ioctl_write_smu_register). The same protocol drives RSMU, MP1 and
 *     HSMP; only the mailbox addresses differ. This tool re-implements
 *     that generic protocol in C over the same two PawnIO functions, so
 *     it can reach HSMP, which the RyzenSMU.bin module's built-in
 *     ioctl_send_smu_command cannot (that function hardcodes RSMU).
 *     Status codes (SMU.cs): OK = 0x01, FAILED = 0xFF,
 *     UNKNOWN_CMD = 0xFE, CMD_REJECTED_PREREQ = 0xFD,
 *     CMD_REJECTED_BUSY = 0xFC.
 *   - Caveat: HSMP is documented in AMD's PPR for Family 19h (server).
 *     Whether a desktop Matisse (Family 17h) implements it is EXACTLY
 *     what this probe tests: step 1 is the interface-version query
 *     (msg 0x3). If HSMP is not implemented, that query fails cleanly
 *     and the tool stops without setting anything.
 *
 * What it does:
 *   zenstates-hsmp.exe [target_mhz]      (default 4000)
 *   1. HSMP GetInterfaceVersion (0x3) -- confirms HSMP exists.
 *   2. HSMP GetBoostLimit (0xA, arg = APIC ID of core 0) -- the socket cap
 *      CURRENTLY set (may already be capped).
 *   3. RSMU GetBoostLimitFrequency (0x6E) -- the FUSED stock max. This is
 *      the true stock max (and the value the restore hint prints), NOT
 *      the HSMP reading: 0x6E is unaffected by the HSMP cap.
 *   4. HSMP SetBoostLimitSocket (0x9, arg = target MHz).
 *   5. HSMP GetBoostLimit (0xA) -- read-back: did it take effect?
 *   6. RSMU 0x6E -- read-back: still the fused max (expected).
 *   The cap is LEFT SET so the user can verify in HWMonitor that the
 *   max frequency no longer exceeds the target. To restore stock, run:
 *       zenstates-hsmp.exe 4200
 *   (or whatever the stock max was; the log prints it).
 *
 * Safety: admin + family checks, HWCR fix (same as zenstates-c), the
 * target is clamped to [3000, stock_max]. It never writes a fixed
 * frequency, voltage, or VID. A reboot restores stock behaviour.
 *
 * Build (Linux cross-compile, verified):
 *   x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o zenstates-hsmp.exe zenstates-hsmp.c zenstates-hsmp-res.o
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

#define FREQ_FLOOR 3000
#define FREQ_CEIL  6000
#define DEFAULT_TARGET_MHZ 4000

/* HSMP mailbox (Zen 2/3/4; ZenStates-Core Zen2Settings/Zen3Settings/Zen4Settings) */
#define HSMP_MSG  0x3B10534u
#define HSMP_RSP  0x3B10980u
#define HSMP_ARG  0x3B109E0u
#define HSMP_MAX_ARGS 8

/* HSMP messages (amd_hsmp.h / HSMPMailbox.cs) */
#define HSMP_GET_IFACE_VERSION   0x3u
#define HSMP_SET_BOOST_LIMIT     0x8u
#define HSMP_SET_BOOST_LIMIT_ALL 0x9u
#define HSMP_GET_BOOST_LIMIT     0xAu

/* SMU status codes (SMU.cs) */
#define SMU_OK               0x01u
#define SMU_FAILED           0xFFu
#define SMU_UNKNOWN_CMD      0xFEu
#define SMU_REJECTED_PREREQ  0xFDu
#define SMU_REJECTED_BUSY    0xFCu

/* RSMU read-back (cross-check) */
#define RSMU_GET_LIMIT 0x6Eu

#define MSR_HWCR 0xC0010015u

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
/* PawnIO                                                               */
/* ------------------------------------------------------------------ */

#define PAWNIO_DEVICE_TYPE   (41394u << 16)
#define PAWNIO_IOCTL_LOAD    (PAWNIO_DEVICE_TYPE | (0x821u << 2))
#define PAWNIO_IOCTL_EXECUTE (PAWNIO_DEVICE_TYPE | (0x841u << 2))
#define PAWNIO_FN_NAME_LEN   32

static HANDLE g_smu = INVALID_HANDLE_VALUE;  /* RyzenSMU.bin */
static HANDLE g_msr = INVALID_HANDLE_VALUE;  /* AMDFamily17.bin */

static HANDLE pawn_open(void)
{
    HANDLE h = CreateFileA("\\\\?\\GLOBALROOT\\Device\\PawnIO",
                           GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        log_msg("ERROR: cannot open PawnIO device (GetLastError=%lu). "
                "Is PawnIO installed and running? (https://pawnio.eu/)",
                (unsigned long)GetLastError());
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
        log_msg("ERROR: IOCTL_PIO_LOAD_BINARY failed for %s, GetLastError=%lu. "
                "PawnIO version mismatch is the usual cause.",
                bin_path, (unsigned long)GetLastError());
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

static bool find_module(const char *name, char *out, size_t out_size)
{
    char moddir[MAX_PATH];
    if (path_join(moddir, sizeof(moddir), g_exe_dir, "modules") &&
        path_join(out, out_size, moddir, name) &&
        GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return true;
    if (path_join(out, out_size, g_exe_dir, name) &&
        GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return true;
    return false;
}

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

/* MSR via AMDFamily17 module (for the HWCR fix, same as zenstates-c). */
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
        DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(), ((DWORD_PTR)1) << i);
        uint64_t v = 0;
        int rc;
        if (old == 0) {
            log_msg("WARN: HWCR: cannot pin to core %lu", (unsigned long)i);
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
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
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
/* Generic mailbox protocol (SMU.cs SendSmuCommand, re-implemented)    */
/* ------------------------------------------------------------------ */

/* Wait until the mailbox response register is non-zero (previous command
 * done). Mirrors SmuWaitDoneNoLock: SMU_TIMEOUT 8192 iterations and a
 * 500 ms hang guard. Returns 1 when ready, 0 on timeout/PCI failure. */
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

/* RSMU read-back (0x6E) via the module's built-in RSMU command path. */
static int rsmu_get_limit(uint32_t *limit)
{
    int64_t in[7] = { (int64_t)RSMU_GET_LIMIT, 0, 0, 0, 0, 0, 0 };
    int64_t out[6] = { 0 };
    int rc = pawn_execute(g_smu, "ioctl_send_smu_command", in, 7, out, 6);
    if (rc != 0) return rc;
    *limit = (uint32_t)(out[0] & 0xFFFFFFFFu);
    return 0;
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

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    char smu_bin[MAX_PATH], msr_bin[MAX_PATH];
    char log_path[MAX_PATH];
    uint32_t smu_ver = 0;
    int target = DEFAULT_TARGET_MHZ;
    int stock_max = 0;
    int apic0 = -1;
    int rc = 0;

    get_exe_dir();
    if (!path_join(log_path, sizeof(log_path), g_exe_dir, "zenstates-hsmp.log"))
        strcpy(log_path, "zenstates-hsmp.log");
    g_log = fopen(log_path, "a");
    if (g_log)
        log_msg("zenstates-hsmp starting, log=%s", log_path);
    else
        printf("WARN: cannot open log file %s; logging to stdout only\n", log_path);

    if (argc >= 2) {
        target = atoi(argv[1]);
        if (target < FREQ_FLOOR || target > FREQ_CEIL) {
            log_msg("ERROR: target %d MHz out of range [%d, %d]", target, FREQ_FLOOR, FREQ_CEIL);
            if (g_log) fclose(g_log);
            return 2;
        }
    }
    log_msg("HSMP boost-limit probe: target = %d MHz (pass 4200 to restore stock; "
            "the cap is LEFT SET so you can check HWMonitor)", target);

    if (!is_admin()) {
        log_msg("ERROR: not running as Administrator. Right-click -> Run as administrator.");
        if (g_log) fclose(g_log);
        return 2;
    }

    /* CPU check: HSMP mailbox addresses are the same for Zen 2/3/4
     * (0x3B10534); Zen 5 uses a different MSG address (0x3B10934). */
    {
        unsigned int eax, ebx, ecx, edx;
        unsigned int family, model;
        if (!__get_cpuid(0, &eax, &ebx, &ecx, &edx) || ebx != 0x68747541u /* "Auth" */) {
            log_msg("ERROR: not an AMD CPU; refusing.");
            if (g_log) fclose(g_log);
            return 2;
        }
        if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
            log_msg("ERROR: CPUID leaf 1 failed; refusing.");
            if (g_log) fclose(g_log);
            return 2;
        }
        family = ((eax >> 8) & 0xFu) + ((eax >> 20) & 0xFFu);
        model = ((eax >> 4) & 0xFu) | (((eax >> 16) & 0xFu) << 4);
        log_msg("CPU: vendor=AuthenticAMD family=0x%X model=0x%X", family, model);
        if (family == 0x1Au) {
            log_msg("ERROR: Zen 5 uses a different HSMP MSG address (0x3B10934); "
                    "this probe only covers Zen 2/3/4 (0x3B10534). Refusing.");
            if (g_log) fclose(g_log);
            return 2;
        }
        if (family != 0x17u && family != 0x19u) {
            log_msg("ERROR: family 0x%X is not Zen 2/3/4; refusing.", family);
            if (g_log) fclose(g_log);
            return 2;
        }
    }

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

    {
        int64_t out[1] = { 0 };
        rc = pawn_execute(g_smu, "ioctl_get_smu_version", NULL, 0, out, 1);
        if (rc != 0 || out[0] == 0) {
            log_msg("ERROR: ioctl_get_smu_version failed or returned 0; aborting.");
            if (g_log) fclose(g_log);
            return 1;
        }
        smu_ver = (uint32_t)(out[0] & 0xFFFFFFFFu);
    }
    log_msg("SMU version: 0x%08X", smu_ver);

    hwcr_fix();

    apic0 = apic_id_of_core(0);
    log_msg("APIC ID of logical core 0 = %d (used as arg for HSMP GetBoostLimit)", apic0);
    if (apic0 < 0) {
        log_msg("ERROR: could not read APIC ID of core 0; aborting.");
        if (g_log) fclose(g_log);
        return 1;
    }

    pci_lock();

    /* 1. HSMP interface version: does this CPU/firmware implement HSMP? */
    {
        uint32_t out[1] = { 0 };
        int st = hsmp_send(HSMP_GET_IFACE_VERSION, NULL, 0, out, 1);
        log_msg("HSMP GetInterfaceVersion (0x3): status=0x%02X (%s), version=%u",
                (unsigned)st, smu_status_name(st), out[0]);
        if (st != (int)SMU_OK || out[0] == 0) {
            log_msg("HSMP NOT SUPPORTED on this CPU/firmware (interface version "
                    "query failed). Stopping without setting anything.");
            pci_unlock();
            if (g_log) fclose(g_log);
            return 2;
        }
    }

    /* 2/3. Current limit, both ways (baseline).
     *   - HSMP 0xA: the socket cap CURRENTLY set (may already be capped).
     *   - RSMU 0x6E: the FUSED stock max. This is the true stock max and
     *     the value the restore hint prints -- NOT the HSMP reading,
     *     which is wrong when a cap is already active (the run-2 hint
     *     bug: it printed the capped value as "stock"). */
    {
        uint32_t inarg[1] = { (uint32_t)apic0 };
        uint32_t out[1] = { 0 };
        uint32_t rsmu = 0;
        int st = hsmp_send(HSMP_GET_BOOST_LIMIT, inarg, 1, out, 1);
        log_msg("HSMP GetBoostLimit (0xA, apic %d): status=0x%02X (%s), limit=%u MHz",
                apic0, (unsigned)st, smu_status_name(st), out[0]);
        if (rsmu_get_limit(&rsmu) == 0)
            log_msg("RSMU GetBoostLimitFrequency (0x6E): limit=%u MHz (fused stock max)", rsmu);
        else
            log_msg("RSMU GetBoostLimitFrequency (0x6E): read failed");
        stock_max = (int)rsmu;
        if (stock_max < FREQ_FLOOR || stock_max > FREQ_CEIL) {
            log_msg("ERROR: fused stock max implausible (%d MHz); aborting before setting anything.",
                    stock_max);
            pci_unlock();
            if (g_log) fclose(g_log);
            return 1;
        }
        log_msg("stock boost limit (fused max) = %d MHz; current HSMP cap = %u MHz",
                stock_max, out[0]);
    }

    if (target > stock_max) {
        log_msg("WARN: target %d MHz above stock max %d MHz; clamping to %d",
                target, stock_max, stock_max);
        target = stock_max;
    }

    /* 4. Set the socket-wide boost limit via HSMP. */
    {
        uint32_t inarg[1] = { (uint32_t)target };
        int st = hsmp_send(HSMP_SET_BOOST_LIMIT_ALL, inarg, 1, NULL, 0);
        log_msg("HSMP SetBoostLimitSocket (0x9, arg %d MHz): status=0x%02X (%s)",
                target, (unsigned)st, smu_status_name(st));
        if (st != (int)SMU_OK) {
            log_msg("HSMP set FAILED; nothing was applied. The RSMU 0x29/0x2B "
                    "route and the HSMP 0x9 route are both rejected on this firmware.");
            pci_unlock();
            if (g_log) fclose(g_log);
            return 3;
        }
    }

    /* 5/6. Read-back, both ways. */
    {
        uint32_t inarg[1] = { (uint32_t)apic0 };
        uint32_t out[1] = { 0 };
        uint32_t rsmu = 0;
        int st = hsmp_send(HSMP_GET_BOOST_LIMIT, inarg, 1, out, 1);
        log_msg("HSMP GetBoostLimit read-back (0xA): status=0x%02X (%s), limit=%u MHz%s",
                (unsigned)st, smu_status_name(st), out[0],
                (st == (int)SMU_OK && (int)out[0] == target) ? " (SET TOOK EFFECT)" : " (SET DID NOT TAKE EFFECT)");
        if (rsmu_get_limit(&rsmu) == 0)
            log_msg("RSMU GetBoostLimitFrequency read-back (0x6E): limit=%u MHz%s",
                    rsmu, ((int)rsmu == target) ? " (agrees with HSMP)" : " (does NOT agree with HSMP)");
        else
            log_msg("RSMU GetBoostLimitFrequency read-back (0x6E): read failed");

        pci_unlock();
        if (st == (int)SMU_OK && (int)out[0] == target) {
            log_msg("RESULT: HSMP cap TOOK EFFECT. Limit is now %d MHz and is LEFT SET. "
                    "Check HWMonitor: max frequency should not exceed ~%d MHz under load. "
                    "To restore stock, run: zenstates-hsmp.exe %d",
                    target, target, stock_max);
            if (g_log) fclose(g_log);
            return 0;
        }
        log_msg("RESULT: HSMP set reported OK but the read-back does not match the "
                "target (%d MHz). Treat the cap as NOT applied.", target);
        if (g_log) fclose(g_log);
        return 3;
    }
}
