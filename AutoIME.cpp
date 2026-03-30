#include <windows.h>
#include <shellapi.h>
#include <imm.h>
#include <psapi.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <sstream>

#pragma comment(lib, "Imm32.lib")
#pragma comment(lib, "Comctl32.lib")

#ifndef IMC_SETOPENSTATUS
#define IMC_SETOPENSTATUS 0x0006
#endif


// ================================================================
// 상수
// ================================================================

#define WM_TRAYICON         (WM_USER + 1)
#define TRAYICON_ID         1

// 트레이 메뉴 커맨드
#define ID_TOGGLE_IME       1001
#define ID_TOGGLE_REMAP     1002
#define ID_SET_REMAP_KEY    1003
#define ID_MANAGE_TARGETS   1004
#define ID_TOGGLE_STARTUP   1005
#define ID_EXIT             1006

// 리맵 다이얼로그 컨트롤
#define IDC_REMAP_COMBO     2001
#define IDC_REMAP_EDIT      2002
#define IDC_REMAP_OK        2003
#define IDC_REMAP_CANCEL    2004

// 프로그램 관리 다이얼로그 컨트롤
#define IDC_TARGET_LIST     3001
#define IDC_TARGET_PROC     3002
#define IDC_TARGET_EDIT     3003
#define IDC_TARGET_ADD      3004
#define IDC_TARGET_DEL      3005
#define IDC_TARGET_OK       3006
#define IDC_TARGET_CANCEL   3007
#define IDC_TARGET_REFRESH  3008


// ================================================================
// 확장 키 테이블 (F13~F24)
// 일반 키보드에 없어 다른 키와 충돌 없이 사용 가능한 키들
// ================================================================

struct KeyEntry { DWORD vk; const wchar_t* label; };

static const KeyEntry EXTENDED_KEYS[] = {
    { VK_F13, L"F13" }, { VK_F14, L"F14" }, { VK_F15, L"F15" },
    { VK_F16, L"F16" }, { VK_F17, L"F17" }, { VK_F18, L"F18" },
    { VK_F19, L"F19" }, { VK_F20, L"F20" }, { VK_F21, L"F21" },
    { VK_F22, L"F22" }, { VK_F23, L"F23" }, { VK_F24, L"F24" },
};
static const int EXTENDED_KEYS_COUNT = (int)(sizeof(EXTENDED_KEYS) / sizeof(EXTENDED_KEYS[0]));


// ================================================================
// 앱 설정 (config.ini 에서 읽고 씀)
// ================================================================

struct Config
{
    bool  autoEnglish  = true;
    bool  remapEnabled = false;
    DWORD remapVKey    = VK_F13;
    std::vector<std::wstring> targets = { L"clipstudiopaint", L"photoshop" };
    wchar_t path[MAX_PATH] = {};

    static std::wstring VKeyToString(DWORD vk)
    {
        for (int i = 0; i < EXTENDED_KEYS_COUNT; i++)
            if (EXTENDED_KEYS[i].vk == vk)
                return EXTENDED_KEYS[i].label;
        if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
            return std::wstring(1, (wchar_t)vk);
        wchar_t buf[16];
        swprintf_s(buf, L"%u", vk);
        return buf;
    }

    static DWORD StringToVKey(const std::wstring& s)
    {
        if (s.empty()) return VK_F13;
        for (int i = 0; i < EXTENDED_KEYS_COUNT; i++)
            if (s == EXTENDED_KEYS[i].label)
                return EXTENDED_KEYS[i].vk;
        if (s.length() == 1)
        {
            wchar_t c = towupper(s[0]);
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                return (DWORD)c;
        }
        try {
            unsigned long v = std::stoul(s);
            if (v > 0 && v < 256) return (DWORD)v;
        }
        catch (...) {}
        return VK_F13;
    }

    void InitPath()
    {
        GetModuleFileNameW(NULL, path, MAX_PATH);
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) *(slash + 1) = L'\0';
        wcscat_s(path, L"config.ini");
    }

    static std::wstring ParseValue(const std::wstring& line, const wchar_t* key)
    {
        std::wstring prefix = std::wstring(key) + L'=';
        if (line.rfind(prefix, 0) == 0)
            return line.substr(prefix.length());
        return {};
    }

    void Load()
    {
        std::wifstream f(path);
        if (!f.good()) { Save(); return; }

        targets.clear();
        std::wstring line;
        while (std::getline(f, line))
        {
            std::wstring v;
            if (!(v = ParseValue(line, L"AutoEnglish")).empty())
                autoEnglish = (v == L"1");
            else if (!(v = ParseValue(line, L"RemapEnabled")).empty())
                remapEnabled = (v == L"1");
            else if (!(v = ParseValue(line, L"RemapKey")).empty())
                remapVKey = StringToVKey(v);
            else if (!(v = ParseValue(line, L"Target")).empty())
                targets.push_back(v);
        }
        if (targets.empty())
            targets = { L"clipstudiopaint", L"photoshop" };
    }

    void Save() const
    {
        std::wofstream f(path, std::ios::trunc);
        f << L"AutoEnglish="  << (autoEnglish  ? 1 : 0) << L"\n";
        f << L"RemapEnabled=" << (remapEnabled ? 1 : 0) << L"\n";
        f << L"RemapKey="     << VKeyToString(remapVKey)  << L"\n";
        for (const auto& t : targets)
            f << L"Target=" << t << L"\n";
    }
};


// ================================================================
// 포커스 프로세스 감지
// ================================================================

struct ProcessWatcher
{
    HWND cachedHwnd     = NULL;
    bool cachedIsTarget = false;

    // 참고: HWND는 Windows 내부에서 재활용될 수 있으므로
    // 같은 HWND라도 다른 프로세스일 수 있다 (실용상 문제없는 수준).
    bool IsTarget(const std::vector<std::wstring>& targets)
    {
        HWND hwnd = GetForegroundWindow();

        if (hwnd && hwnd == cachedHwnd)
            return cachedIsTarget;

        cachedHwnd     = hwnd;
        cachedIsTarget = false;

        if (!hwnd) return false;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);

        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!h) return false;

        wchar_t name[MAX_PATH] = {};
        GetModuleBaseNameW(h, NULL, name, MAX_PATH);
        CloseHandle(h);

        std::wstring exe = name;
        std::transform(exe.begin(), exe.end(), exe.begin(), towlower);

        for (const auto& t : targets)
        {
            std::wstring tl = t;
            std::transform(tl.begin(), tl.end(), tl.begin(), towlower);
            if (exe.find(tl) != std::wstring::npos)
            {
                cachedIsTarget = true;
                break;
            }
        }
        return cachedIsTarget;
    }

    // 타겟 목록 변경 시 캐시 무효화
    void Invalidate() { cachedHwnd = NULL; }
};


// ================================================================
// 앱 전체 상태 (전역 하나로 묶음)
// ================================================================

struct AppState
{
    Config          config;
    ProcessWatcher  watcher;
    HINSTANCE       hInstance = NULL;
    HWND            hwndMsg   = NULL;
    HHOOK           mouseHook = NULL;
    HHOOK           kbdHook   = NULL;
    NOTIFYICONDATAW nid       = {};
    POINT           lastPos   = { -1, -1 };

    bool    dlgOpen         = false;
    DWORD   dlgCapturedVKey = 0;
    WNDPROC dlgOrigEditProc = NULL;
} g_app;


// ================================================================
// IME
// ================================================================

void ForceEnglish()
{
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return;
    HWND ime = ImmGetDefaultIMEWnd(hwnd);
    if (!ime) return;
    SendMessage(ime, WM_IME_CONTROL, IMC_SETOPENSTATUS, 0);
}


// ================================================================
// 트레이 아이콘
// ================================================================

void UpdateTrayIcon()
{
    g_app.nid.hIcon = LoadIcon(g_app.hInstance,
        g_app.config.autoEnglish ? MAKEINTRESOURCE(1) : MAKEINTRESOURCE(2));
    Shell_NotifyIcon(NIM_MODIFY, &g_app.nid);
}


// ================================================================
// 공용 유틸
// ================================================================

static std::wstring GetKeyDisplayName(DWORD vk)
{
    for (int i = 0; i < EXTENDED_KEYS_COUNT; i++)
        if (EXTENDED_KEYS[i].vk == vk)
            return EXTENDED_KEYS[i].label;
    wchar_t buf[64] = {};
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (sc) GetKeyNameTextW((LONG)(sc << 16), buf, 64);
    return buf[0] ? buf : Config::VKeyToString(vk);
}

// 실행 중인 프로세스 목록 수집 (소문자, .exe 제거, 정렬, 중복 제거)
static std::vector<std::wstring> GetRunningProcesses()
{
    std::vector<std::wstring> list;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return list;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do {
            std::wstring name = pe.szExeFile;
            std::transform(name.begin(), name.end(), name.begin(), towlower);
            if (name.size() > 4 && name.substr(name.size() - 4) == L".exe")
                name = name.substr(0, name.size() - 4);
            if (std::find(list.begin(), list.end(), name) == list.end())
                list.push_back(name);
        }
        while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    std::sort(list.begin(), list.end());
    return list;
}


// ================================================================
// DlgBuf — 바이트 버퍼로 DLGTEMPLATE 직접 구성 (리소스 파일 없이)
// ================================================================

struct DlgBuf
{
    static const DWORD CAPACITY = 4096;
    BYTE  data[CAPACITY] = {};
    DWORD pos = 0;

    void align4() { pos = (pos + 3) & ~3u; }

    void writeW(WORD w)
    {
        if (pos + 2 > CAPACITY) return;
        *(WORD*)(data + pos) = w; pos += 2;
    }
    void writeDW(DWORD d)
    {
        if (pos + 4 > CAPACITY) return;
        *(DWORD*)(data + pos) = d; pos += 4;
    }
    void writeStr(const wchar_t* s)
    {
        while (*s) writeW((WORD)*s++);
        writeW(0);
    }
    // 컨트롤 하나 기록
    void writeCtrl(DWORD style, short x, short y, short cx, short cy,
                   WORD id, WORD cls, const wchar_t* text)
    {
        align4();
        writeDW(style); writeDW(0);
        writeW((WORD)x); writeW((WORD)y); writeW((WORD)cx); writeW((WORD)cy);
        writeW(id);
        writeW(0xFFFF); writeW(cls);
        writeStr(text);
        writeW(0);
    }
    // 다이얼로그 헤더 기록
    void writeHeader(WORD ctrlCount, short w, short h, const wchar_t* title)
    {
        writeDW(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT);
        writeDW(0);
        writeW(ctrlCount);
        writeW(0); writeW(0); writeW((WORD)w); writeW((WORD)h);
        writeW(0); writeW(0);
        writeStr(title);
        writeW(9);
        writeStr(L"MS Shell Dlg");
    }
};


// ================================================================
// 리맵 키 다이얼로그
// 상단: F13~F24 드롭다운 (ComboBox)
// 하단: 직접 키 입력 (Edit, 키 캡처)
// ================================================================

LRESULT CALLBACK EditKeyCapture(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
    {
        DWORD vk = (DWORD)wParam;

        if (vk == VK_RETURN) { SendMessage(GetParent(hwnd), WM_COMMAND, IDC_REMAP_OK,     0); return 0; }
        if (vk == VK_ESCAPE) { SendMessage(GetParent(hwnd), WM_COMMAND, IDC_REMAP_CANCEL, 0); return 0; }
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
            vk == VK_LWIN  || vk == VK_RWIN    || vk == VK_TAB)
            return 0;

        g_app.dlgCapturedVKey = vk;

        // 콤보박스 선택 동기화
        HWND hCombo = GetDlgItem(GetParent(hwnd), IDC_REMAP_COMBO);
        SendMessage(hCombo, CB_SETCURSEL, (WPARAM)-1, 0);
        for (int i = 0; i < EXTENDED_KEYS_COUNT; i++)
        {
            if (EXTENDED_KEYS[i].vk == vk)
            {
                SendMessage(hCombo, CB_SETCURSEL, (WPARAM)i, 0);
                break;
            }
        }
        SetWindowTextW(hwnd, GetKeyDisplayName(vk).c_str());
        return 0;
    }
    return CallWindowProc(g_app.dlgOrigEditProc, hwnd, msg, wParam, lParam);
}

INT_PTR CALLBACK RemapDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        g_app.dlgCapturedVKey = g_app.config.remapVKey;

        // 콤보박스 채우기 + 현재 키 선택
        HWND hCombo = GetDlgItem(hDlg, IDC_REMAP_COMBO);
        for (int i = 0; i < EXTENDED_KEYS_COUNT; i++)
        {
            int idx = (int)SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)EXTENDED_KEYS[i].label);
            if (EXTENDED_KEYS[i].vk == g_app.config.remapVKey)
                SendMessage(hCombo, CB_SETCURSEL, (WPARAM)idx, 0);
        }

        // Edit 서브클래싱 (키 직접 캡처)
        HWND hEdit = GetDlgItem(hDlg, IDC_REMAP_EDIT);
        g_app.dlgOrigEditProc = (WNDPROC)SetWindowLongPtr(
            hEdit, GWLP_WNDPROC, (LONG_PTR)EditKeyCapture);
        SetWindowTextW(hEdit, GetKeyDisplayName(g_app.config.remapVKey).c_str());

        SetFocus(hEdit);
        return FALSE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        // 콤보 선택 → Edit 동기화
        case IDC_REMAP_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                HWND hCombo = GetDlgItem(hDlg, IDC_REMAP_COMBO);
                int idx = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
                if (idx >= 0 && idx < EXTENDED_KEYS_COUNT)
                {
                    g_app.dlgCapturedVKey = EXTENDED_KEYS[idx].vk;
                    SetWindowTextW(GetDlgItem(hDlg, IDC_REMAP_EDIT), EXTENDED_KEYS[idx].label);
                }
            }
            break;

        case IDC_REMAP_OK:
            if (g_app.dlgCapturedVKey != 0)
            {
                g_app.config.remapVKey = g_app.dlgCapturedVKey;
                g_app.config.Save();
            }
            EndDialog(hDlg, IDOK);
            return TRUE;

        case IDC_REMAP_CANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void ShowRemapDialog(HWND hwndParent)
{
    if (g_app.dlgOpen) return;
    g_app.dlgOpen = true;

    DlgBuf b;
    // 컨트롤 6개: STATIC, COMBOBOX, STATIC, EDIT, 확인, 취소
    b.writeHeader(6, 200, 90, L"Capslock \ud0a4 \uc124\uc815");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | SS_LEFT,
                5, 5, 190, 10, 0, 0x0082,
                L"F13~F24 \ubaa9\ub85d\uc5d0\uc11c \uc120\ud0dd\ud558\uac70\ub098 \ud0a4\ub97c \ub204\ub974\uc138\uc694:");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                5, 17, 190, 120, IDC_REMAP_COMBO, 0x0085, L"");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | SS_LEFT,
                5, 38, 55, 10, 0, 0x0082,
                L"\uc9c1\uc811 \uc785\ub825:");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_CENTER | ES_READONLY,
                63, 36, 132, 14, IDC_REMAP_EDIT, 0x0081, L"");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                40, 70, 50, 14, IDC_REMAP_OK, 0x0080, L"\ud655\uc778");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                110, 70, 50, 14, IDC_REMAP_CANCEL, 0x0080, L"\ucde8\uc18c");

    DialogBoxIndirectParamW(g_app.hInstance, (LPCDLGTEMPLATEW)b.data,
                            hwndParent, RemapDlgProc, 0);
    g_app.dlgOpen = false;
}


// ================================================================
// 프로그램 관리 다이얼로그
// 상단: 등록 목록 (ListBox) + 삭제 버튼
// 하단: 실행 중 프로세스 콤보 (새로고침) + 직접 입력 Edit + 추가 버튼
// ================================================================

static std::vector<std::wstring> g_dlgTargets;  // 다이얼로그 내 임시 목록

static void TargetList_Refresh(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_TARGET_LIST);
    SendMessage(hList, LB_RESETCONTENT, 0, 0);
    for (const auto& t : g_dlgTargets)
        SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)t.c_str());
}

static void ProcCombo_Refresh(HWND hDlg)
{
    HWND hCombo = GetDlgItem(hDlg, IDC_TARGET_PROC);
    SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
    for (const auto& p : GetRunningProcesses())
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)p.c_str());
}

INT_PTR CALLBACK TargetDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        g_dlgTargets = g_app.config.targets;
        TargetList_Refresh(hDlg);
        ProcCombo_Refresh(hDlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        // 프로세스 콤보 선택 → Edit 동기화
        case IDC_TARGET_PROC:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                HWND hCombo = GetDlgItem(hDlg, IDC_TARGET_PROC);
                int idx = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
                if (idx >= 0)
                {
                    wchar_t buf[MAX_PATH] = {};
                    SendMessage(hCombo, CB_GETLBTEXT, (WPARAM)idx, (LPARAM)buf);
                    SetDlgItemTextW(hDlg, IDC_TARGET_EDIT, buf);
                }
            }
            break;

        case IDC_TARGET_REFRESH:
            ProcCombo_Refresh(hDlg);
            break;

        case IDC_TARGET_ADD:
        {
            wchar_t buf[MAX_PATH] = {};
            GetDlgItemTextW(hDlg, IDC_TARGET_EDIT, buf, MAX_PATH);
            std::wstring name = buf;
            std::transform(name.begin(), name.end(), name.begin(), towlower);
            // .exe 제거
            if (name.size() > 4 && name.substr(name.size() - 4) == L".exe")
                name = name.substr(0, name.size() - 4);
            if (!name.empty() &&
                std::find(g_dlgTargets.begin(), g_dlgTargets.end(), name) == g_dlgTargets.end())
            {
                g_dlgTargets.push_back(name);
                TargetList_Refresh(hDlg);
                SetDlgItemTextW(hDlg, IDC_TARGET_EDIT, L"");
            }
            break;
        }

        case IDC_TARGET_DEL:
        {
            HWND hList = GetDlgItem(hDlg, IDC_TARGET_LIST);
            int idx = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
            if (idx >= 0 && idx < (int)g_dlgTargets.size())
            {
                g_dlgTargets.erase(g_dlgTargets.begin() + idx);
                TargetList_Refresh(hDlg);
            }
            break;
        }

        case IDC_TARGET_OK:
            g_app.config.targets = g_dlgTargets;
            g_app.config.Save();
            g_app.watcher.Invalidate();
            EndDialog(hDlg, IDOK);
            return TRUE;

        case IDC_TARGET_CANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void ShowTargetDialog(HWND hwndParent)
{
    if (g_app.dlgOpen) return;
    g_app.dlgOpen = true;

    DlgBuf b;
    // 컨트롤 11개: STATIC, LISTBOX, 삭제, STATIC, 새로고침, COMBOBOX, STATIC, EDIT, 추가, 확인, 취소
    b.writeHeader(11, 220, 175, L"\ub300\uc0c1 \ud504\ub85c\uadf8\ub7a8 \uad00\ub9ac");

    // 등록 목록 레이블
    b.writeCtrl(WS_CHILD | WS_VISIBLE | SS_LEFT,
                5, 5, 210, 10, 0, 0x0082,
                L"\ub4f1\ub85d\ub41c \ud504\ub85c\uadf8\ub7a8:");

    // 등록 목록 ListBox
    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                5, 17, 170, 55, IDC_TARGET_LIST, 0x0083, L"");

    // 삭제 버튼
    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                180, 17, 35, 14, IDC_TARGET_DEL, 0x0080, L"\uc0ad\uc81c");

    // 실행 중 프로세스 레이블
    b.writeCtrl(WS_CHILD | WS_VISIBLE | SS_LEFT,
                5, 80, 140, 10, 0, 0x0082,
                L"\uc2e4\ud589 \uc911\uc778 \ud504\ub85c\uc138\uc2a4:");

    // 새로고침 버튼
    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                150, 78, 65, 14, IDC_TARGET_REFRESH, 0x0080, L"\uc0c8\ub85c\uace0\uce68");

    // 프로세스 콤보
    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                5, 92, 210, 120, IDC_TARGET_PROC, 0x0085, L"");

    // 직접 입력 레이블
    b.writeCtrl(WS_CHILD | WS_VISIBLE | SS_LEFT,
                5, 115, 55, 10, 0, 0x0082,
                L"\uc9c1\uc811 \uc785\ub825:");

    // 직접 입력 Edit
    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                63, 113, 112, 14, IDC_TARGET_EDIT, 0x0081, L"");

    // 추가 버튼
    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                180, 113, 35, 14, IDC_TARGET_ADD, 0x0080, L"\ucd94\uac00");

    // 확인 버튼
    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                50, 155, 50, 14, IDC_TARGET_OK, 0x0080, L"\ud655\uc778");

    // 취소 버튼
    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                120, 155, 50, 14, IDC_TARGET_CANCEL, 0x0080, L"\ucde8\uc18c");

    DialogBoxIndirectParamW(g_app.hInstance, (LPCDLGTEMPLATEW)b.data,
                            hwndParent, TargetDlgProc, 0);
    g_app.dlgOpen = false;
}


// ================================================================
// 훅 프로시저
// ================================================================

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode < 0) return CallNextHookEx(g_app.kbdHook, nCode, wParam, lParam);

    auto* kb = (KBDLLHOOKSTRUCT*)lParam;

    if (kb->vkCode == VK_CAPITAL &&
        (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN ||
         wParam == WM_KEYUP   || wParam == WM_SYSKEYUP))
    {
        if (g_app.config.remapEnabled && g_app.watcher.IsTarget(g_app.config.targets))
        {
            bool up        = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
            INPUT inp      = {};
            inp.type       = INPUT_KEYBOARD;
            inp.ki.wVk     = (WORD)g_app.config.remapVKey;
            inp.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
            SendInput(1, &inp, sizeof(INPUT));
            return 1;  // 원래 Capslock 이벤트 차단
        }
    }

    return CallNextHookEx(g_app.kbdHook, nCode, wParam, lParam);
}

LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && wParam == WM_MOUSEMOVE && g_app.config.autoEnglish)
    {
        POINT p;
        GetCursorPos(&p);

        if (p.x != g_app.lastPos.x || p.y != g_app.lastPos.y)
        {
            g_app.lastPos = p;
            if (g_app.watcher.IsTarget(g_app.config.targets))
                ForceEnglish();
        }
    }

    return CallNextHookEx(g_app.mouseHook, nCode, wParam, lParam);
}


// ================================================================
// 시작프로그램 레지스트리 (HKCU\...\Run)
// 관리자 권한 없이 현재 사용자 범위에서만 등록
// ================================================================

static const wchar_t* RUN_KEY  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_NAME = L"AutoIME";

static bool Startup_IsRegistered()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;

    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    bool found = (RegQueryValueExW(hKey, RUN_NAME, NULL, NULL,
                                   (BYTE*)buf, &size) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return found;
}

static void Startup_Register()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return;

    RegSetValueExW(hKey, RUN_NAME, 0, REG_SZ,
                   (BYTE*)exePath, (DWORD)((wcslen(exePath) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
}

static void Startup_Unregister()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return;

    RegDeleteValueW(hKey, RUN_NAME);
    RegCloseKey(hKey);
}


// ================================================================
// 트레이 메뉴
//
// ✓ 자동 영어 전환
// ✓ Capslock 리맵
// ─────────────────
//   설정 ▶
//     Capslock → F13 설정...
//     대상 프로그램 관리...
// ─────────────────
// 종료
// ================================================================

void ShowTrayMenu(HWND hwnd)
{
    wchar_t remapLabel[64];
    swprintf_s(remapLabel, L"Capslock \u2192 %s \uc124\uc815...",
               GetKeyDisplayName(g_app.config.remapVKey).c_str());

    // 설정 서브메뉴
    bool startupOn = Startup_IsRegistered();
    HMENU hSub = CreatePopupMenu();
    InsertMenuW(hSub, -1, MF_BYPOSITION, ID_SET_REMAP_KEY, remapLabel);
    InsertMenuW(hSub, -1, MF_BYPOSITION, ID_MANAGE_TARGETS,
        L"\ub300\uc0c1 \ud504\ub85c\uadf8\ub7a8 \uad00\ub9ac...");
    InsertMenuW(hSub, -1, MF_SEPARATOR, 0, NULL);
    InsertMenuW(hSub, -1,
        MF_BYPOSITION | (startupOn ? MF_CHECKED : 0),
        ID_TOGGLE_STARTUP,
        L"\uc2dc\uc791 \ud504\ub85c\uadf8\ub7a8 \ub4f1\ub85d");    // "시작 프로그램 등록"

    // 메인 메뉴
    HMENU hMenu = CreatePopupMenu();

    InsertMenuW(hMenu, -1,
        MF_BYPOSITION | (g_app.config.autoEnglish ? MF_CHECKED : 0),
        ID_TOGGLE_IME, L"\uc790\ub3d9 \uc601\uc5b4 \uc804\ud658");

    InsertMenuW(hMenu, -1,
        MF_BYPOSITION | (g_app.config.remapEnabled ? MF_CHECKED : 0),
        ID_TOGGLE_REMAP, L"Capslock \ub9ac\ub9f5");

    InsertMenuW(hMenu, -1, MF_SEPARATOR, 0, NULL);

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_POPUP,
        (UINT_PTR)hSub, L"\uc124\uc815");

    InsertMenuW(hMenu, -1, MF_SEPARATOR, 0, NULL);

    InsertMenuW(hMenu, -1, MF_BYPOSITION, ID_EXIT, L"\uc885\ub8cc");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);  // hSub 도 함께 해제됨

    switch (cmd)
    {
    case ID_TOGGLE_IME:
        g_app.config.autoEnglish = !g_app.config.autoEnglish;
        g_app.config.Save();
        UpdateTrayIcon();
        break;

    case ID_TOGGLE_REMAP:
        g_app.config.remapEnabled = !g_app.config.remapEnabled;
        g_app.config.Save();
        break;

    case ID_SET_REMAP_KEY:
        ShowRemapDialog(hwnd);
        break;

    case ID_MANAGE_TARGETS:
        ShowTargetDialog(hwnd);
        break;

    case ID_TOGGLE_STARTUP:
        if (Startup_IsRegistered())
            Startup_Unregister();
        else
            Startup_Register();
        break;

    case ID_EXIT:
        PostQuitMessage(0);
        break;
    }
}


// ================================================================
// 메시지 윈도우 프로시저
// ================================================================

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_TRAYICON && lParam == WM_RBUTTONUP)
        ShowTrayMenu(hwnd);

    return DefWindowProc(hwnd, msg, wParam, lParam);
}


// ================================================================
// WinMain
// ================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    g_app.hInstance = hInstance;

    g_app.config.InitPath();
    g_app.config.Load();

    WNDCLASSW wc     = {};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = L"AutoIME";
    RegisterClassW(&wc);

    g_app.hwndMsg = CreateWindowExW(0, L"AutoIME", L"", 0,
                                    0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!g_app.hwndMsg)
    {
        MessageBoxW(NULL, L"\uc708\ub3c4\uc6b0 \uc0dd\uc131 \uc2e4\ud328",
                    L"AutoIME", MB_ICONERROR);
        return 1;
    }

    auto& nid            = g_app.nid;
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_app.hwndMsg;
    nid.uID              = TRAYICON_ID;
    nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon            = LoadIcon(hInstance,
        g_app.config.autoEnglish ? MAKEINTRESOURCE(1) : MAKEINTRESOURCE(2));
    wcscpy_s(nid.szTip, L"Auto IME");

    if (!Shell_NotifyIcon(NIM_ADD, &nid))
    {
        MessageBoxW(NULL, L"\ud2b8\ub808\uc774 \uc544\uc774\ucf58 \ub4f1\ub85d \uc2e4\ud328",
                    L"AutoIME", MB_ICONERROR);
        return 1;
    }

    g_app.mouseHook = SetWindowsHookEx(WH_MOUSE_LL,    MouseProc,    NULL, 0);
    g_app.kbdHook   = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, NULL, 0);

    if (!g_app.mouseHook || !g_app.kbdHook)
        MessageBoxW(NULL, L"\ud6c5 \uc124\uce58 \uc2e4\ud328",
                    L"AutoIME", MB_ICONWARNING);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_app.mouseHook) UnhookWindowsHookEx(g_app.mouseHook);
    if (g_app.kbdHook)   UnhookWindowsHookEx(g_app.kbdHook);
    Shell_NotifyIcon(NIM_DELETE, &g_app.nid);

    return 0;
}