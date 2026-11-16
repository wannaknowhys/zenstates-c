/* SPDX-License-Identifier: GPL-3.0-or-later
 * zenstates-c-gui.c -- pure C Win32 GUI for editing zenstates-c.ini, 0.2.5.
 *
 * Traditional HWND UI only: no MFC, no .NET, no third-party UI library.
 * A standard Tab Control (SysTabControl32) sits at the top as a strip.
 * ALL controls are created directly as children of the MAIN window (not
 * as children of per-tab page windows) and are shown/hidden by tab --
 * this is deliberate: an earlier version used one child window per tab
 * page ("ZSC_Page") positioned over the tab's display area with
 * TabCtrl_AdjustRect, and on the user's machine the page windows never
 * appeared, leaving every tab blank. The page-window layer was the bug;
 * direct children have no AdjustRect, no Z-order and no class-registration
 * failure mode, so there is nothing to go blank.
 *   [CPU / Boost]  SingleCoreMaxMHz, AllCoreMaxMHz (+ note: cap, not fixed)
 *                    + "Read current from CPU" button (runs zenstates-c.exe
 *                    --read elevated, then refreshes the fields from the INI)
 *   [Power]        PPT_W, TDC_A, EDC_A
 *   [Settings]     AutoApply checkbox, LogPath, Install/Uninstall autostart
 * Bottom row (main window): Apply Now | Save | Close.
 *
 * Power keys are OPTIONAL and are shown EMPTY when unset: an empty field
 * means "left as the BIOS set it" (the key is absent or empty in the INI).
 * Saving with a Power field empty DELETES that key from the INI; typing a
 * number sets it. This keeps "unset" visibly distinct from a real value --
 * the old behaviour showed the 88/60/90 defaults even when the key was
 * absent, which looked like a real setting and was not one.
 *
 * Division of labour (deliberate, see README.md):
 *   - This GUI ONLY reads/writes the INI. It never opens the PawnIO
 *     device and never sends an SMU command, so it runs unelevated
 *     (asInvoker manifest).
 *   - "Apply Now" saves the INI, then launches zenstates-c.exe elevated
 *     (ShellExecute "runas") and reports its exit code.
 *   - "Install/Uninstall autostart" run schtasks.exe elevated ("runas").
 *   Tradeoff: each privileged action pops one UAC prompt, and if the INI
 *   lives in a protected folder (e.g. Program Files) saving also needs an
 *   elevated GUI -- so keep the install in a normal writable folder,
 *   where an unelevated GUI can write the INI.
 *
 * Build (Linux cross-compile, verified):
 *   x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -mwindows -o zenstates-c-gui.exe zenstates-c-gui.c zenstates-c-gui-res.o -lcomctl32
 * Build (Windows, MinGW-w64): same command. With MSVC cl: see build.bat.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Layout scale. GUI_SCALE=1 is the normal (moderately widened) GUI.
 * Compile with -DGUI_SCALE=2 for the XL variant: every coordinate, size
 * and the font are doubled. The XL build is a separate exe
 * (zenstates-c-gui-xl.exe) so the user can compare the two layouts. */
#ifndef GUI_SCALE
#define GUI_SCALE 1
#endif
#define SX(v) ((v) * GUI_SCALE)
#define SY(v) ((v) * GUI_SCALE)

#define IDC_TAB        100
#define IDC_APPLY_NOW  200
#define IDC_SAVE       201
#define IDC_CLOSE      202
#define IDC_INSTALL    203
#define IDC_UNINSTALL  204
#define IDC_STATUS     205
#define IDC_READ       206

#define ID_ED_SINGLE   300
#define ID_UP_SINGLE   301
#define ID_ED_ALL      302
#define ID_UP_ALL      303
#define ID_ED_PPT      310
#define ID_UP_PPT      311
#define ID_ED_TDC      312
#define ID_UP_TDC      313
#define ID_ED_EDC      314
#define ID_UP_EDC      315
#define ID_CHK_AUTO    320
#define ID_ED_LOG      321

static HINSTANCE g_hInst;
static HWND g_hTab, g_hStatus;
static HFONT g_font;
/* Controls, by page -- every control is a direct child of the main window
 * (see the header comment: no per-tab page windows). show_page() shows the
 * controls of the selected tab and hides the other two tabs' controls. */
static HWND g_edSingle, g_edAll, g_edPpt, g_edTdc, g_edEdc, g_edLog;
static HWND g_chkAuto;
static HWND g_tabCtrls[3][16];
static int  g_tabCtrlCount[3];
static void tab_add(int tab, HWND h)
{
    if (tab >= 0 && tab < 3 && h && g_tabCtrlCount[tab] < 16)
        g_tabCtrls[tab][g_tabCtrlCount[tab]++] = h;
}
static char g_exeDir[MAX_PATH];
static char g_iniPath[MAX_PATH];
static int  g_iniFound = 0;
/* Power keys are optional in 0.2.0: whether they were present when loaded,
 * and the values shown then (a Save must not re-add keys the user never set). */
static int  g_pptPresent, g_tdcPresent, g_edcPresent;
static int  g_pptLoaded, g_tdcLoaded, g_edcLoaded;

static void set_font(HWND h) { if (g_font) SendMessage(h, WM_SETFONT, (WPARAM)g_font, TRUE); }

/* Create the GUI font, scaled by GUI_SCALE. At scale 1 this is the stock
 * default GUI font (unchanged look). At scale 2 the default font's height
 * is doubled, so text stays proportional to the doubled layout. */
static HFONT make_scaled_font(void)
{
    HFONT base;
    LOGFONTA lf;
    if (GUI_SCALE <= 1) return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    base = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!GetObjectA(base, sizeof(lf), &lf)) return base;
    lf.lfHeight *= GUI_SCALE;
    lf.lfWidth *= GUI_SCALE;
    return CreateFontIndirectA(&lf);
}

static HWND make_label(HWND parent, int x, int y, int w, const char *text)
{
    HWND h = CreateWindowExA(0, "STATIC", text,
                             WS_CHILD | WS_VISIBLE | SS_LEFT,
                             SX(x), SY(y), SX(w), SY(16), parent, NULL, g_hInst, NULL);
    set_font(h);
    return h;
}

static HWND make_edit(HWND parent, int id, int x, int y, int w)
{
    HWND h = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                             SX(x), SY(y), SX(w), SY(22), parent, (HMENU)(INT_PTR)id, g_hInst, NULL);
    set_font(h);
    return h;
}

/* Edit + up-down spinner buddy. Returns the edit handle. Both the edit
 * and the spinner are registered to `tab`, so show_page() hides/shows
 * them together -- a spinner left out of the tab list would stay visible
 * on the wrong tab. */
static HWND make_number(int tab, HWND parent, int idEdit, int idUp, int x, int y,
                        int lo, int hi, int value)
{
    HWND ed = make_edit(parent, idEdit, x, y, 70);
    HWND up = CreateUpDownControl(WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                  UDS_SETBUDDYINT | UDS_ALIGNRIGHT |
                                  UDS_ARROWKEYS | UDS_HOTTRACK,
                                  SX(x + 70), SY(y), SX(18), SY(22), parent, idUp, g_hInst,
                                  ed, hi, lo, value);
    set_font(up);
    tab_add(tab, ed);
    tab_add(tab, up);
    return ed;
}

static void set_num(HWND ed, int v)
{
    char b[16];
    snprintf(b, sizeof(b), "%d", v);
    SetWindowTextA(ed, b);
}
/* Set a number, or leave the field EMPTY when the key is unset. An empty
 * field is how the GUI shows "not set" (see the header comment). */
static void set_num_or_empty(HWND ed, int present, int v)
{
    if (present) set_num(ed, v);
    else SetWindowTextA(ed, "");
}
static int edit_is_empty(HWND ed)
{
    return GetWindowTextLengthA(ed) == 0;
}
static int get_num(HWND ed)
{
    BOOL ok = FALSE;
    UINT v = GetDlgItemInt(GetParent(ed), GetDlgCtrlID(ed), &ok, FALSE);
    return ok ? (int)v : 0;
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

/* ------------------------------------------------------------------ */
/* INI                                                                 */
/* ------------------------------------------------------------------ */

static void resolve_ini(void)
{
    char cand[MAX_PATH], cand2[MAX_PATH];
    if (path_join(cand, sizeof(cand), g_exeDir, "zenstates-c.ini") &&
        GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) {
        strcpy(g_iniPath, cand);
        g_iniFound = 1;
        return;
    }
    if (GetEnvironmentVariableA("ProgramData", cand, sizeof(cand)) > 0 &&
        path_join(cand2, sizeof(cand2), cand, "ZenStates-C") &&
        path_join(cand, sizeof(cand), cand2, "zenstates-c.ini") &&
        GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) {
        strcpy(g_iniPath, cand);
        g_iniFound = 1;
        return;
    }
    if (!path_join(g_iniPath, sizeof(g_iniPath), g_exeDir, "zenstates-c.ini"))
        strcpy(g_iniPath, "zenstates-c.ini");
    g_iniFound = 0;
}

static void load_ini_to_controls(void)
{
    set_num(g_edSingle, GetPrivateProfileIntA("Boost", "SingleCoreMaxMHz", 4000, g_iniPath));
    set_num(g_edAll,    GetPrivateProfileIntA("Boost", "AllCoreMaxMHz",    3900, g_iniPath));
    {
        char s[32];
        g_pptPresent = GetPrivateProfileStringA("Power", "PPT_W", "", s, sizeof(s), g_iniPath) > 0 && s[0];
        g_tdcPresent = GetPrivateProfileStringA("Power", "TDC_A", "", s, sizeof(s), g_iniPath) > 0 && s[0];
        g_edcPresent = GetPrivateProfileStringA("Power", "EDC_A", "", s, sizeof(s), g_iniPath) > 0 && s[0];
    }
    g_pptLoaded = GetPrivateProfileIntA("Power", "PPT_W", 88, g_iniPath);
    g_tdcLoaded = GetPrivateProfileIntA("Power", "TDC_A", 60, g_iniPath);
    g_edcLoaded = GetPrivateProfileIntA("Power", "EDC_A", 90, g_iniPath);
    /* Power fields show EMPTY when unset (not the 88/60/90 defaults) --
     * an empty field is "left as the BIOS set it", visibly distinct from
     * a real value. */
    set_num_or_empty(g_edPpt, g_pptPresent, g_pptLoaded);
    set_num_or_empty(g_edTdc, g_tdcPresent, g_tdcLoaded);
    set_num_or_empty(g_edEdc, g_edcPresent, g_edcLoaded);
    CheckDlgButton(GetParent(g_chkAuto), ID_CHK_AUTO,
                   GetPrivateProfileIntA("Startup", "AutoApply", 1, g_iniPath) ? BST_CHECKED : BST_UNCHECKED);
    {
        char lp[MAX_PATH] = "";
        GetPrivateProfileStringA("Logging", "LogPath", "", lp, sizeof(lp), g_iniPath);
        SetWindowTextA(g_edLog, lp);
    }
}

/* Returns 1 on success. On failure the file is probably in a protected
 * folder and the GUI needs elevation -- say so plainly. */
static int save_ini_from_controls(void)
{
    char b[16], lp[MAX_PATH];
    snprintf(b, sizeof(b), "%d", get_num(g_edSingle));
    WritePrivateProfileStringA("Boost", "SingleCoreMaxMHz", b, g_iniPath);
    snprintf(b, sizeof(b), "%d", get_num(g_edAll));
    WritePrivateProfileStringA("Boost", "AllCoreMaxMHz", b, g_iniPath);
    /* Power keys are optional in 0.2.0: an EMPTY field means "unset" --
     * delete the key so the INI keeps the unset state (and the GUI shows
     * it empty next time). A number means "set" -- write it, but only if
     * it was present or the user changed the shown value; otherwise a GUI
     * Save would silently re-enable power limiting with the shown defaults. */
    if (edit_is_empty(g_edPpt)) {
        WritePrivateProfileStringA("Power", "PPT_W", NULL, g_iniPath);
        g_pptPresent = 0;
    } else if (g_pptPresent || get_num(g_edPpt) != g_pptLoaded) {
        snprintf(b, sizeof(b), "%d", get_num(g_edPpt));
        WritePrivateProfileStringA("Power", "PPT_W", b, g_iniPath);
        g_pptPresent = 1; g_pptLoaded = get_num(g_edPpt);
    }
    if (edit_is_empty(g_edTdc)) {
        WritePrivateProfileStringA("Power", "TDC_A", NULL, g_iniPath);
        g_tdcPresent = 0;
    } else if (g_tdcPresent || get_num(g_edTdc) != g_tdcLoaded) {
        snprintf(b, sizeof(b), "%d", get_num(g_edTdc));
        WritePrivateProfileStringA("Power", "TDC_A", b, g_iniPath);
        g_tdcPresent = 1; g_tdcLoaded = get_num(g_edTdc);
    }
    if (edit_is_empty(g_edEdc)) {
        WritePrivateProfileStringA("Power", "EDC_A", NULL, g_iniPath);
        g_edcPresent = 0;
    } else if (g_edcPresent || get_num(g_edEdc) != g_edcLoaded) {
        snprintf(b, sizeof(b), "%d", get_num(g_edEdc));
        WritePrivateProfileStringA("Power", "EDC_A", b, g_iniPath);
        g_edcPresent = 1; g_edcLoaded = get_num(g_edEdc);
    }
    WritePrivateProfileStringA("Startup", "AutoApply",
        SendMessageA(g_chkAuto, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0",
        g_iniPath);
    GetWindowTextA(g_edLog, lp, sizeof(lp));
    WritePrivateProfileStringA("Logging", "LogPath", lp, g_iniPath);
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(NULL,
            "Could not write the INI file. It is probably in a protected folder.\n"
            "Run this GUI as administrator once, or move the install to a writable\n"
            "folder (not Program Files).",
            "ZenStates-C GUI", MB_ICONERROR | MB_OK);
        return 0;
    }
    g_iniFound = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Elevated helpers: run a program via "runas", wait, return exit code */
/* ------------------------------------------------------------------ */

static int run_elevated(const char *exe, const char *params, DWORD *exit_code)
{
    SHELLEXECUTEINFOA sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = NULL;
    sei.lpVerb = "runas";
    sei.lpFile = exe;
    sei.lpParameters = params;
    sei.nShow = SW_SHOW;
    if (!ShellExecuteExA(&sei)) {
        DWORD e = GetLastError();
        if (e == ERROR_CANCELLED)
            MessageBoxA(NULL, "UAC prompt was cancelled.", "ZenStates-C GUI", MB_ICONWARNING | MB_OK);
        else {
            char b[256];
            snprintf(b, sizeof(b), "Failed to start %.200s (error %lu).", exe, (unsigned long)e);
            MessageBoxA(NULL, b, "ZenStates-C GUI", MB_ICONERROR | MB_OK);
        }
        return 0;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    GetExitCodeProcess(sei.hProcess, exit_code);
    CloseHandle(sei.hProcess);
    return 1;
}

static void status(const char *text)
{
    SetWindowTextA(g_hStatus, text);
}

/* ------------------------------------------------------------------ */
/* Controls (direct children of the main window, grouped by tab)       */
/* ------------------------------------------------------------------ */

/* Create every control once, as a direct child of the main window.
 * Positions are fixed, below the tab strip (which is at y=8..~38).
 * show_page() shows one tab's controls and hides the other two.
 * All coordinates are design units, scaled by GUI_SCALE in the helpers.
 *
 * Layout (0.2.4, moderately widened): the window is 560x400, labels are
 * wide enough that their text is never clipped (the 0.2.3 layout clipped
 * "Single-core boost cap (MHz):" and "Log path (empty = exe dir):" because
 * the labels were narrower than the text and the edit boxes started too
 * far left), and rows have more vertical room. */
static void create_controls(HWND hwnd)
{
    HWND h;
    /* Page 0: CPU / Boost */
    tab_add(0, make_label(hwnd, 12, 48, 520, "Boost caps -- these are upper limits, NOT a fixed frequency."));
    tab_add(0, make_label(hwnd, 12, 70, 520, "Below the cap, Boost still scales and idle still downclocks."));
    tab_add(0, make_label(hwnd, 12, 92, 520, "HSMP socket cap: effective cap = lower of the two."));
    tab_add(0, make_label(hwnd, 12, 120, 210, "Single-core boost cap (MHz):"));
    g_edSingle = make_number(0, hwnd, ID_ED_SINGLE, ID_UP_SINGLE, 230, 116, 3000, 6000, 4000);
    tab_add(0, make_label(hwnd, 12, 154, 210, "All-core boost cap (MHz):"));
    g_edAll = make_number(0, hwnd, ID_ED_ALL, ID_UP_ALL, 230, 150, 3000, 6000, 3900);
    tab_add(0, make_label(hwnd, 12, 194, 520, "Stock max is read from the CPU when applied (0x6E);"));
    tab_add(0, make_label(hwnd, 12, 212, 520, "values above it are clamped. See README.md."));
    h = CreateWindowExA(0, "BUTTON", "Read current from CPU",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        SX(12), SY(240), SX(220), SY(28), hwnd, (HMENU)(INT_PTR)IDC_READ, g_hInst, NULL);
    set_font(h);
    tab_add(0, h);
    /* Page 1: Power */
    tab_add(1, make_label(hwnd, 12, 48, 520, "Power limits -- optional. Unset keys are left as the BIOS set them."));
    tab_add(1, make_label(hwnd, 12, 120, 150, "PPT (W):"));
    g_edPpt = make_number(1, hwnd, ID_ED_PPT, ID_UP_PPT, 180, 116, 45, 300, 88);
    tab_add(1, make_label(hwnd, 12, 154, 150, "TDC (A):"));
    g_edTdc = make_number(1, hwnd, ID_ED_TDC, ID_UP_TDC, 180, 150, 30, 200, 60);
    tab_add(1, make_label(hwnd, 12, 188, 150, "EDC (A):"));
    g_edEdc = make_number(1, hwnd, ID_ED_EDC, ID_UP_EDC, 180, 184, 45, 300, 90);
    tab_add(1, make_label(hwnd, 12, 228, 520, "Empty = unset (left as the BIOS set it). Type a value to pin it;"));
    tab_add(1, make_label(hwnd, 12, 246, 520, "Ryzen 5 3600 stock is 88 / 60 / 90. Or leave this page alone."));
    /* Page 2: Settings */
    g_chkAuto = CreateWindowExA(0, "BUTTON", "Apply at boot (install scheduled task)",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                SX(12), SY(48), SX(360), SY(20), hwnd, (HMENU)(INT_PTR)ID_CHK_AUTO, g_hInst, NULL);
    set_font(g_chkAuto);
    tab_add(2, g_chkAuto);
    tab_add(2, make_label(hwnd, 12, 84, 220, "Log path (empty = exe dir):"));
    g_edLog = make_edit(hwnd, ID_ED_LOG, 12, 104, 520);
    tab_add(2, g_edLog);
    h = CreateWindowExA(0, "BUTTON", "Install autostart",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        SX(12), SY(140), SX(160), SY(28), hwnd, (HMENU)(INT_PTR)IDC_INSTALL, g_hInst, NULL);
    set_font(h);
    tab_add(2, h);
    h = CreateWindowExA(0, "BUTTON", "Uninstall autostart",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        SX(182), SY(140), SX(160), SY(28), hwnd, (HMENU)(INT_PTR)IDC_UNINSTALL, g_hInst, NULL);
    set_font(h);
    tab_add(2, h);
    tab_add(2, make_label(hwnd, 12, 182, 520, "Autostart = Task Scheduler, ONSTART, as SYSTEM, highest"));
    tab_add(2, make_label(hwnd, 12, 200, 520, "privileges, 30 s delay. No UAC prompt at boot."));
}

static void show_page(int index)
{
    int i, t;
    for (t = 0; t < 3; t++)
        for (i = 0; i < g_tabCtrlCount[t]; i++)
            ShowWindow(g_tabCtrls[t][i], t == index ? SW_SHOW : SW_HIDE);
    TabCtrl_SetCurSel(g_hTab, index);
}

/* Position the tab strip (full width, fixed height) and the bottom row.
 * The tab controls are at fixed positions (created once in
 * create_controls), so they need no repositioning on resize. */
static void layout_controls(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    SetWindowPos(g_hTab, NULL, SX(8), SY(8), rc.right - SX(16), SY(30), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, IDC_APPLY_NOW), NULL, SX(8), rc.bottom - SY(58), SX(110), SY(26), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, IDC_SAVE), NULL, SX(126), rc.bottom - SY(58), SX(80), SY(26), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, IDC_CLOSE), NULL, rc.right - SX(88), rc.bottom - SY(58), SX(80), SY(26), SWP_NOZORDER);
    SetWindowPos(g_hStatus, NULL, SX(8), rc.bottom - SY(28), rc.right - SX(16), SY(18), SWP_NOZORDER);
}

/* ------------------------------------------------------------------ */
/* Main window                                                         */
/* ------------------------------------------------------------------ */

static void do_apply_now(HWND hwnd)
{
    char exe[MAX_PATH], msg[512];
    DWORD code = 0;
    if (!save_ini_from_controls()) return;
    if (!path_join(exe, sizeof(exe), g_exeDir, "zenstates-c.exe"))
        strcpy(exe, "zenstates-c.exe");
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(hwnd, "zenstates-c.exe was not found next to this GUI.",
                    "ZenStates-C GUI", MB_ICONERROR | MB_OK);
        return;
    }
    status("Applying... (UAC prompt expected)");
    if (!run_elevated(exe, NULL, &code)) { status("Apply cancelled/failed."); return; }
    snprintf(msg, sizeof(msg),
             "zenstates-c.exe finished with exit code %lu.\n%s\nSee the log for details.",
             (unsigned long)code,
             code == 0 ? "Boost cap applied." :
             code == 2 ? "Not admin, or wrong CPU." :
             code == 3 ? "One or more SMU commands failed." :
                         "Driver / module / config error.");
    MessageBoxA(hwnd, msg, "ZenStates-C GUI", code == 0 ? MB_ICONINFORMATION : MB_ICONWARNING | MB_OK);
    status(code == 0 ? "Applied OK." : "Apply finished with errors -- see log.");
}

static void do_read_current(HWND hwnd)
{
    char exe[MAX_PATH];
    DWORD code = 0;
    if (!path_join(exe, sizeof(exe), g_exeDir, "zenstates-c.exe"))
        strcpy(exe, "zenstates-c.exe");
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(hwnd, "zenstates-c.exe was not found next to this GUI.",
                    "ZenStates-C GUI", MB_ICONERROR | MB_OK);
        return;
    }
    /* Do NOT save the GUI's current values first: Read replaces them with
     * the CPU's real values, so saving first would overwrite the INI with
     * the (possibly default) shown values before the read runs. */
    status("Reading current config from CPU... (UAC prompt expected)");
    if (!run_elevated(exe, "--read", &code)) { status("Read cancelled/failed."); return; }
    if (code == 0) {
        /* Refresh: re-read the INI the elevated --read just wrote, so the
         * fields show the CPU's current config. Without this the GUI would
         * keep showing the old (possibly default) values. */
        load_ini_to_controls();
        status("Read OK -- fields now show the CPU's current config.");
    } else {
        char b[256];
        snprintf(b, sizeof(b), "Read failed (exit code %lu). See the log.",
                 (unsigned long)code);
        MessageBoxA(hwnd, b, "ZenStates-C GUI", MB_ICONWARNING | MB_OK);
        status("Read failed -- see log.");
    }
}

static void do_autostart(HWND hwnd, int install)
{
    char exe[MAX_PATH], params[1024];
    DWORD code = 0;
    if (!path_join(exe, sizeof(exe), g_exeDir, "zenstates-c.exe"))
        strcpy(exe, "zenstates-c.exe");
    if (install) {
        /* ONSTART as SYSTEM, highest privileges, 30 s delay so the
         * PawnIO driver is up. /F overwrites an existing task. */
        snprintf(params, sizeof(params),
                 "/create /TN \"ZenStates-C\" /TR \"\\\"%s\\\"\" "
                 "/SC ONSTART /RU SYSTEM /RL HIGHEST /DELAY 0000:30 /F",
                 exe);
        status("Installing autostart... (UAC prompt expected)");
        if (!run_elevated("schtasks.exe", params, &code)) { status("Install cancelled/failed."); return; }
        if (code == 0) {
            MessageBoxA(hwnd, "Autostart installed: ZenStates-C runs at boot as SYSTEM.",
                        "ZenStates-C GUI", MB_ICONINFORMATION | MB_OK);
            status("Autostart installed.");
        } else {
            char b[256];
            snprintf(b, sizeof(b), "schtasks failed with exit code %lu.", (unsigned long)code);
            MessageBoxA(hwnd, b, "ZenStates-C GUI", MB_ICONERROR | MB_OK);
            status("Autostart install failed.");
        }
    } else {
        snprintf(params, sizeof(params), "/delete /TN \"ZenStates-C\" /F");
        status("Uninstalling autostart... (UAC prompt expected)");
        if (!run_elevated("schtasks.exe", params, &code)) { status("Uninstall cancelled/failed."); return; }
        MessageBoxA(hwnd, code == 0 ? "Autostart removed." : "schtasks reported an error (the task may not have existed).",
                    "ZenStates-C GUI", code == 0 ? MB_ICONINFORMATION : MB_ICONWARNING | MB_OK);
        status(code == 0 ? "Autostart removed." : "Autostart uninstall reported an error.");
    }
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        TCITEMA ti;
        int i;
        static const char *titles[3] = { "CPU / Boost", "Power", "Settings" };
        INITCOMMONCONTROLSEX ice = { sizeof(ice), ICC_TAB_CLASSES | ICC_UPDOWN_CLASS };
        InitCommonControlsEx(&ice);
        g_font = make_scaled_font();

        g_hTab = CreateWindowExA(0, WC_TABCONTROLA, "",
                                 WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_TAB, g_hInst, NULL);
        set_font(g_hTab);
        memset(&ti, 0, sizeof(ti));
        ti.mask = TCIF_TEXT;
        for (i = 0; i < 3; i++) {
            ti.pszText = (char *)titles[i];
            TabCtrl_InsertItem(g_hTab, i, &ti);
        }
        /* All tab controls are direct children of this window (see the
         * header comment) -- no per-tab page windows. */
        create_controls(hwnd);
        {
            HWND b;
            b = CreateWindowExA(0, "BUTTON", "Apply Now",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_APPLY_NOW, g_hInst, NULL);
            set_font(b);
            b = CreateWindowExA(0, "BUTTON", "Save",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SAVE, g_hInst, NULL);
            set_font(b);
            b = CreateWindowExA(0, "BUTTON", "Close",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CLOSE, g_hInst, NULL);
            set_font(b);
        }
        g_hStatus = CreateWindowExA(0, "STATIC", "",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_STATUS, g_hInst, NULL);
        set_font(g_hStatus);

        resolve_ini();
        load_ini_to_controls();
        show_page(0);
        layout_controls(hwnd);
        {
            char sb[600];
            snprintf(sb, sizeof(sb), "INI: %s%s", g_iniPath,
                     g_iniFound ? "" : " (not found -- defaults shown, Save will create it)");
            status(sb);
        }
        return 0;
    }
    case WM_SIZE:
        layout_controls(hwnd);
        return 0;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE)
            show_page(TabCtrl_GetCurSel(g_hTab));
        else if (nm->code == UDN_DELTAPOS) {
            /* Frequency spinners (MHz) step by 25 per click, not 1: the
             * up-down control proposes iDelta = +/-1; scale it to +/-25
             * and let the control apply the modified delta (range clamping
             * still applies). Power spinners (W/A) keep step 1. */
            NMUPDOWN *up = (NMUPDOWN *)lp;
            if (nm->idFrom == ID_UP_SINGLE || nm->idFrom == ID_UP_ALL)
                up->iDelta *= 25;
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_APPLY_NOW) do_apply_now(hwnd);
        else if (id == IDC_READ) do_read_current(hwnd);
        else if (id == IDC_SAVE) {
            if (save_ini_from_controls()) status("Saved.");
        }
        else if (id == IDC_CLOSE) DestroyWindow(hwnd);
        else if (id == IDC_INSTALL) do_autostart(hwnd, 1);
        else if (id == IDC_UNINSTALL) do_autostart(hwnd, 0);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;
    char path[MAX_PATH], *slash;
    (void)hPrev; (void)lpCmdLine;

    g_hInst = hInstance;
    GetModuleFileNameA(NULL, path, sizeof(path));
    slash = strrchr(path, '\\');
    if (slash) {
        size_t n = (size_t)(slash - path);
        if (n >= sizeof(g_exeDir)) n = sizeof(g_exeDir) - 1;
        memcpy(g_exeDir, path, n);
        g_exeDir[n] = '\0';
    } else strcpy(g_exeDir, ".");

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "ZenStatesC_GUI";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    hwnd = CreateWindowExA(0, "ZenStatesC_GUI", "ZenStates-C -- Boost cap settings",
                           WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, SX(560), SY(400),
                           NULL, NULL, hInstance, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
