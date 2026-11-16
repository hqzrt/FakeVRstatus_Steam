#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <shobjidl.h>
#include <string>
#include <vector>
#include <regex>
#include <filesystem>
#include <cstdio>
#include "driver_dll.h"

namespace fs = std::filesystem;
using std::string; using std::wstring;

static const wchar_t *kTitle = L"FakeVR";
static const char *kManifest =
    "{\n  \"alwaysActivate\": false,\n  \"name\": \"fakehmd\",\n  \"directory\": \"\",\n"
    "  \"resourceOnly\": false,\n  \"hmd_presence\": [ \"*.*\" ]\n}\n";

static const wchar_t *kHide[] = { L"vrmonitor.exe", L"vrcompositor.exe" };
static const wchar_t *kAll[]  = { L"vrserver.exe", L"vrmonitor.exe", L"vrcompositor.exe",
                                  L"vrdashboard.exe", L"vrwebhelper.exe", L"vrstartup.exe" };

static wstring W(const string &s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w;
}
static bool ReadAll(const fs::path &p, string &out)
{
    FILE *f = _wfopen(p.c_str(), L"rb"); if (!f) return false;
    out.clear(); char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f); return true;
}
static bool WriteAll(const fs::path &p, const void *data, size_t len)
{
    FILE *f = _wfopen(p.c_str(), L"wb"); if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len; fclose(f); return ok;
}
static bool Exists(const fs::path &p) { std::error_code ec; return fs::exists(p, ec); }
static int Ask(const wstring &text, UINT flags = MB_YESNO | MB_ICONQUESTION)
{ return MessageBoxW(nullptr, text.c_str(), kTitle, flags | MB_SETFOREGROUND); }
static void Info(const wstring &text, UINT icon = MB_ICONINFORMATION)
{ MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | icon | MB_SETFOREGROUND); }

static fs::path SelfPath()
{ wchar_t b[MAX_PATH * 2]; GetModuleFileNameW(nullptr, b, MAX_PATH * 2); return fs::path(b); }
static fs::path AppDir()
{ wchar_t *p = nullptr; fs::path r; if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) r = fs::path(p) / L"FakeVR"; CoTaskMemFree(p); return r; }
static fs::path AppExe() { return AppDir() / L"FakeVR.exe"; }
static fs::path Desktop()
{ wchar_t *p = nullptr; fs::path r; if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) r = p; CoTaskMemFree(p); return r; }
static bool SamePath(const fs::path &a, const fs::path &b)
{ return _wcsicmp(a.lexically_normal().c_str(), b.lexically_normal().c_str()) == 0; }

static std::vector<DWORD> FindPids(const wchar_t *const *names, int n)
{
    std::vector<DWORD> r;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return r;
    PROCESSENTRY32W e; e.dwSize = sizeof e;
    if (Process32FirstW(s, &e)) do {
        for (int i = 0; i < n; i++) if (_wcsicmp(e.szExeFile, names[i]) == 0) { r.push_back(e.th32ProcessID); break; }
    } while (Process32NextW(s, &e));
    CloseHandle(s); return r;
}
static bool VrRunning() { return !FindPids(kAll, 3).empty(); }
static void StopVR()
{
    for (DWORD pid : FindPids(kAll, 6)) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) { TerminateProcess(h, 0); CloseHandle(h); }
    }
}
static BOOL CALLBACK HideProc(HWND h, LPARAM l)
{
    auto *pids = reinterpret_cast<std::vector<DWORD> *>(l);
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    for (DWORD p : *pids) if (p == pid && IsWindowVisible(h)) { ShowWindow(h, SW_HIDE); break; }
    return TRUE;
}
static void Squeeze()
{
    for (DWORD pid : FindPids(kAll, 6)) {
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!h) continue;
        SetPriorityClass(h, IDLE_PRIORITY_CLASS); EmptyWorkingSet(h); CloseHandle(h);
    }
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static fs::path SteamDir()
{
    wchar_t buf[1024]; DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return {};
    wstring s = buf; for (auto &c : s) if (c == L'/') c = L'\\';
    return fs::path(s);
}
static fs::path SteamVRDir(const fs::path &steam)
{
    std::vector<fs::path> libs{ steam };
    string vdf;
    if (ReadAll(steam / L"steamapps" / L"libraryfolders.vdf", vdf)) {
        std::regex re("\"path\"\\s+\"([^\"]+)\"");
        for (auto it = std::sregex_iterator(vdf.begin(), vdf.end(), re); it != std::sregex_iterator(); ++it) {
            string p = (*it)[1].str(), u;
            for (size_t i = 0; i < p.size(); i++) { if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') i++; u += p[i]; }
            libs.push_back(fs::path(W(u)));
        }
    }
    for (auto &l : libs) { fs::path vr = l / L"steamapps" / L"common" / L"SteamVR"; if (Exists(vr / L"bin")) return vr; }
    return {};
}

static void Backup(const fs::path &f)
{ fs::path b = f; b += L".bak"; if (!Exists(b)) CopyFileW(f.c_str(), b.c_str(), TRUE); }

static bool PatchKeys(const fs::path &f, const std::vector<std::pair<string, string>> &kv)
{
    string t; if (!ReadAll(f, t)) return false;
    string orig = t;
    for (auto &p : kv)
        t = std::regex_replace(t, std::regex("(\"" + p.first + "\"\\s*:\\s*)[^,\\r\\n}]+"), "$1" + p.second);
    if (t == orig) return true;
    Backup(f); return WriteAll(f, t.data(), t.size());
}
static bool PatchUser(const fs::path &f, const string &forced, bool nullEnable)
{
    string t; if (!ReadAll(f, t)) return true;
    string orig = t;
    t = std::regex_replace(t, std::regex("(\"forcedDriver\"\\s*:\\s*)\"[^\"]*\""), "$1\"" + forced + "\"");
    t = std::regex_replace(t, std::regex("(\"requireHmd\"\\s*:\\s*)(true|false)"), "$1true");
    t = std::regex_replace(t, std::regex("(\"driver_null\"\\s*:\\s*\\{[^}]*?\"enable\"\\s*:\\s*)(true|false)"),
                           string("$1") + (nullEnable ? "true" : "false"));
    if (t == orig) return true;
    Backup(f); return WriteAll(f, t.data(), t.size());
}

static wstring InstallDriver()
{
    fs::path steam = SteamDir();
    if (steam.empty()) return L"Не нашёл Steam. Он установлен?";
    fs::path vr = SteamVRDir(steam);
    if (vr.empty()) return L"Не нашёл SteamVR. Установи его из библиотеки Steam (он бесплатный).";

    std::error_code ec;
    fs::path drv = vr / L"drivers" / L"fakehmd";
    fs::create_directories(drv / L"bin" / L"win64", ec);
    if (!WriteAll(drv / L"driver.vrdrivermanifest", kManifest, strlen(kManifest)) ||
        !WriteAll(drv / L"bin" / L"win64" / L"driver_fakehmd.dll", driver_fakehmd_dll, driver_fakehmd_dll_len))
        return L"Не получилось записать драйвер в папку SteamVR:\n" + drv.wstring() +
               L"\n\nПопробуй запустить FakeVR.exe от имени администратора.";

    PatchKeys(vr / L"drivers" / L"null" / L"resources" / L"settings" / L"default.vrsettings", { { "enable", "false" } });
    PatchKeys(vr / L"resources" / L"settings" / L"default.vrsettings", {
        { "requireHmd", "true" }, { "forcedDriver", "\"fakehmd\"" }, { "activateMultipleDrivers", "true" },
        { "pauseCompositorOnStandby", "false" }, { "turnOffScreensTimeout", "999999" },
        { "enableHomeApp", "false" }, { "motionSmoothing", "false" }, { "showMirrorView", "false" } });
    PatchUser(steam / L"config" / L"steamvr.vrsettings", "fakehmd", false);
    return L"";
}

static bool DriverLooksInstalled()
{
    fs::path steam = SteamDir(); if (steam.empty()) return false;
    fs::path vr = SteamVRDir(steam); if (vr.empty()) return false;
    if (!Exists(vr / L"drivers" / L"fakehmd" / L"bin" / L"win64" / L"driver_fakehmd.dll")) return false;
    string t;
    if (ReadAll(vr / L"resources" / L"settings" / L"default.vrsettings", t) &&
        !std::regex_search(t, std::regex("\"forcedDriver\"\\s*:\\s*\"fakehmd\""))) return false;
    if (ReadAll(steam / L"config" / L"steamvr.vrsettings", t) &&
        std::regex_search(t, std::regex("\"forcedDriver\"\\s*:\\s*\"(?!fakehmd\")[^\"]+\""))) return false;
    return true;
}

static void MakeShortcut(const fs::path &lnk, const fs::path &target, const wchar_t *args)
{
    IShellLinkW *sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void **)&sl))) return;
    sl->SetPath(target.c_str()); sl->SetArguments(args);
    sl->SetWorkingDirectory(target.parent_path().c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile *pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void **)&pf))) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
    sl->Release();
}
static const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static void SetAutostart(bool on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wstring v = L"\"" + AppExe().wstring() + L"\" --autostart";
        RegSetValueExW(k, L"FakeVR", 0, REG_SZ, (const BYTE *)v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, L"FakeVR");
    RegCloseKey(k);
}
static const wchar_t *kLnkOn = L"VR статус ВКЛ.lnk";
static const wchar_t *kLnkOff = L"VR статус ВЫКЛ.lnk";

static void RunLauncher(bool autostart)
{
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"FakeVR_Launcher_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return;
    if (autostart) Sleep(20000);

    if (!VrRunning() && !DriverLooksInstalled()) InstallDriver();

    ShellExecuteW(nullptr, L"open", L"steam://run/250820", nullptr, nullptr, SW_SHOWNORMAL);
    bool started = false; DWORD t0 = GetTickCount(), lastSq = 0;
    for (;;) {
        std::vector<DWORD> pids = FindPids(kHide, 2);
        if (!pids.empty()) {
            if (!started) { started = true; t0 = GetTickCount(); }
            EnumWindows(HideProc, reinterpret_cast<LPARAM>(&pids));
        } else if (started) {
            if (FindPids(kAll, 1).empty()) break;
        } else if (GetTickCount() - t0 > 300000) break;

        DWORD age = GetTickCount() - t0;
        if (started && age > 20000 && GetTickCount() - lastSq > 60000) { Squeeze(); lastSq = GetTickCount(); }
        Sleep(age < 90000 ? 500 : 3000);
    }
    if (mtx) CloseHandle(mtx);
}

static bool EnsureVrClosed()
{
    if (!VrRunning()) return true;
    if (Ask(L"SteamVR сейчас запущен. Закрыть его, чтобы продолжить?") != IDYES) return false;
    StopVR(); Sleep(2500); return true;
}

static void Install()
{
    if (!EnsureVrClosed()) return;
    wstring err = InstallDriver();
    if (!err.empty()) { Info(err, MB_ICONERROR); return; }

    std::error_code ec;
    fs::create_directories(AppDir(), ec);
    fs::path self = SelfPath(), app = AppExe();
    if (!SamePath(self, app)) {
        const wchar_t *me[] = { L"FakeVR.exe" };
        for (DWORD pid : FindPids(me, 1)) {
            if (pid == GetCurrentProcessId()) continue;
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (h) { TerminateProcess(h, 0); CloseHandle(h); }
        }
        for (int i = 0; i < 10 && !CopyFileW(self.c_str(), app.c_str(), FALSE); i++) Sleep(300);
    }
    fs::path desk = Desktop();
    MakeShortcut(desk / kLnkOn, app, L"");
    MakeShortcut(desk / kLnkOff, app, L"--stop");

    SetAutostart(Ask(L"Драйвер установлен, на рабочем столе появились ярлыки «VR статус ВКЛ» и «VR статус ВЫКЛ».\n\n"
                     L"Включать VR-статус автоматически при входе в Windows?") == IDYES);

    if (Ask(L"Готово! Включить VR-статус прямо сейчас?") == IDYES)
        ShellExecuteW(nullptr, L"open", app.c_str(), nullptr, app.parent_path().c_str(), SW_SHOWNORMAL);
}

static void Uninstall()
{
    if (Ask(L"Удалить FakeVR и вернуть настройки SteamVR как было?", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    StopVR(); Sleep(2000);
    std::error_code ec;
    fs::path steam = SteamDir();
    fs::path vr = steam.empty() ? fs::path() : SteamVRDir(steam);
    if (!vr.empty()) {
        fs::remove_all(vr / L"drivers" / L"fakehmd", ec);
        for (fs::path f : { vr / L"drivers" / L"null" / L"resources" / L"settings" / L"default.vrsettings",
                            vr / L"resources" / L"settings" / L"default.vrsettings" }) {
            fs::path b = f; b += L".bak";
            if (Exists(b)) { CopyFileW(b.c_str(), f.c_str(), FALSE); DeleteFileW(b.c_str()); }
        }
    }
    if (!steam.empty()) PatchUser(steam / L"config" / L"steamvr.vrsettings", "", false);
    SetAutostart(false);
    fs::path desk = Desktop();
    DeleteFileW((desk / kLnkOn).c_str()); DeleteFileW((desk / kLnkOff).c_str());

    fs::path app = AppExe(), self = SelfPath();
    if (SamePath(self, app)) {
        wstring cmd = L"cmd.exe /c timeout /t 2 /nobreak >nul & rmdir /s /q \"" + AppDir().wstring() + L"\"";
        STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
    } else fs::remove_all(AppDir(), ec);
    Info(L"FakeVR удалён, настройки SteamVR восстановлены.");
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wstring args = cmd ? cmd : L"";

    if (args.find(L"--stop") != wstring::npos) { StopVR(); return 0; }
    if (args.find(L"--uninstall") != wstring::npos) { Uninstall(); return 0; }
    if (args.find(L"--autostart") != wstring::npos) { RunLauncher(true); return 0; }

    if (SamePath(SelfPath(), AppExe())) { RunLauncher(false); return 0; }

    if (Exists(AppExe())) {
        int r = Ask(L"FakeVR уже установлен.\n\nДа — переустановить / починить\nНет — удалить\nОтмена — выйти",
                    MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDYES) Install(); else if (r == IDNO) Uninstall();
    } else if (Ask(L"Установить FakeVR?\n\nОн поставит виртуальный шлем в SteamVR, чтобы в Steam горел статус «Online using VR».") == IDYES) {
        Install();
    }
    return 0;
}
