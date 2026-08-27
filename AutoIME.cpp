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
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Gdi32.lib")

#ifndef IMC_SETOPENSTATUS
#define IMC_SETOPENSTATUS 0x0006
#endif


// ================================================================
// 상수
// ================================================================

#define WM_TRAYICON         (WM_USER + 1)
#define TRAYICON_ID         1
#define TIMER_DBLCLK        2001

// 트레이 메뉴 커맨드
#define ID_TOGGLE_IME       1001
#define ID_TOGGLE_REMAP     1002
#define ID_TOGGLE_TAB       1003
#define ID_SET_REMAP_KEY    1005
#define ID_SET_TAB_KEY      1006
#define ID_MANAGE_TARGETS   1007
#define ID_TOGGLE_STARTUP   1008
#define ID_EXIT             1009

// 리맵 다이얼로그 컨트롤 (Capslock / Tab 공용)
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
#define IDC_TARGET_EDITPROF 3009

// 프로필 편집 다이얼로그 컨트롤 (프로그램 하나의 전체 설정)
#define IDC_PF_TITLE        4001
#define IDC_PF_AE_CHECK     4002
#define IDC_PF_REMAP_CHECK  4003
#define IDC_PF_REMAP_COMBO  4004
#define IDC_PF_TAB_CHECK    4005
#define IDC_PF_TAB_COMBO    4006
#define IDC_PF_OK           4007
#define IDC_PF_CANCEL       4008


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
// 프로그램 프로필
// 프로그램 하나가 가지는 완전한 설정 — 더 이상 "전역" 개념 없음.
// 새로 추가되는 프로그램은 DefaultProfile() 값으로 시작.
// ================================================================

struct TargetProfile
{
    std::wstring name;
    bool  autoEnglish  = true;
    bool  remapEnabled = false;
    DWORD remapVKey    = VK_F13;
    bool  tabEnabled   = false;
    DWORD tabVKey      = 0;       // 0 = 비활성화(입력 차단), 그 외 = 리맵 대상 키

    static TargetProfile DefaultProfile(const std::wstring& name)
    {
        TargetProfile p;
        p.name = name;
        return p;   // 위의 기본값 그대로: 자동영어 ON, 리맵 OFF
    }
};


// ================================================================
// 키 유틸
// ================================================================

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


// ================================================================
// 앱 설정 (config.ini 에서 읽고 씀)
// 전역 설정 없음 — 프로그램별 프로필 목록만 존재.
// ================================================================

struct Config
{
    std::vector<TargetProfile> profiles;
    wchar_t path[MAX_PATH] = {};

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

    // "이름|ae=1|remap=1|remapkey=F13|tab=0|tabkey=0" 형식으로 인코딩
    static std::wstring EncodeProfile(const TargetProfile& p)
    {
        std::wstring s = p.name;
        s += L"|ae="       + std::to_wstring(p.autoEnglish ? 1 : 0);
        s += L"|remap="    + std::to_wstring(p.remapEnabled ? 1 : 0);
        s += L"|remapkey=" + VKeyToString(p.remapVKey);
        s += L"|tab="      + std::to_wstring(p.tabEnabled ? 1 : 0);
        s += L"|tabkey="   + (p.tabVKey == 0 ? L"0" : VKeyToString(p.tabVKey));
        return s;
    }

    static TargetProfile DecodeProfile(const std::wstring& s)
    {
        TargetProfile p;
        std::wstringstream ss(s);
        std::wstring token;
        bool first = true;
        while (std::getline(ss, token, L'|'))
        {
            if (first) { p.name = token; first = false; continue; }
            size_t eq = token.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring key = token.substr(0, eq);
            std::wstring val = token.substr(eq + 1);
            if      (key == L"ae")       p.autoEnglish  = (val == L"1");
            else if (key == L"remap")    p.remapEnabled = (val == L"1");
            else if (key == L"remapkey") p.remapVKey    = StringToVKey(val);
            else if (key == L"tab")      p.tabEnabled   = (val == L"1");
            else if (key == L"tabkey")   p.tabVKey      = (val == L"0") ? 0 : StringToVKey(val);
        }
        return p;
    }

    // 구버전 config.ini(전역 설정 + Target=이름|ae=..|remap=..|tab=.. 3필드)를
    // 감지해서, 각 프로그램에 그 시점의 전역값을 그대로 복사해 넣는다.
    // 반환값: 마이그레이션을 수행했으면 true (호출 측에서 즉시 재저장하도록)
    bool MigrateFromLegacy(std::wistream& f)
    {
        bool legacyAutoEnglish = true;
        bool legacyRemapEnabled = false;
        DWORD legacyRemapKey = VK_F13;
        bool legacyTabEnabled = false;
        DWORD legacyTabKey = 0;
        std::vector<std::wstring> legacyTargetLines;
        bool isLegacy = false;

        std::wstring line;
        while (std::getline(f, line))
        {
            std::wstring v;
            if (!(v = ParseValue(line, L"AutoEnglish")).empty())
            { legacyAutoEnglish = (v == L"1"); isLegacy = true; }
            else if (!(v = ParseValue(line, L"RemapEnabled")).empty())
            { legacyRemapEnabled = (v == L"1"); isLegacy = true; }
            else if (!(v = ParseValue(line, L"RemapKey")).empty())
            { legacyRemapKey = StringToVKey(v); isLegacy = true; }
            else if (!(v = ParseValue(line, L"TabEnabled")).empty())
            { legacyTabEnabled = (v == L"1"); isLegacy = true; }
            else if (!(v = ParseValue(line, L"TabKey")).empty())
            { legacyTabKey = (v == L"0") ? 0 : StringToVKey(v); isLegacy = true; }
            else if (!(v = ParseValue(line, L"Target")).empty())
                legacyTargetLines.push_back(v);
        }

        if (!isLegacy) return false;   // 구버전 키가 하나도 없으면 마이그레이션 대상 아님

        // 구버전 Target 라인은 "이름|ae=-1|remap=-1|tab=-1" (override, -1=전역 따름) 형식이었음.
        // 각 이름을 꺼내서, 새 TargetProfile을 "그 시점의 전역값"으로 채운다.
        // (override가 -1이 아니었다면 그 override 값을 우선 적용 — 곧 프로그램별로 다르게 켜둔 걸 보존)
        profiles.clear();
        for (const auto& tline : legacyTargetLines)
        {
            std::wstringstream ss(tline);
            std::wstring token;
            std::wstring name;
            int ovAe = -1, ovRemap = -1, ovTab = -1;
            bool first = true;
            while (std::getline(ss, token, L'|'))
            {
                if (first) { name = token; first = false; continue; }
                size_t eq = token.find(L'=');
                if (eq == std::wstring::npos) continue;
                std::wstring key = token.substr(0, eq);
                int val = -1;
                try { val = std::stoi(token.substr(eq + 1)); } catch (...) {}
                if      (key == L"ae")    ovAe    = val;
                else if (key == L"remap") ovRemap = val;
                else if (key == L"tab")   ovTab   = val;
            }

            TargetProfile p;
            p.name = name;
            p.autoEnglish  = (ovAe    != -1) ? (ovAe    == 1) : legacyAutoEnglish;
            p.remapEnabled = (ovRemap != -1) ? (ovRemap == 1) : legacyRemapEnabled;
            p.remapVKey    = legacyRemapKey;   // 리맵 키 자체는 구버전에 프로그램별 값이 없었음
            p.tabEnabled   = (ovTab   != -1) ? (ovTab   == 1) : legacyTabEnabled;
            p.tabVKey      = legacyTabKey;
            profiles.push_back(p);
        }

        if (profiles.empty())
            profiles = { TargetProfile::DefaultProfile(L"clipstudiopaint"),
                         TargetProfile::DefaultProfile(L"photoshop") };

        return true;
    }

    void Load()
    {
        std::wifstream f(path);
        if (!f.good())
        {
            profiles = { TargetProfile::DefaultProfile(L"clipstudiopaint"),
                         TargetProfile::DefaultProfile(L"photoshop") };
            Save();
            return;
        }

        // 먼저 구버전 형식인지 확인 (구버전 키: AutoEnglish/RemapEnabled/... 최상위)
        // 신버전은 Profile= 라인만 사용하므로, 둘 다 시도해본다.
        std::wstringstream buffer;
        buffer << f.rdbuf();
        std::wstring allText = buffer.str();

        // 신버전 형식(Profile=) 먼저 확인
        bool hasNewFormat = (allText.find(L"Profile=") != std::wstring::npos);

        if (hasNewFormat)
        {
            profiles.clear();
            std::wstringstream ss(allText);
            std::wstring line;
            while (std::getline(ss, line))
            {
                std::wstring v = ParseValue(line, L"Profile");
                if (!v.empty())
                    profiles.push_back(DecodeProfile(v));
            }
            if (profiles.empty())
                profiles = { TargetProfile::DefaultProfile(L"clipstudiopaint"),
                             TargetProfile::DefaultProfile(L"photoshop") };
            return;
        }

        // 구버전 형식 마이그레이션 시도
        std::wstringstream ss2(allText);
        if (MigrateFromLegacy(ss2))
        {
            Save();   // 마이그레이션 결과를 신버전 형식으로 즉시 재저장
            return;
        }

        // 아무 형식도 아니면(빈 파일 등) 기본값
        profiles = { TargetProfile::DefaultProfile(L"clipstudiopaint"),
                     TargetProfile::DefaultProfile(L"photoshop") };
    }

    void Save() const
    {
        std::wofstream f(path, std::ios::trunc);
        for (const auto& p : profiles)
            f << L"Profile=" << EncodeProfile(p) << L"\n";
    }

    TargetProfile* Find(const std::wstring& name)
    {
        for (auto& p : profiles)
            if (p.name == name) return &p;
        return nullptr;
    }
};


// ================================================================
// 숨김 처리할 시스템/셸 프로세스
// 이 목록은 "선택 목록에 안 보이게"만 할 뿐, 직접 입력으로 추가하는 것까지 막지는 않는다
// (알고 하는 사용자의 판단은 존중 — 다만 explorer 등을 등록하면 시스템 전반에 영향이 생길 수 있음).
// ProcessWatcher가 "트레이 클릭으로 포커스가 explorer로 넘어간 순간"을 걸러내는 데도 사용됨.
// ================================================================

static const wchar_t* HIDDEN_PROCESSES[] = {
    L"explorer",       // 바탕화면/작업표시줄/탐색기 — 항상 포그라운드가 되어 리맵이 상시 발동하는 문제
    L"dwm",             // 데스크톱 창 관리자
    L"csrss",           // 클라이언트/서버 런타임
    L"wininit",
    L"winlogon",
    L"services",
    L"lsass",
    L"svchost",
    L"taskmgr",         // 작업 관리자 — 시스템 도구는 건드리지 않는 게 안전
    L"smss",
    L"autoime",         // 이 프로그램 자신 — 트레이 메뉴 클릭 시 순간적으로 포커스가
                        // 여기(숨김 메시지 윈도우)로 넘어오는 경우가 있어 제외
};
static const int HIDDEN_PROCESSES_COUNT = (int)(sizeof(HIDDEN_PROCESSES) / sizeof(HIDDEN_PROCESSES[0]));

static bool IsHiddenSystemProcess(const std::wstring& nameLower)
{
    for (int i = 0; i < HIDDEN_PROCESSES_COUNT; i++)
        if (nameLower == HIDDEN_PROCESSES[i])
            return true;
    return false;
}


// ================================================================
// 포커스 프로세스 감지
// ================================================================

struct ProcessWatcher
{
    HWND cachedHwnd    = NULL;
    std::wstring cachedExeName;   // 소문자, .exe 제거된 현재 포그라운드 프로세스 이름 (매칭 안 되어도 기록)
    TargetProfile* cachedProfile = NULL;

    // 트레이 아이콘을 클릭하면 포커스가 explorer(작업표시줄)로 순간 이동하는데,
    // 그 상태에서 GetForegroundWindow()를 부르면 방금까지 보던 프로그램이 아니라
    // explorer가 잡혀버린다. 이를 피하기 위해 "마지막으로 감지된, 숨김 목록에 없는
    // 유효한 포그라운드 프로세스"를 별도로 기억해 둔다.
    std::wstring lastValidExeName;
    TargetProfile* lastValidProfile = NULL;

    // 참고: HWND는 Windows 내부에서 재활용될 수 있으므로
    // 같은 HWND라도 다른 프로세스일 수 있다 (실용상 문제없는 수준).
    //
    // 현재 포그라운드 프로세스명을 반환하고, 매칭되는 프로필이 있으면 outProfile에 담는다.
    // 이 함수는 "지금 이 순간의" 포그라운드를 그대로 조회한다 — 키/마우스 훅처럼
    // 실시간 반응이 필요한 곳에서 사용.
    std::wstring GetCurrent(std::vector<TargetProfile>& profiles, TargetProfile** outProfile)
    {
        HWND hwnd = GetForegroundWindow();

        if (hwnd && hwnd == cachedHwnd)
        {
            *outProfile = cachedProfile;
            return cachedExeName;
        }

        cachedHwnd    = hwnd;
        cachedExeName.clear();
        cachedProfile = NULL;

        if (!hwnd) { *outProfile = NULL; return L""; }

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);

        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!h) { *outProfile = NULL; return L""; }

        wchar_t name[MAX_PATH] = {};
        GetModuleBaseNameW(h, NULL, name, MAX_PATH);
        CloseHandle(h);

        std::wstring exe = name;
        std::transform(exe.begin(), exe.end(), exe.begin(), towlower);
        if (exe.size() > 4 && exe.substr(exe.size() - 4) == L".exe")
            exe = exe.substr(0, exe.size() - 4);
        cachedExeName = exe;

        for (auto& p : profiles)
        {
            std::wstring pl = p.name;
            std::transform(pl.begin(), pl.end(), pl.begin(), towlower);
            // 정확히 일치할 때만 매칭한다. 예전에는 부분 문자열 포함 여부(양방향)로
            // 판단했는데, 프로세스 이름이 짧으면(예: 어떤 게임 런처가 "game", "app"
            // 같은 흔한 이름을 쓰는 경우) 등록하지 않은 프로그램이 우연히 매칭되어
            // 리맵/자동전환이 의도치 않게 걸리는 문제가 있었다. 프로그램 등록은
            // "실행 중 프로세스 목록에서 선택"하거나 정확한 이름을 직접 입력하는
            // 방식이라 정확 일치로도 실사용에 문제가 없다.
            if (exe == pl)
            {
                cachedProfile = &p;
                break;
            }
        }

        *outProfile = cachedProfile;

        // "마지막 유효 포그라운드"는 등록된 프로그램일 때만 갱신한다.
        // 임의의 다른 트레이 프로그램(카톡, 디스코드 등)을 우클릭할 때도
        // 순간적으로 포커스가 그쪽으로 넘어갈 수 있는데, 그런 미등록 프로그램까지
        // 다 유효값으로 인정하면 트레이 메뉴가 엉뚱한 프로그램을 계속 가리키게 된다.
        // 숨김 목록(explorer, autoime 등)뿐 아니라 미등록 프로그램도 lastValid 갱신에서 제외.
        if (cachedProfile)
        {
            lastValidExeName = exe;
            lastValidProfile = cachedProfile;
        }

        return cachedExeName;
    }

    // 트레이 메뉴처럼, 메뉴를 여는 그 순간엔 포커스가 explorer나 다른 트레이 프로그램으로
    // 넘어가 있을 수 있는 곳에서 사용.
    // 지금 포그라운드가 등록된 프로그램이 아니라면, 마지막으로 감지된 등록 프로그램
    // (lastValid*)을 대신 돌려준다.
    // 트레이 아이콘 표시도 이 함수를 그대로 사용한다 — "지금 이 순간의 진짜 상태"를
    // 좇으려던 시도(Alt+Tab, 트레이 클릭, 다른 트레이 앱 클릭 등 온갖 포커스 전환
    // 경로를 다 구분)가 계속 새로운 예외를 만들어내서, 아이콘도 메뉴와 동일하게
    // "마지막으로 확인된 등록 프로그램" 기준으로 단순하게 표시하기로 했다.
    std::wstring GetForTrayMenu(std::vector<TargetProfile>& profiles, TargetProfile** outProfile)
    {
        std::wstring cur = GetCurrent(profiles, outProfile);

        if (*outProfile)
            return cur;   // 지금 포그라운드가 이미 등록된 프로그램이면 그대로 사용

        // 지금은 미등록/숨김 프로세스(explorer, 다른 트레이 앱 등) → 마지막 유효값으로 대체
        *outProfile = lastValidProfile;
        return lastValidExeName;
    }

    // 프로필 목록이 바뀌었을 때 캐시 무효화
    void Invalidate()
    {
        cachedHwnd = NULL; cachedProfile = NULL; cachedExeName.clear();
        // lastValid*는 유지 — 프로필 목록이 바뀐 것과 별개로, 방금까지 보고 있던
        // 프로그램이 무엇이었는지는 그대로 기억해 둘 필요가 있다.
    }
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

    bool    dlgOpen         = false;   // 대상 프로그램 관리(ShowTargetDialog) 열림 여부
    bool    dlgProfileOpen  = false;   // 프로필 편집(ShowProfileDialog) — Target 다이얼로그 위에 중첩됨
    bool    dlgRemapOpen    = false;   // 리맵 키(ShowRemapDialog) — Profile 다이얼로그 위에 또 중첩됨
    bool    dlgIsTab        = false;  // true = Tab 설정, false = Capslock 설정 (리맵 키 다이얼로그용)
    DWORD   dlgCapturedVKey = 0;
    WNDPROC dlgOrigEditProc = NULL;
    std::wstring dlgProfileEditName;  // 프로필 편집 다이얼로그에서 편집 중인 프로그램 이름
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
// 아이콘 리소스: 1=활성(자동영어 ON), 2=비활성(자동영어 OFF)
// ================================================================

enum class TrayIconState { Active, Inactive };

static TrayIconState g_lastIconState = TrayIconState::Inactive;

void UpdateTrayIcon(TrayIconState state)
{
    if (state == g_lastIconState) return;   // 불필요한 갱신 방지

    int resId = (state == TrayIconState::Active) ? 1 : 2;
    HICON hIcon = LoadIcon(g_app.hInstance, MAKEINTRESOURCE(resId));
    if (!hIcon) return;   // 리소스 로딩 실패 시 기존 아이콘 유지 (깜빡임 방지)

    g_lastIconState = state;
    g_app.nid.hIcon  = hIcon;
    Shell_NotifyIcon(NIM_MODIFY, &g_app.nid);
}

// 마지막으로 감지된 등록 프로필(GetForTrayMenu 기준) 상태에 맞춰 트레이 아이콘을 갱신한다.
// 미등록 아이콘은 두지 않는다 — 등록/미등록을 실시간으로 정확히 구분하려던 시도가
// Alt+Tab, 트레이 클릭, 다른 트레이 앱 클릭 등 온갖 포커스 전환 경로마다 예외를
// 만들어내서 복잡도만 키웠다. 아이콘은 트레이 메뉴와 동일하게 "마지막으로 확인된
// 등록 프로그램" 기준으로 단순하게 표시한다. prof가 NULL이면(앱 시작 직후 등
// 아직 아무 등록 프로그램도 감지되지 않은 경우) 비활성으로 표시한다.
void RefreshTrayIconFromProfile(const TargetProfile* prof)
{
    if (prof && prof->autoEnglish)
        UpdateTrayIcon(TrayIconState::Active);
    else
        UpdateTrayIcon(TrayIconState::Inactive);
}

// 트레이 아이콘에 마우스를 올렸을 때 뜨는 툴팁을 갱신한다.
// "단축키 도우미 - <최근 작업한 프로그램명>" 형식. exeName이 비어 있으면
// (아직 아무 등록 프로그램도 감지되지 않음) 프로그램 이름만 표시한다.
// szTip은 최대 128자(구버전 셸 호환을 위해 64자로 안전하게 제한)이므로 넘치지 않게 자른다.
void RefreshTrayTooltip(const std::wstring& exeName)
{
    static std::wstring lastTip;   // 불필요한 Shell_NotifyIcon 호출 방지

    std::wstring tip = L"\ub2e8\ucd95\ud0a4 \ub3c4\uc6b0\ubbf8";   // "단축키 도우미"
    if (!exeName.empty())
        tip += L" - " + exeName;

    if (tip == lastTip) return;
    lastTip = tip;

    wcsncpy_s(g_app.nid.szTip, tip.c_str(), _TRUNCATE);
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
    return buf[0] ? buf : VKeyToString(vk);
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
            if (IsHiddenSystemProcess(name)) continue;   // 목록에는 노출하지 않음
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
    // 클래스명을 아톰이 아닌 문자열로 지정해야 하는 공용 컨트롤(SysListView32 등)용 오버로드
    void writeCtrl(DWORD style, DWORD exStyle, short x, short y, short cx, short cy,
                   WORD id, const wchar_t* clsName, const wchar_t* text)
    {
        align4();
        writeDW(style); writeDW(exStyle);
        writeW((WORD)x); writeW((WORD)y); writeW((WORD)cx); writeW((WORD)cy);
        writeW(id);
        writeStr(clsName);
        writeStr(text);
        writeW(0);
    }
    void writeHeader(WORD ctrlCount, short w, short h, const wchar_t* title, WORD fontSize = 9)
    {
        writeDW(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT);
        writeDW(0);
        writeW(ctrlCount);
        writeW(0); writeW(0); writeW((WORD)w); writeW((WORD)h);
        writeW(0); writeW(0);
        writeStr(title);
        writeW(fontSize);
        writeStr(L"\uad74\ub9bc");  // "굴림"
    }
};


// ================================================================
// 리맵 키 다이얼로그 (Capslock / Tab 공용)
// 콤보 맨 위: "비활성화 (차단)"
// 그 아래: F13~F24
// Edit: 직접 키 캡처
//
// 이 다이얼로그는 g_app.dlgProfileEditName 이 가리키는 프로필의
// remapVKey 혹은 tabVKey를 직접 수정한다.
// ================================================================

static const int COMBO_DISABLE_IDX = 0;

LRESULT CALLBACK EditKeyCapture(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
    {
        DWORD vk = (DWORD)wParam;

        if (vk == VK_RETURN) { SendMessage(GetParent(hwnd), WM_COMMAND, IDC_REMAP_OK,     0); return 0; }
        if (vk == VK_ESCAPE) { SendMessage(GetParent(hwnd), WM_COMMAND, IDC_REMAP_CANCEL, 0); return 0; }
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
            vk == VK_LWIN  || vk == VK_RWIN)
            return 0;

        g_app.dlgCapturedVKey = vk;

        HWND hCombo = GetDlgItem(GetParent(hwnd), IDC_REMAP_COMBO);
        SendMessage(hCombo, CB_SETCURSEL, (WPARAM)-1, 0);
        for (int i = 0; i < EXTENDED_KEYS_COUNT; i++)
        {
            if (EXTENDED_KEYS[i].vk == vk)
            {
                SendMessage(hCombo, CB_SETCURSEL, (WPARAM)(i + 1), 0);
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
        TargetProfile* p = g_app.config.Find(g_app.dlgProfileEditName);
        if (!p) { EndDialog(hDlg, IDCANCEL); return TRUE; }

        DWORD curVKey = g_app.dlgIsTab ? p->tabVKey : p->remapVKey;
        g_app.dlgCapturedVKey = curVKey;

        HWND hCombo = GetDlgItem(hDlg, IDC_REMAP_COMBO);
        HWND hEdit  = GetDlgItem(hDlg, IDC_REMAP_EDIT);

        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)L"\ucc28\ub2e8");  // "차단"
        if (curVKey == 0)
        {
            SendMessage(hCombo, CB_SETCURSEL, COMBO_DISABLE_IDX, 0);
            SetWindowTextW(hEdit, L"\ucc28\ub2e8");  // "차단"
        }

        for (int i = 0; i < EXTENDED_KEYS_COUNT; i++)
        {
            int idx = (int)SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)EXTENDED_KEYS[i].label);
            if (EXTENDED_KEYS[i].vk == curVKey)
            {
                SendMessage(hCombo, CB_SETCURSEL, (WPARAM)idx, 0);
                SetWindowTextW(hEdit, EXTENDED_KEYS[i].label);
            }
        }

        if (curVKey != 0 && SendMessage(hCombo, CB_GETCURSEL, 0, 0) == CB_ERR)
            SetWindowTextW(hEdit, GetKeyDisplayName(curVKey).c_str());

        g_app.dlgOrigEditProc = (WNDPROC)SetWindowLongPtr(
            hEdit, GWLP_WNDPROC, (LONG_PTR)EditKeyCapture);

        SetFocus(hEdit);
        return FALSE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_REMAP_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                HWND hCombo = GetDlgItem(hDlg, IDC_REMAP_COMBO);
                int idx = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0);
                HWND hEdit = GetDlgItem(hDlg, IDC_REMAP_EDIT);
                if (idx == COMBO_DISABLE_IDX)
                {
                    g_app.dlgCapturedVKey = 0;
                    SetWindowTextW(hEdit, L"\ucc28\ub2e8");  // "차단"
                }
                else
                {
                    int keyIdx = idx - 1;
                    if (keyIdx >= 0 && keyIdx < EXTENDED_KEYS_COUNT)
                    {
                        g_app.dlgCapturedVKey = EXTENDED_KEYS[keyIdx].vk;
                        SetWindowTextW(hEdit, EXTENDED_KEYS[keyIdx].label);
                    }
                }
            }
            break;

        case IDC_REMAP_OK:
        {
            TargetProfile* p = g_app.config.Find(g_app.dlgProfileEditName);
            if (p)
            {
                if (g_app.dlgIsTab) p->tabVKey    = g_app.dlgCapturedVKey;
                else                 p->remapVKey  = g_app.dlgCapturedVKey;
                g_app.config.Save();
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }

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

// profileName: 이 리맵 키를 수정할 대상 프로그램 이름
void ShowRemapDialog(HWND hwndParent, const std::wstring& profileName, bool isTab)
{
    if (g_app.dlgRemapOpen) return;
    g_app.dlgRemapOpen       = true;
    g_app.dlgIsTab           = isTab;
    g_app.dlgProfileEditName = profileName;

    wchar_t title[MAX_PATH + 32];
    swprintf_s(title, L"%s - %s \uc124\uc815", profileName.c_str(),
               isTab ? L"Tab" : L"Capslock");   // "<프로그램> - Capslock/Tab 설정"

    DlgBuf b;
    b.writeHeader(6, 200, 90, title);

    b.writeCtrl(WS_CHILD | WS_VISIBLE | SS_LEFT,
                5, 5, 190, 10, 0, 0x0082,
                L"\ubaa9\ub85d\uc5d0\uc11c \uc120\ud0dd\ud558\uac70\ub098 \ud0a4\ub97c \ub204\ub974\uc138\uc694:");

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
    g_app.dlgRemapOpen = false;
}


// ================================================================
// 프로필 편집 다이얼로그
// 프로그램 하나의 전체 설정(자동영어/Capslock리맵+키/Tab리맵+키)을 편집.
// 체크박스 3개 + 리맵 키는 별도 버튼으로 ShowRemapDialog를 다시 연다.
// ================================================================

static std::vector<TargetProfile> g_dlgProfiles;   // 대상 프로그램 관리 다이얼로그의 임시 목록

// 리스트뷰 컬럼 인덱스
enum { COL_NAME = 0, COL_AUTOENG = 1, COL_CAPSREMAP = 2, COL_TABREMAP = 3 };

// 켜짐/꺼짐 상태를 "● 텍스트" 형태로 표시. 원(●)은 NM_CUSTOMDRAW에서 초록/빨강으로 칠해진다.
// 리맵 항목은 켜져 있으면 원 뒤에 키 이름(또는 "차단")을 덧붙인다.
static std::wstring FormatCellText(bool on, DWORD vk = (DWORD)-1)
{
    std::wstring s = on ? L"\u25cf ON" : L"\u25cf OFF";   // ● ON / ● OFF
    if (on && vk != (DWORD)-1)
    {
        s = L"\u25cf ";
        s += (vk == 0) ? L"\ucc28\ub2e8" : GetKeyDisplayName(vk);   // "● 차단" / "● F13"
    }
    return s;
}

// 리스트뷰에 컬럼 4개를 구성한다 (최초 1회만 호출).
static void TargetList_SetupColumns(HWND hList)
{
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = (LPWSTR)L"\ud504\ub85c\uadf8\ub7a8\uba85";       // "프로그램명"
    col.cx = 90;
    ListView_InsertColumn(hList, COL_NAME, &col);

    col.pszText = (LPWSTR)L"\uc790\ub3d9 \uc601\uc5b4 \uc804\ud658"; // "자동 영어 전환"
    col.cx = 100;
    ListView_InsertColumn(hList, COL_AUTOENG, &col);

    col.pszText = (LPWSTR)L"CapsLock \ub9ac\ub9f5";                 // "CapsLock 리맵"
    col.cx = 100;
    ListView_InsertColumn(hList, COL_CAPSREMAP, &col);

    col.pszText = (LPWSTR)L"Tab \ub9ac\ub9f5";                      // "Tab 리맵"
    col.cx = 78;
    ListView_InsertColumn(hList, COL_TABREMAP, &col);
}

static void ProfileList_Refresh(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_TARGET_LIST);
    int  sel   = ListView_GetNextItem(hList, -1, LVNI_SELECTED);

    ListView_DeleteAllItems(hList);

    for (size_t i = 0; i < g_dlgProfiles.size(); i++)
    {
        const auto& p = g_dlgProfiles[i];

        LVITEMW item = {};
        item.mask    = LVIF_TEXT;
        item.iItem   = (int)i;
        item.iSubItem = COL_NAME;
        item.pszText  = (LPWSTR)p.name.c_str();
        ListView_InsertItem(hList, &item);

        std::wstring ae   = FormatCellText(p.autoEnglish);
        std::wstring caps = FormatCellText(p.remapEnabled, p.remapVKey);
        std::wstring tab  = FormatCellText(p.tabEnabled,   p.tabVKey);

        ListView_SetItemText(hList, (int)i, COL_AUTOENG,   (LPWSTR)ae.c_str());
        ListView_SetItemText(hList, (int)i, COL_CAPSREMAP, (LPWSTR)caps.c_str());
        ListView_SetItemText(hList, (int)i, COL_TABREMAP,  (LPWSTR)tab.c_str());
    }

    if (sel >= 0 && sel < (int)g_dlgProfiles.size())
        ListView_SetItemState(hList, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

static void ProcCombo_Refresh(HWND hDlg)
{
    HWND hCombo = GetDlgItem(hDlg, IDC_TARGET_PROC);
    SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
    for (const auto& p : GetRunningProcesses())
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)p.c_str());
}

INT_PTR CALLBACK ProfileDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        // g_dlgProfiles 안에서 편집 대상을 찾는다 (대상 프로그램 관리의 임시 목록 기준)
        TargetProfile* p = nullptr;
        for (auto& x : g_dlgProfiles)
            if (x.name == g_app.dlgProfileEditName) { p = &x; break; }
        if (!p) { EndDialog(hDlg, IDCANCEL); return TRUE; }

        wchar_t title[MAX_PATH + 16];
        swprintf_s(title, L"%s \uc124\uc815", p->name.c_str());  // "<이름> 설정"
        SetDlgItemTextW(hDlg, IDC_PF_TITLE, title);

        CheckDlgButton(hDlg, IDC_PF_AE_CHECK,    p->autoEnglish  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_PF_REMAP_CHECK, p->remapEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_PF_TAB_CHECK,   p->tabEnabled   ? BST_CHECKED : BST_UNCHECKED);

        SetDlgItemTextW(hDlg, IDC_PF_REMAP_COMBO,
            (L"Capslock \ub9ac\ub9f5 \ud0a4: " + (p->remapVKey == 0 ? L"\ucc28\ub2e8" : GetKeyDisplayName(p->remapVKey))).c_str());
        SetDlgItemTextW(hDlg, IDC_PF_TAB_COMBO,
            (L"Tab \ub9ac\ub9f5 \ud0a4: " + (p->tabVKey == 0 ? L"\ucc28\ub2e8" : GetKeyDisplayName(p->tabVKey))).c_str());
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_PF_REMAP_COMBO:
        {
            // 버튼처럼 사용: 눌리면 리맵 키 다이얼로그를 연다
            ShowRemapDialog(hDlg, g_app.dlgProfileEditName, false);
            // 다이얼로그가 g_app.config의 실제 프로필을 수정했으니, g_dlgProfiles에도 반영
            TargetProfile* real = g_app.config.Find(g_app.dlgProfileEditName);
            for (auto& x : g_dlgProfiles)
                if (x.name == g_app.dlgProfileEditName && real)
                    x.remapVKey = real->remapVKey;
            for (auto& x : g_dlgProfiles)
                if (x.name == g_app.dlgProfileEditName)
                    SetDlgItemTextW(hDlg, IDC_PF_REMAP_COMBO,
                        (L"Capslock \ub9ac\ub9f5 \ud0a4: " + (x.remapVKey == 0 ? L"\ucc28\ub2e8" : GetKeyDisplayName(x.remapVKey))).c_str());
            break;
        }

        case IDC_PF_TAB_COMBO:
        {
            ShowRemapDialog(hDlg, g_app.dlgProfileEditName, true);
            TargetProfile* real = g_app.config.Find(g_app.dlgProfileEditName);
            for (auto& x : g_dlgProfiles)
                if (x.name == g_app.dlgProfileEditName && real)
                    x.tabVKey = real->tabVKey;
            for (auto& x : g_dlgProfiles)
                if (x.name == g_app.dlgProfileEditName)
                    SetDlgItemTextW(hDlg, IDC_PF_TAB_COMBO,
                        (L"Tab \ub9ac\ub9f5 \ud0a4: " + (x.tabVKey == 0 ? L"\ucc28\ub2e8" : GetKeyDisplayName(x.tabVKey))).c_str());
            break;
        }

        case IDC_PF_OK:
        {
            for (auto& x : g_dlgProfiles)
            {
                if (x.name != g_app.dlgProfileEditName) continue;
                x.autoEnglish  = (IsDlgButtonChecked(hDlg, IDC_PF_AE_CHECK)    == BST_CHECKED);
                x.remapEnabled = (IsDlgButtonChecked(hDlg, IDC_PF_REMAP_CHECK) == BST_CHECKED);
                x.tabEnabled   = (IsDlgButtonChecked(hDlg, IDC_PF_TAB_CHECK)   == BST_CHECKED);
                break;
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        case IDC_PF_CANCEL:
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

void ShowProfileDialog(HWND hwndParent, const std::wstring& profileName)
{
    if (g_app.dlgProfileOpen) return;
    g_app.dlgProfileOpen     = true;
    g_app.dlgProfileEditName = profileName;

    DlgBuf b;
    // 컨트롤 8개: TITLE, CHECK*3, BUTTON(리맵키 콤보 대용)*2, 확인, 취소
    b.writeHeader(8, 210, 130, L"\ud504\ub85c\uadf8\ub7a8 \uc124\uc815");  // "프로그램 설정"

    b.writeCtrl(WS_CHILD | WS_VISIBLE | SS_LEFT,
                5, 5, 200, 12, IDC_PF_TITLE, 0x0082, L"");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                5, 22, 200, 12, IDC_PF_AE_CHECK, 0x0080,
                L"\uc790\ub3d9 \uc601\uc5b4 \uc804\ud658 \uc0ac\uc6a9");  // "자동 영어 전환 사용"

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                5, 40, 200, 12, IDC_PF_REMAP_CHECK, 0x0080,
                L"Capslock \ub9ac\ub9f5 \uc0ac\uc6a9");  // "Capslock 리맵 사용"

    // 리맵 키 버튼(콤보 자리 대신 버튼처럼 사용 — 누르면 별도 다이얼로그)
    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                20, 54, 175, 14, IDC_PF_REMAP_COMBO, 0x0080, L"Capslock \ub9ac\ub9f5 \ud0a4");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                5, 74, 200, 12, IDC_PF_TAB_CHECK, 0x0080,
                L"Tab \ub9ac\ub9f5 \uc0ac\uc6a9");  // "Tab 리맵 사용"

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                20, 88, 175, 14, IDC_PF_TAB_COMBO, 0x0080, L"Tab \ub9ac\ub9f5 \ud0a4");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                45, 108, 50, 14, IDC_PF_OK, 0x0080, L"\ud655\uc778");
    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                115, 108, 50, 14, IDC_PF_CANCEL, 0x0080, L"\ucde8\uc18c");

    DialogBoxIndirectParamW(g_app.hInstance, (LPCDLGTEMPLATEW)b.data,
                            hwndParent, ProfileDlgProc, 0);
    g_app.dlgProfileOpen = false;
}


// ================================================================
// 프로그램 관리 다이얼로그
// 등록된 프로그램 = 그 자체로 완전한 설정 프로필.
// 새로 추가하면 DefaultProfile()로 시작.
// ================================================================

// 목록에서 선택된 프로그램의 프로필 설정 다이얼로그를 연다.
// "프로그램 설정" 버튼과 리스트뷰 더블클릭(NM_DBLCLK) 양쪽에서 공용으로 사용.
static void OpenSelectedProfile(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_TARGET_LIST);
    int idx = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (idx >= 0 && idx < (int)g_dlgProfiles.size())
    {
        // 편집 중에는 g_app.config.profiles를 임시로 g_dlgProfiles와 동기화해서
        // ShowRemapDialog(내부적으로 g_app.config.Find 사용)가 올바로 동작하게 한다.
        // ProfileDlgProc은 g_dlgProfiles를 직접 수정하므로(체크박스/리맵키 전부),
        // 여기서 g_app.config.profiles → g_dlgProfiles로 되돌려쓰면 방금 한 편집이
        // 사라지니 그렇게 하지 않는다.
        g_app.config.profiles = g_dlgProfiles;
        ShowProfileDialog(hDlg, g_dlgProfiles[idx].name);
        ProfileList_Refresh(hDlg);
    }
    else
    {
        MessageBoxW(hDlg,
            L"\uba3c\uc800 \ubaa9\ub85d\uc5d0\uc11c \ud504\ub85c\uadf8\ub7a8\uc744 \uc120\ud0dd\ud574\uc8fc\uc138\uc694.",
            L"\ud504\ub85c\uadf8\ub7a8 \uc124\uc815", MB_ICONINFORMATION);
    }
}

INT_PTR CALLBACK TargetDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        HWND hList = GetDlgItem(hDlg, IDC_TARGET_LIST);
        ListView_SetExtendedListViewStyle(hList,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        TargetList_SetupColumns(hList);

        g_dlgProfiles = g_app.config.profiles;
        ProfileList_Refresh(hDlg);
        ProcCombo_Refresh(hDlg);

        // 선택된 항목이 없는 초기 상태에서는 삭제/프로그램 설정 버튼을 비활성화
        EnableWindow(GetDlgItem(hDlg, IDC_TARGET_DEL), FALSE);
        EnableWindow(GetDlgItem(hDlg, IDC_TARGET_EDITPROF), FALSE);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
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
            if (name.size() > 4 && name.substr(name.size() - 4) == L".exe")
                name = name.substr(0, name.size() - 4);

            bool exists = std::any_of(g_dlgProfiles.begin(), g_dlgProfiles.end(),
                [&](const TargetProfile& p) { return p.name == name; });

            if (!name.empty() && !exists)
            {
                g_dlgProfiles.push_back(TargetProfile::DefaultProfile(name));
                ProfileList_Refresh(hDlg);
                SetDlgItemTextW(hDlg, IDC_TARGET_EDIT, L"");
            }
            break;
        }

        case IDC_TARGET_DEL:
        {
            HWND hList = GetDlgItem(hDlg, IDC_TARGET_LIST);
            int idx = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (idx >= 0 && idx < (int)g_dlgProfiles.size())
            {
                wchar_t msg[MAX_PATH + 32];
                swprintf_s(msg, L"\uc815\ub9d0\ub85c \"%s\" \ud504\ub85c\uadf8\ub7a8\uc744 \uc0ad\uc81c\ud560\uae4c\uc694?",
                            g_dlgProfiles[idx].name.c_str());
                // "정말로 "<이름>" 프로그램을 삭제할까요?"
                int result = MessageBoxW(hDlg, msg,
                    L"\ud504\ub85c\uadf8\ub7a8 \uc0ad\uc81c",   // "프로그램 삭제"
                    MB_YESNO | MB_ICONQUESTION);
                if (result == IDYES)
                {
                    g_dlgProfiles.erase(g_dlgProfiles.begin() + idx);
                    ProfileList_Refresh(hDlg);
                }
            }
            break;
        }

        case IDC_TARGET_EDITPROF:
            OpenSelectedProfile(hDlg);
            break;

        case IDC_TARGET_OK:
            g_app.config.profiles = g_dlgProfiles;
            g_app.config.Save();
            g_app.watcher.Invalidate();
            EndDialog(hDlg, IDOK);
            return TRUE;

        case IDC_TARGET_CANCEL:
            // 편집 중 임시로 config.profiles를 건드렸을 수 있으니 원상복구
            g_app.config.Load();
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_NOTIFY:
    {
        NMHDR* nmhdr = (NMHDR*)lParam;

        if (nmhdr->idFrom == IDC_TARGET_LIST && nmhdr->code == NM_DBLCLK)
        {
            // 목록에서 항목을 더블클릭하면 그 프로그램의 설정 다이얼로그를 바로 연다.
            // 더블클릭 시점엔 이미 그 항목이 선택된 상태이므로 OpenSelectedProfile을
            // 그대로 재사용할 수 있다.
            OpenSelectedProfile(hDlg);
            return TRUE;
        }

        if (nmhdr->idFrom == IDC_TARGET_LIST && nmhdr->code == LVN_ITEMCHANGED)
        {
            // 선택 상태가 바뀔 때마다(선택/해제 모두 포함) 삭제/프로그램 설정
            // 버튼의 활성화 여부를 그 즉시 동기화한다.
            HWND hList = GetDlgItem(hDlg, IDC_TARGET_LIST);
            bool hasSelection = ListView_GetNextItem(hList, -1, LVNI_SELECTED) >= 0;
            EnableWindow(GetDlgItem(hDlg, IDC_TARGET_DEL), hasSelection);
            EnableWindow(GetDlgItem(hDlg, IDC_TARGET_EDITPROF), hasSelection);
            break;
        }

        if (nmhdr->idFrom == IDC_TARGET_LIST && nmhdr->code == NM_RCLICK)
        {
            // 우클릭한 위치의 항목을 선택 상태로 만든 뒤(우클릭이 항목 위였을 때만),
            // "프로그램 설정" / "삭제" 컨텍스트 메뉴를 띄운다.
            HWND hList = GetDlgItem(hDlg, IDC_TARGET_LIST);
            LPNMITEMACTIVATE ia = (LPNMITEMACTIVATE)lParam;

            if (ia->iItem >= 0)
            {
                ListView_SetItemState(hList, ia->iItem,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);

                POINT pt;
                GetCursorPos(&pt);

                HMENU hCtxMenu = CreatePopupMenu();
                AppendMenuW(hCtxMenu, MF_STRING, IDC_TARGET_EDITPROF,
                    L"\ud504\ub85c\uadf8\ub7a8 \uc124\uc815");   // "프로그램 설정"
                AppendMenuW(hCtxMenu, MF_STRING, IDC_TARGET_DEL,
                    L"\uc0ad\uc81c");                             // "삭제"

                int cmd = TrackPopupMenu(hCtxMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                          pt.x, pt.y, 0, hDlg, NULL);
                DestroyMenu(hCtxMenu);

                // 팝업 메뉴 항목 ID를 그대로 재사용했으므로, 기존 WM_COMMAND 핸들러와
                // 동일한 동작을 하도록 그쪽으로 전달한다.
                if (cmd == IDC_TARGET_EDITPROF || cmd == IDC_TARGET_DEL)
                    SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
            }
            break;
        }

        if (nmhdr->idFrom == IDC_TARGET_LIST && nmhdr->code == NM_CUSTOMDRAW)
        {
            NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lParam;
            switch (cd->nmcd.dwDrawStage)
            {
            case CDDS_PREPAINT:
                SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                return TRUE;

            case CDDS_ITEMPREPAINT:
                SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYSUBITEMDRAW);
                return TRUE;

            case (CDDS_ITEMPREPAINT | CDDS_SUBITEM):
            {
                int col = cd->iSubItem;
                if (col == COL_AUTOENG || col == COL_CAPSREMAP || col == COL_TABREMAP)
                {
                    int row = (int)cd->nmcd.dwItemSpec;
                    if (row >= 0 && row < (int)g_dlgProfiles.size())
                    {
                        const auto& p = g_dlgProfiles[row];
                        bool on = (col == COL_AUTOENG)   ? p.autoEnglish
                                : (col == COL_CAPSREMAP)  ? p.remapEnabled
                                :                            p.tabEnabled;

                        // 이 셀은 "● 텍스트" 형태다. ●만 초록/빨강으로 칠하고
                        // 나머지 텍스트는 기본 색으로 직접 그린 뒤, 리스트뷰의
                        // 기본 텍스트 그리기는 건너뛴다(CDRF_SKIPDEFAULT).
                        HWND hList = GetDlgItem(hDlg, IDC_TARGET_LIST);
                        wchar_t buf[64] = {};
                        ListView_GetItemText(hList, row, col, buf, 64);

                        RECT rc;
                        ListView_GetSubItemRect(hList, row, col, LVIR_LABEL, &rc);

                        HDC hdc = cd->nmcd.hdc;

                        // 이 셀도 행 전체 선택 하이라이트를 따라가도록, 선택 여부에 맞는
                        // 배경색을 먼저 채운다. FULLROWSELECT라도 커스텀드로우로 직접
                        // 그리는 서브아이템은 배경을 스스로 채워야 한다.
                        bool selected = (ListView_GetItemState(hList, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                        bool hasFocus = (GetFocus() == hList);
                        COLORREF bgColor = selected
                            ? GetSysColor(hasFocus ? COLOR_HIGHLIGHT : COLOR_BTNFACE)
                            : GetSysColor(COLOR_WINDOW);
                        HBRUSH hBrush = CreateSolidBrush(bgColor);
                        FillRect(hdc, &rc, hBrush);
                        DeleteObject(hBrush);

                        SetBkMode(hdc, TRANSPARENT);

                        // "● " 부분과 나머지 텍스트 부분을 분리해서 각각 그린다
                        std::wstring full = buf;
                        size_t spacePos = full.find(L' ');
                        std::wstring dot  = (spacePos != std::wstring::npos) ? full.substr(0, spacePos) : full;
                        std::wstring rest = (spacePos != std::wstring::npos) ? full.substr(spacePos)    : L"";

                        RECT rcDot = rc;
                        rcDot.left += 4;
                        // 원(●) 색은 켜짐/꺼짐을 그대로 보여줘야 하므로 선택 상태와 무관하게 유지
                        SetTextColor(hdc, on ? RGB(0, 150, 0) : RGB(200, 0, 0));
                        DrawTextW(hdc, dot.c_str(), (int)dot.size(), &rcDot,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);

                        SIZE dotSize = {};
                        GetTextExtentPoint32W(hdc, dot.c_str(), (int)dot.size(), &dotSize);

                        RECT rcRest = rc;
                        rcRest.left += 4 + dotSize.cx;
                        SetTextColor(hdc, selected && hasFocus
                            ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                            : GetSysColor(COLOR_WINDOWTEXT));
                        DrawTextW(hdc, rest.c_str(), (int)rest.size(), &rcRest,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);

                        SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                        return TRUE;
                    }
                }
                SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_DODEFAULT);
                return TRUE;
            }
            }
        }
        break;
    }

    case WM_CLOSE:
        g_app.config.Load();
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
    // 컨트롤 12개: STATIC, LISTVIEW, 삭제, 프로그램설정, STATIC, 새로고침, COMBOBOX,
    //             STATIC, EDIT, 추가, 확인, 취소
    b.writeHeader(12, 240, 195, L"\ub300\uc0c1 \ud504\ub85c\uadf8\ub7a8 \ub4f1\ub85d/\uc218\uc815");  // "대상 프로그램 등록/수정"

    b.writeCtrl(WS_CHILD | WS_VISIBLE | SS_LEFT,
                5, 5, 230, 10, 0, 0x0082,
                L"\ud504\ub85c\uadf8\ub7a8 \ubaa9\ub85d (\uac01\uc790 \ub3c5\ub9bd\uc801\uc778 \uc124\uc815\uc744 \uac00\uc9d0):");
    // "프로그램 목록 (각자 독립적인 설정을 가짐):"

    // 리스트뷰(표) — 프로그램명 | 자동 영어 전환 | CapsLock 리맵 | Tab 리맵
    // 창 폭이 줄어든 만큼 컬럼 폭도 TargetList_SetupColumns에서 함께 축소했다.
    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
                LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                WS_EX_CLIENTEDGE,
                5, 17, 230, 55, IDC_TARGET_LIST, L"SysListView32", L"");

    // 우측 정렬: [프로그램 설정(큼)] [삭제(작음)] — 자주 쓰는 동작을 크고 먼저,
    // 파괴적인 삭제는 작고 뒤에 두어 실수 클릭 가능성을 낮춘다.
    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                146, 75, 60, 14, IDC_TARGET_EDITPROF, 0x0080,
                L"\ud504\ub85c\uadf8\ub7a8 \uc124\uc815");  // "프로그램 설정"

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                208, 75, 27, 14, IDC_TARGET_DEL, 0x0080, L"\uc0ad\uc81c");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | SS_LEFT,
                5, 95, 105, 10, 0, 0x0082,
                L"\uc2e4\ud589 \uc911\uc778 \ud504\ub85c\uc138\uc2a4:");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                188, 93, 47, 14, IDC_TARGET_REFRESH, 0x0080, L"\uc0c8\ub85c\uace0\uce68");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                5, 108, 230, 120, IDC_TARGET_PROC, 0x0085, L"");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | SS_LEFT,
                5, 131, 41, 10, 0, 0x0082,
                L"\uc9c1\uc811 \uc785\ub825:");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                47, 129, 152, 14, IDC_TARGET_EDIT, 0x0081, L"");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                204, 129, 32, 14, IDC_TARGET_ADD, 0x0080, L"\ucd94\uac00");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                75, 168, 42, 14, IDC_TARGET_OK, 0x0080, L"\ud655\uc778");

    b.writeCtrl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                131, 168, 42, 14, IDC_TARGET_CANCEL, 0x0080, L"\ucde8\uc18c");

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
    bool  isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    bool  isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

    if (isDown || isUp)
    {
        TargetProfile* p = NULL;
        g_app.watcher.GetCurrent(g_app.config.profiles, &p);

        if (p)
        {
            if (kb->vkCode == VK_CAPITAL && p->remapEnabled)
            {
                if (p->remapVKey == 0) return 1;   // 비활성화: 차단

                INPUT inp      = {};
                inp.type       = INPUT_KEYBOARD;
                inp.ki.wVk     = (WORD)p->remapVKey;
                inp.ki.dwFlags = isUp ? KEYEVENTF_KEYUP : 0;
                SendInput(1, &inp, sizeof(INPUT));
                return 1;
            }

            if (kb->vkCode == VK_TAB && p->tabEnabled)
            {
                if (p->tabVKey == 0) return 1;

                INPUT inp      = {};
                inp.type       = INPUT_KEYBOARD;
                inp.ki.wVk     = (WORD)p->tabVKey;
                inp.ki.dwFlags = isUp ? KEYEVENTF_KEYUP : 0;
                SendInput(1, &inp, sizeof(INPUT));
                return 1;
            }
        }
    }

    return CallNextHookEx(g_app.kbdHook, nCode, wParam, lParam);
}

LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && wParam == WM_MOUSEMOVE)
    {
        POINT p;
        GetCursorPos(&p);

        if (p.x != g_app.lastPos.x || p.y != g_app.lastPos.y)
        {
            g_app.lastPos = p;

            TargetProfile* prof = NULL;
            g_app.watcher.GetCurrent(g_app.config.profiles, &prof);

            if (prof && prof->autoEnglish)
                ForceEnglish();

            // 아이콘/툴팁은 트레이 메뉴와 동일하게 "마지막으로 확인된 등록 프로그램"
            // 기준으로 표시한다(GetForTrayMenu). 지금 이 순간이 미등록 프로그램이거나
            // explorer라도 직전 등록 프로그램 상태를 그대로 유지한다.
            TargetProfile* iconProf = NULL;
            std::wstring iconExeName = g_app.watcher.GetForTrayMenu(g_app.config.profiles, &iconProf);
            RefreshTrayIconFromProfile(iconProf);
            RefreshTrayTooltip(iconProf ? iconExeName : L"");
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
// 등록된 프로그램일 때:
//   [프로그램명]
//   ─────────
//   ✓ 자동 영어 전환
//   ✓ Capslock 리맵 (F13)
//     Tab 리맵 (F14)
//   ─────────
//     설정 ▶
//   ─────────
//   종료
//
// 미등록 프로그램일 때:
//   [프로그램명] (미등록)
//   ─────────
//     자동 영어 전환 (회색, 비활성)
//     Capslock 리맵 (회색, 비활성)
//     Tab 리맵 (회색, 비활성)
//     + 이 프로그램 등록하기
//   ─────────
//     설정 ▶
//   ─────────
//   종료
// ================================================================

void ShowTrayMenu(HWND hwnd)
{
    TargetProfile* prof = NULL;
    std::wstring exeName = g_app.watcher.GetForTrayMenu(g_app.config.profiles, &prof);

    bool startupOn = Startup_IsRegistered();

    HMENU hMenu = CreatePopupMenu();

    // 상단: 지금 이 메뉴의 조작이 적용될 대상 프로그램 표시 — 클릭 불가능한 정보성 항목
    wchar_t headerLabel[MAX_PATH + 32];
    swprintf_s(headerLabel, L"\ud604\uc7ac \ud504\ub85c\uadf8\ub7a8: %s",
               prof ? exeName.c_str() : L"\uc5c6\uc74c");
    // "현재 프로그램: <이름>" / "현재 프로그램: 없음"
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_DISABLED, 0, headerLabel);
    InsertMenuW(hMenu, -1, MF_SEPARATOR, 0, NULL);

    if (prof)
    {
        // 등록된 프로그램: 토글이 이 프로그램의 실제 설정을 그대로 반영
        InsertMenuW(hMenu, -1,
            MF_BYPOSITION | (prof->autoEnglish ? MF_CHECKED : 0),
            ID_TOGGLE_IME, L"\uc790\ub3d9 \uc601\uc5b4 \uc804\ud658");

        wchar_t remapLabel[64];
        swprintf_s(remapLabel, L"Capslock \ub9ac\ub9f5 (%s)",
                   !prof->remapEnabled ? L"F13"
                   : prof->remapVKey == 0 ? L"\ucc28\ub2e8"
                   : GetKeyDisplayName(prof->remapVKey).c_str());
        InsertMenuW(hMenu, -1,
            MF_BYPOSITION | (prof->remapEnabled ? MF_CHECKED : 0),
            ID_TOGGLE_REMAP, remapLabel);

        wchar_t tabLabel[64];
        swprintf_s(tabLabel, L"Tab \ub9ac\ub9f5 (%s)",
                   !prof->tabEnabled ? L"\ucc28\ub2e8"
                   : prof->tabVKey == 0 ? L"\ucc28\ub2e8"
                   : GetKeyDisplayName(prof->tabVKey).c_str());
        InsertMenuW(hMenu, -1,
            MF_BYPOSITION | (prof->tabEnabled ? MF_CHECKED : 0),
            ID_TOGGLE_TAB, tabLabel);
    }
    else
    {
        // 등록된 적 없거나(앱 시작 직후) 아직 어떤 등록 프로그램도 감지되지 않은 상태:
        // 토글은 회색으로 비활성 표시. 프로그램 등록은 [대상 프로그램 등록/수정...]에서만 가능.
        InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_DISABLED, ID_TOGGLE_IME,
            L"\uc790\ub3d9 \uc601\uc5b4 \uc804\ud658");
        InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_DISABLED, ID_TOGGLE_REMAP,
            L"Capslock \ub9ac\ub9f5");
        InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_DISABLED, ID_TOGGLE_TAB,
            L"Tab \ub9ac\ub9f5");
    }

    InsertMenuW(hMenu, -1, MF_SEPARATOR, 0, NULL);

    // 서브메뉴 없이 메인 메뉴에 바로 노출
    InsertMenuW(hMenu, -1, MF_BYPOSITION, ID_MANAGE_TARGETS,
        L"\ub300\uc0c1 \ud504\ub85c\uadf8\ub7a8 \ub4f1\ub85d/\uc218\uc815...");  // "대상 프로그램 등록/수정..."
    InsertMenuW(hMenu, -1,
        MF_BYPOSITION | (startupOn ? MF_CHECKED : 0),
        ID_TOGGLE_STARTUP, L"\uc708\ub3c4\uc6b0 \uc2dc\uc791 \uc2dc \uc790\ub3d9 \uc2e4\ud589");  // "윈도우 시작 시 자동 실행"

    InsertMenuW(hMenu, -1, MF_SEPARATOR, 0, NULL);
    InsertMenuW(hMenu, -1, MF_BYPOSITION, ID_EXIT, L"\uc885\ub8cc");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);

    switch (cmd)
    {
    case ID_TOGGLE_IME:
        if (prof)
        {
            prof->autoEnglish = !prof->autoEnglish;
            g_app.config.Save();
            RefreshTrayIconFromProfile(prof);
        }
        break;

    case ID_TOGGLE_REMAP:
        if (prof) { prof->remapEnabled = !prof->remapEnabled; g_app.config.Save(); }
        break;

    case ID_TOGGLE_TAB:
        if (prof) { prof->tabEnabled = !prof->tabEnabled; g_app.config.Save(); }
        break;

    case ID_SET_REMAP_KEY:
        if (prof) ShowRemapDialog(hwnd, prof->name, false);
        break;

    case ID_SET_TAB_KEY:
        if (prof) ShowRemapDialog(hwnd, prof->name, true);
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
    static int clickCount = 0;

    if (msg == WM_TRAYICON)
    {
        if (lParam == WM_RBUTTONUP)
        {
            clickCount = 0;
            KillTimer(hwnd, TIMER_DBLCLK);

            ShowTrayMenu(hwnd);        // TrackPopupMenu가 메뉴 닫힐 때까지 블로킹
        }
        else if (lParam == WM_LBUTTONUP)
        {
            clickCount++;
            if (clickCount == 1)
            {
                SetTimer(hwnd, TIMER_DBLCLK, GetDoubleClickTime(), NULL);
            }
            else if (clickCount >= 2)
            {
                clickCount = 0;
                KillTimer(hwnd, TIMER_DBLCLK);

                // 더블클릭: 현재(또는 직전) 프로그램의 자동 영어 전환을 토글
                TargetProfile* prof = NULL;
                g_app.watcher.GetForTrayMenu(g_app.config.profiles, &prof);
                if (prof)
                {
                    prof->autoEnglish = !prof->autoEnglish;
                    g_app.config.Save();
                    RefreshTrayIconFromProfile(prof);
                }
            }
        }
    }
    else if (msg == WM_TIMER && wParam == TIMER_DBLCLK)
    {
        clickCount = 0;
        KillTimer(hwnd, TIMER_DBLCLK);
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}


// ================================================================
// WinMain
// ================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    // 중복 실행 방지
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"AutoIME_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(hMutex);
        return 0;
    }

    g_app.hInstance = hInstance;

    // 대상 프로그램 관리 화면의 리스트뷰(SysListView32) 사용을 위해 필요
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

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
                    L"\ub2e8\ucd95\ud0a4 \ub3c4\uc6b0\ubbf8", MB_ICONERROR);
        return 1;
    }

    auto& nid            = g_app.nid;
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_app.hwndMsg;
    nid.uID              = TRAYICON_ID;
    nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon            = LoadIcon(hInstance, MAKEINTRESOURCE(2));   // 시작 직후엔 비활성 상태로 표시
    wcscpy_s(nid.szTip, L"\ub2e8\ucd95\ud0a4 \ub3c4\uc6b0\ubbf8");    // "단축키 도우미" (실제 툴팁은 RefreshTrayTooltip이 매번 갱신)

    if (!Shell_NotifyIcon(NIM_ADD, &nid))
    {
        MessageBoxW(NULL, L"\ud2b8\ub808\uc774 \uc544\uc774\ucf58 \ub4f1\ub85d \uc2e4\ud328",
                    L"\ub2e8\ucd95\ud0a4 \ub3c4\uc6b0\ubbf8", MB_ICONERROR);
        return 1;
    }

    g_app.mouseHook = SetWindowsHookEx(WH_MOUSE_LL,    MouseProc,    NULL, 0);
    g_app.kbdHook   = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, NULL, 0);

    if (!g_app.mouseHook || !g_app.kbdHook)
        MessageBoxW(NULL, L"\ud6c5 \uc124\uce58 \uc2e4\ud328",
                    L"\ub2e8\ucd95\ud0a4 \ub3c4\uc6b0\ubbf8", MB_ICONWARNING);

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