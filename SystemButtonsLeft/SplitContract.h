#pragma once
#include <windows.h>

//// “win按钮居左，软件图标居中”插件与它注入到 explorer.exe 的助手 DLL 之间的约定。
//// 两边只通过注册表这一个通道通信（TM 插件 <-> explorer 里的助手 DLL 不在同一个进程）。
//
// 谁写谁读：
//   插件（在 TrafficMonitor 进程里）写：TaskbarSplitEnabled、TaskbarSplitOwner、TaskbarSplitAckEvent、TaskbarSplitReqSeq
//   助手（在 explorer.exe 里）写：TaskbarSplitActive、TaskbarSplitPid、TaskbarSplitBlock、TaskbarSplitClasses、TaskbarSplitAckSeq
//   主程序（TrafficMonitor.exe）读：TaskbarSplitActive、TaskbarSplitBlock —— 只用来把任务栏窗口摆在按钮块后面

// 注册表位置
static constexpr const wchar_t* kSplitRegPath = L"Software\\TrafficMonitor";

// 插件 -> 助手
static constexpr const wchar_t* kRegEnabled = L"TaskbarSplitEnabled";   //REG_DWORD 1：把系统按钮钉在左侧
static constexpr const wchar_t* kRegOwnerPid = L"TaskbarSplitOwner";    //REG_DWORD 插件宿主（TrafficMonitor）的进程ID，宿主退出时助手自行复原
static constexpr const wchar_t* kRegAckEvent = L"TaskbarSplitAckEvent"; //REG_SZ 助手完成安装/复原后置事件的名字，插件等它，不轮询
static constexpr const wchar_t* kRegReqSeq = L"TaskbarSplitReqSeq";     //REG_DWORD 请求序号：插件每次改动开关就 +1

// 助手 -> 插件、主程序
static constexpr const wchar_t* kRegAckSeq = L"TaskbarSplitAckSeq";     //REG_DWORD 助手已经处理完的请求序号（等于 kRegReqSeq 表示这一轮做完了）
static constexpr const wchar_t* kRegActive = L"TaskbarSplitActive";     //REG_DWORD 1：助手已在当前 explorer.exe 里挂钩成功
static constexpr const wchar_t* kRegPid = L"TaskbarSplitPid";           //REG_DWORD 助手所在进程（explorer）的ID
static constexpr const wchar_t* kRegBlock = L"TaskbarSplitBlock";       //REG_DWORD 左侧系统按钮块的宽度，单位 1/100 DIP
static constexpr const wchar_t* kRegClasses = L"TaskbarSplitClasses";   //REG_SZ 调试信息：助手在任务栏上看到的控件类型名

// 助手 DLL 的文件名（与插件 DLL 放在同一个 plugins 目录下）
static constexpr const wchar_t* kHelperDllName = L"SystemButtonsLeftHelper.dll";
// 助手 DLL 里供 SetWindowsHookEx 使用的钩子函数名
static constexpr const char* kHelperHookProcName = "TaskbarSplitMessageHook";

// 打开（必要时创建）约定用的注册表键，返回的句柄由调用方关闭。
// 先按只读/通知权限试着打开（RegCreateKeyEx 要求写入权限，用它来拿 KEY_NOTIFY 会失败），
// 打不开再创建——这样插件和助手谁先跑都能把这个键建出来。
inline HKEY SplitOpenRegKey(DWORD access)
{
    HKEY key{};
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kSplitRegPath, 0, access, &key) == ERROR_SUCCESS)
        return key;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kSplitRegPath, 0, nullptr, REG_OPTION_NON_VOLATILE, access, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return nullptr;
    return key;
}

// 小的注册表工具（两个工程共用本头文件）。都不带 key 参数：每次都自己开关 kSplitRegPath，
// 因为这些值只在“勾选/取消、挂钩/复原”时写，频率极低。
inline DWORD SplitRegReadDword(const wchar_t* name, DWORD default_value)
{
    HKEY h_key{};
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kSplitRegPath, 0, KEY_READ, &h_key) != ERROR_SUCCESS)
        return default_value;
    DWORD value{};
    DWORD size = sizeof(value);
    DWORD type{};
    LONG ret = ::RegQueryValueExW(h_key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    ::RegCloseKey(h_key);
    if (ret != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value))
        return default_value;
    return value;
}

inline bool SplitRegWriteDword(const wchar_t* name, DWORD value)
{
    HKEY h_key = SplitOpenRegKey(KEY_SET_VALUE);
    if (h_key == nullptr)
        return false;
    bool ok = (::RegSetValueExW(h_key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS);
    ::RegCloseKey(h_key);
    return ok;
}

//值真的变了才写。两边都用 RegNotifyChangeKeyValue 监视同一个键，
//写自己已经写过的值会把自己再叫醒一次，形成空转，所以助手这边的上报一律走这个函数。
inline bool SplitRegWriteDwordIfChanged(const wchar_t* name, DWORD value)
{
    if (SplitRegReadDword(name, 0xFFFFFFFFu) == value)
        return true;
    return SplitRegWriteDword(name, value);
}

inline bool SplitRegReadString(const wchar_t* name, wchar_t* buffer, DWORD buffer_chars)
{
    HKEY h_key{};
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kSplitRegPath, 0, KEY_READ, &h_key) != ERROR_SUCCESS)
        return false;
    DWORD size = buffer_chars * sizeof(wchar_t);
    DWORD type{};
    buffer[0] = L'\0';
    LONG ret = ::RegQueryValueExW(h_key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size);
    ::RegCloseKey(h_key);
    return (ret == ERROR_SUCCESS && type == REG_SZ);
}

inline bool SplitRegWriteString(const wchar_t* name, const wchar_t* value)
{
    HKEY h_key = SplitOpenRegKey(KEY_SET_VALUE);
    if (h_key == nullptr)
        return false;
    DWORD bytes = static_cast<DWORD>((::wcslen(value) + 1) * sizeof(wchar_t));
    bool ok = (::RegSetValueExW(h_key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value), bytes) == ERROR_SUCCESS);
    ::RegCloseKey(h_key);
    return ok;
}


