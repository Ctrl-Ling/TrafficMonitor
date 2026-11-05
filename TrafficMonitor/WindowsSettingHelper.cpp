#include "stdafx.h"
#include "WindowsSettingHelper.h"
#include "TrafficMonitor.h"

bool CWindowsSettingHelper::m_light_theme{};
bool CWindowsSettingHelper::IsWindows10LightTheme()
{
    return m_light_theme;
}

void CWindowsSettingHelper::CheckWindows10LightTheme()
{
    if (theApp.m_win_version.IsWindows10OrLater())
    {
        HKEY hKey;
        DWORD dwThemeData(0);
        LONG lRes = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey);
        if (lRes == ERROR_SUCCESS) {
            GetDWORDRegKeyData(hKey, L"SystemUsesLightTheme", dwThemeData);
            m_light_theme = (dwThemeData != 0);
        }
        else
        {
            m_light_theme = false;
        }
        RegCloseKey(hKey);
    }
    else
    {
        m_light_theme = false;
    }
}

bool CWindowsSettingHelper::IsDotNetFramework4Point5Installed()
{
    DWORD netFramewordRelease{};
    if (!GetDWORDRegKeyData(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\NET Framework Setup\\NDP\\v4\\Full", L"Release", netFramewordRelease))
        return false;
    return netFramewordRelease >= 379893;
}

bool CWindowsSettingHelper::IsTaskbarShowingInAllDisplays()
{
    DWORD data{};
    if (!GetDWORDRegKeyData(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"MMTaskbarEnabled", data))
        return true;
    return data != 0;
}

bool CWindowsSettingHelper::IsTaskbarWidgetsBtnShown()
{
    if (!theApp.m_taskbar_data.is_windows_web_experience_detected)
        return false;
    DWORD data{};
    if (!GetDWORDRegKeyData(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"TaskbarDa", data))
        return true;
    return data != 0;
}

bool CWindowsSettingHelper::IsTaskbarCenterAlign()
{
    DWORD data{};
    if (!GetDWORDRegKeyData(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"TaskbarAl", data))
        return true;
    return data != 0;
}

bool CWindowsSettingHelper::SetTaskbarCenterAlign(bool center_align)
{
    CRegKey key;
    if (key.Open(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", KEY_SET_VALUE) != ERROR_SUCCESS)
        return false;
    DWORD value = center_align ? 1 : 0;
    if (key.SetDWORDValue(L"TaskbarAl", value) != ERROR_SUCCESS)
        return false;
    key.Close();
    // 通知资源管理器立即应用任务栏对齐方式的更改
    DWORD_PTR result{};
    ::SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"TraySettings"),
        SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 500, &result);
    return true;
}

//任务栏拆分模块（TaskbarSplitMod）是否正在当前资源管理器中生效
//模块把开始、搜索、多任务视图按钮固定在任务栏左侧，同时保持图标对齐方式为“居中”
bool CWindowsSettingHelper::IsTaskbarSplitActive()
{
    DWORD active{};
    if (!GetDWORDRegKeyData(HKEY_CURRENT_USER, L"Software\\TrafficMonitor", L"TaskbarSplitActive", active) || active == 0)
        return false;

    //模块运行在资源管理器进程中，资源管理器重启后旧的进程ID就会失效
    DWORD pid{};
    if (!GetDWORDRegKeyData(HKEY_CURRENT_USER, L"Software\\TrafficMonitor", L"TaskbarSplitPid", pid) || pid == 0)
        return false;
    HWND h_taskbar = ::FindWindow(L"Shell_TrayWnd", nullptr);
    if (h_taskbar == nullptr)
        return false;
    DWORD taskbar_pid{};
    ::GetWindowThreadProcessId(h_taskbar, &taskbar_pid);
    return taskbar_pid != 0 && taskbar_pid == pid;
}

//模块固定在左侧的按钮块宽度（DIP），还没有拿到值时返回0
int CWindowsSettingHelper::GetTaskbarSplitBlockWidth()
{
    DWORD block{};
    if (!GetDWORDRegKeyData(HKEY_CURRENT_USER, L"Software\\TrafficMonitor", L"TaskbarSplitBlock", block))
        return 0;
    //模块以 1/100 DIP 为单位上报
    return static_cast<int>(block / 100);
}

//把“win按钮居左，软件图标居中”选项的状态告诉任务栏拆分模块
void CWindowsSettingHelper::SetTaskbarSplitEnabled(bool enabled)
{
    CRegKey key;
    if (key.Create(HKEY_CURRENT_USER, L"Software\\TrafficMonitor") != ERROR_SUCCESS)
        return;
    key.SetDWORDValue(L"TaskbarSplitEnabled", enabled ? 1u : 0u);
}

LONG CWindowsSettingHelper::GetDWORDRegKeyData(HKEY hKey, const wstring& strValueName, DWORD& dwValueData)
{
    DWORD dwBufferSize(sizeof(DWORD));
    DWORD dwResult(0);
    LONG lError = ::RegQueryValueExW(hKey, strValueName.c_str(), NULL, NULL, reinterpret_cast<LPBYTE>(&dwResult), &dwBufferSize);
    if (lError == ERROR_SUCCESS)
        dwValueData = dwResult;
    return lError;
}

bool CWindowsSettingHelper::GetDWORDRegKeyData(HKEY keyParent, const wstring& strKeyName, const wstring& strValueName, DWORD& dwValueData)
{
    CRegKey key;
    if (key.Open(keyParent, strKeyName.c_str(), KEY_READ) != ERROR_SUCCESS)
        return false;
    return (key.QueryDWORDValue(strValueName.c_str(), dwValueData) == ERROR_SUCCESS);
}
