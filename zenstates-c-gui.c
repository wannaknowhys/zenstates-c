/* SPDX-License-Identifier: GPL-3.0-or-later
 * zenstates-c-gui.c -- pure C Win32 GUI for editing zenstates-c.ini.
 *
 * Traditional HWND UI only: no MFC, no .NET, no third-party UI library.
 * Standard Tab Control (SysTabControl32) with three pages, each page a
 * WS_CHILD window whose controls are created in its WM_CREATE:
 *   [CPU / Boost]  SingleCoreMaxMHz, AllCoreMaxMHz (+ note: cap, not fixed)
 *   [Power]        PPT_W, TDC_A, EDC_A
 *   [Settings]     AutoApply checkbox, LogPath, Install/Uninstall autostart
 * Bottom row (main window): Apply Now | Save | Close.
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

#define IDC_TAB        100
#define IDC_APPLY_NOW  200
#define IDC_SAVE       201
#define IDC_CLOSE      202
#define IDC_INSTALL    203
#define IDC_UNINSTALL  204
#define IDC_STATUS     205

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
static HWND g_pages[3];
static HFONT g_font;
/* Controls, by page */
static HWND g_edSingle, g_edAll, g_edPpt, g_edTdc, g_edEdc, g_edLog;
static HWND g_chkAuto;
static char g_exeDir[MAX_PATH];
static char g_iniPath[MAX_PATH];
static int  g_iniFound = 0;

static void set_font(HWND h) { if (g_font) SendMessage(h, WM_SETFONT, (WPARAM)g_font, TRUE); }

static HWND make_label(HWND parent, int x, int y, int w, const char *text)
{
    HWND h = CreateWindowExA(0, "STATIC", text,
                             WS_CHILD | WS_VISIBLE | SS_LEFT,
                             x, y, w, 16, parent, NULL, g_hInst, NULL);
    set_font(h);
    return h;
}

static HWND make_edit(HWND parent, int id, int x, int y, int w)
{
    HWND h = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                             x, y, w, 22, parent, (HMENU)(INT_PTR)id, g_hInst, NULL);
    set_font(h);
    return h;
}

/* Edit + up-down spinner buddy. Returns the edit handle. */
static HWND make_number(HWND parent, int idEdit, int idUp, int x, int y,
                        int lo, int hi, int value)
{
    HWND ed = make_edit(parent, idEdit, x, y, 70);
    HWND up = CreateUpDownControl(WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                  UDS_SETBUDDYINT | UDS_ALIGNRIGHT |
                                  UDS_ARROWKEYS | UDS_HOTTRACK,
                                  x + 70, y, 18, 22, parent, idUp, g_hInst,
                                  ed, hi, lo, value);
    set_font(up);
    return ed;
}

static void set_num(HWND ed, int v)
{
    char b[16];
    snprintf(b, sizeof(b), "%d", v);
    SetWindowTextA(ed, b);
}
static int get_num(HWND ed)
{
    BOOL ok = FALSE;
    UINT v = GetDlgItemInt(GetParent(ed), GetDlgCtrlID(ed), &ok, FALSE);
    return ok ? (int)v : 0;
}

/* ------------------------------------------------------------------ */
/* INI                                                                 */
/* ------------------------------------------------------------------ */

static void resolve_ini(void)
{
    char cand[MAX_PATH];
    snprintf(cand, sizeof(cand), "%s\\zenstates-c.ini", g_exeDir);
    if (GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) {
        strcpy(g_iniPath, cand);
        g_iniFound = 1;
        return;
    }
    if (GetEnvironmentVariableA("ProgramData", cand, sizeof(cand)) > 0) {
        char cand2[MAX_PATH];
        snprintf(cand2, sizeof(cand2), "%s\\ZenStates-C\\zenstates-c.ini", cand);
        if (GetFileAttributesA(cand2) != INVALID_FILE_ATTRIBUTES) {
            strcpy(g_iniPath, cand2);
            g_iniFound = 1;
            return;
        }
    }
    snprintf(g_iniPath, sizeof(g_iniPath), "%s\\zenstates-c.ini", g_exeDir);
    g_iniFound = 0;
}

static void load_ini_to_controls(void)
{
    set_num(g_edSingle, GetPrivateProfileIntA("Boost", "SingleCoreMaxMHz", 4000, g_iniPath));
    set_num(g_edAll,    GetPrivateProfileIntA("Boost", "AllCoreMaxMHz",    3900, g_iniPath));
    set_num(g_edPpt,    GetPrivateProfileIntA("Power", "PPT_W",            88,   g_iniPath));
    set_num(g_edTdc,    GetPrivateProfileIntA("Power", "TDC_A",            60,   g_iniPath));
    set_num(g_edEdc,    GetPrivateProfileIntA("Power", "EDC_A",            90,   g_iniPath));
    CheckDlgButton(g_pages[2], ID_CHK_AUTO,
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
    snprintf(b, sizeof(b), "%d", get_num(g_edPpt));
    WritePrivateProfileStringA("Power", "PPT_W", b, g_iniPath);
    snprintf(b, sizeof(b), "%d", get_num(g_edTdc));
    WritePrivateProfileStringA("Power", "TDC_A", b, g_iniPath);
    snprintf(b, sizeof(b), "%d", get_num(g_edEdc));
    WritePrivateProfileStringA("Power", "EDC_A", b, g_iniPath);
    WritePrivateProfileStringA("Startup", "AutoApply",
        IsDlgButtonChecked(g_pages[2], ID_CHK_AUTO) == BST_CHECKED ? "1" : "0",
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
            snprintf(b, sizeof(b), "Failed to start %s (error %lu).", exe, (unsigned long)e);
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
/* Page windows                                                        */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK PageWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    int page = (int)(INT_PTR)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        page = (int)(INT_PTR)cs->lpCreateParams;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)page);
        if (page == 0) {
            make_label(hwnd, 12, 12, 400, "Boost caps -- these are upper limits, NOT a fixed frequency.");
            make_label(hwnd, 12, 32, 400, "Below the cap, Boost still scales and idle still downclocks.");
            make_label(hwnd, 12, 66, 150, "Single-core boost cap (MHz):");
            g_edSingle = make_number(hwnd, ID_ED_SINGLE, ID_UP_SINGLE, 180, 62, 3600, 4200, 4000);
            make_label(hwnd, 12, 100, 150, "All-core boost cap (MHz):");
            g_edAll = make_number(hwnd, ID_ED_ALL, ID_UP_ALL, 180, 96, 3600, 4200, 3900);
            make_label(hwnd, 12, 140, 400, "Stock max is 4200 MHz. 4000 / 3900 lowers the peak VID");
            make_label(hwnd, 12, 158, 400, "from ~1.45 V towards ~1.35 V. See README.md.");
        } else if (page == 1) {
            make_label(hwnd, 12, 12, 400, "Power limits -- stock values for Ryzen 5 3600 (PBO off).");
            make_label(hwnd, 12, 66, 150, "PPT (W):");
            g_edPpt = make_number(hwnd, ID_ED_PPT, ID_UP_PPT, 180, 62, 45, 142, 88);
            make_label(hwnd, 12, 100, 150, "TDC (A):");
            g_edTdc = make_number(hwnd, ID_ED_TDC, ID_UP_TDC, 180, 96, 30, 95, 60);
            make_label(hwnd, 12, 134, 150, "EDC (A):");
            g_edEdc = make_number(hwnd, ID_ED_EDC, ID_UP_EDC, 180, 130, 45, 140, 90);
            make_label(hwnd, 12, 172, 400, "Out-of-range values are clamped by zenstates-c.exe, not rejected.");
        } else {
            HWND chk = CreateWindowExA(0, "BUTTON", "Apply at boot (install scheduled task)",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                       12, 14, 300, 20, hwnd, (HMENU)(INT_PTR)ID_CHK_AUTO, g_hInst, NULL);
            set_font(chk);
            g_chkAuto = chk;
            make_label(hwnd, 12, 48, 100, "Log path (empty = exe dir):");
            g_edLog = make_edit(hwnd, ID_ED_LOG, 12, 68, 400);
            {
                HWND bi = CreateWindowExA(0, "BUTTON", "Install autostart",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                          12, 104, 150, 26, hwnd, (HMENU)(INT_PTR)IDC_INSTALL, g_hInst, NULL);
                set_font(bi);
                HWND bu = CreateWindowExA(0, "BUTTON", "Uninstall autostart",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                          172, 104, 150, 26, hwnd, (HMENU)(INT_PTR)IDC_UNINSTALL, g_hInst, NULL);
                set_font(bu);
            }
            make_label(hwnd, 12, 144, 400, "Autostart = Task Scheduler, ONSTART, as SYSTEM, highest");
            make_label(hwnd, 12, 162, 400, "privileges, 30 s delay. No UAC prompt at boot.");
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_INSTALL || id == IDC_UNINSTALL) {
            /* Forward to the main window; pages do not own the logic. */
            SendMessageA(GetParent(hwnd), WM_COMMAND, wp, lp);
            return 0;
        }
        break;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_page(int index)
{
    int i;
    for (i = 0; i < 3; i++)
        ShowWindow(g_pages[i], i == index ? SW_SHOW : SW_HIDE);
    TabCtrl_SetCurSel(g_hTab, index);
}

static void layout_pages(HWND hwnd)
{
    RECT rc;
    int i;
    GetClientRect(hwnd, &rc);
    SetWindowPos(g_hTab, NULL, 8, 8, rc.right - 16, rc.bottom - 76,
                 SWP_NOZORDER);
    {
        RECT tr = { 8, 8, rc.right - 8, rc.bottom - 68 };
        TabCtrl_AdjustRect(g_hTab, FALSE, &tr);
        for (i = 0; i < 3; i++)
            SetWindowPos(g_pages[i], NULL, tr.left, tr.top,
                         tr.right - tr.left, tr.bottom - tr.top, SWP_NOZORDER);
    }
    SetWindowPos(GetDlgItem(hwnd, IDC_APPLY_NOW), NULL, 8, rc.bottom - 58, 110, 26, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, IDC_SAVE), NULL, 126, rc.bottom - 58, 80, 26, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, IDC_CLOSE), NULL, rc.right - 88, rc.bottom - 58, 80, 26, SWP_NOZORDER);
    SetWindowPos(g_hStatus, NULL, 8, rc.bottom - 28, rc.right - 16, 18, SWP_NOZORDER);
}

/* ------------------------------------------------------------------ */
/* Main window                                                         */
/* ------------------------------------------------------------------ */

static void do_apply_now(HWND hwnd)
{
    char exe[MAX_PATH], msg[512];
    DWORD code = 0;
    if (!save_ini_from_controls()) return;
    snprintf(exe, sizeof(exe), "%s\\zenstates-c.exe", g_exeDir);
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

static void do_autostart(HWND hwnd, int install)
{
    char exe[MAX_PATH], params[1024];
    DWORD code = 0;
    snprintf(exe, sizeof(exe), "%s\\zenstates-c.exe", g_exeDir);
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
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

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
        {
            WNDCLASSA wc;
            memset(&wc, 0, sizeof(wc));
            wc.lpfnWndProc = PageWndProc;
            wc.hInstance = g_hInst;
            wc.lpszClassName = "ZSC_Page";
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            RegisterClassA(&wc);
        }
        for (i = 0; i < 3; i++) {
            g_pages[i] = CreateWindowExA(0, "ZSC_Page", "",
                                        WS_CHILD | WS_CLIPSIBLINGS,
                                        0, 0, 0, 0, hwnd, NULL, g_hInst,
                                        (LPVOID)(INT_PTR)i);
        }
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
        layout_pages(hwnd);
        {
            char sb[600];
            snprintf(sb, sizeof(sb), "INI: %s%s", g_iniPath,
                     g_iniFound ? "" : " (not found -- defaults shown, Save will create it)");
            status(sb);
        }
        return 0;
    }
    case WM_SIZE:
        layout_pages(hwnd);
        return 0;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE)
            show_page(TabCtrl_GetCurSel(g_hTab));
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_APPLY_NOW) do_apply_now(hwnd);
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

    hwnd = CreateWindowExA(0, "ZenStatesC_GUI", "ZenStates-C -- Boost cap settings (Ryzen 5 3600)",
                           WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, 470, 370,
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
