#include "framework.h"
#include <cstring>
#include <shlwapi.h>
#include "TaskbarSplitController.h"
#include "PluginInterface.h"
#include "SplitContract.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")

//插件 DLL 自己的模块句柄（dllmain.cpp 里赋值），用它定位同目录下的助手 DLL
extern HMODULE g_plugin_module;

//Win11 任务栏图标对齐方式的注册表位置（1:居中，0:靠左）
static constexpr const wchar_t* kExplorerAdvancedPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
static constexpr const wchar_t* kTaskbarAlValueName = L"TaskbarAl";
//插件与助手之间用来“等对方干完活”的事件名（同一会话内，explorer 也能打开它）
static constexpr const wchar_t* kAckEventName = L"Local\\TrafficMonitor.TaskbarSplit.Ack";

static bool IsWindows11OrLater()
{
    bool win11 = false;
    HMODULE ntdll = ::LoadLibraryW(L"ntdll.dll");
    if (ntdll != nullptr)
    {
        typedef void(WINAPI* pfRtlGetNtVersionNumbers)(DWORD*, DWORD*, DWORD*);
        auto func = reinterpret_cast<pfRtlGetNtVersionNumbers>(::GetProcAddress(ntdll, "RtlGetNtVersionNumbers"));
        if (func != nullptr)
        {
            DWORD major{}, minor{}, build{};
            func(&major, &minor, &build);
            build &= 0x0ffff;
            win11 = (major > 10) || (major == 10 && build >= 22000);
        }
        ::FreeLibrary(ntdll);
    }
    return win11;
}

static DWORD ReadTaskbarAl()
{
    HKEY key{};
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kExplorerAdvancedPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return 1;
    DWORD value{}, size = sizeof(value), type{};
    LONG ret = ::RegQueryValueExW(key, kTaskbarAlValueName, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    ::RegCloseKey(key);
    if (ret != ERROR_SUCCESS || type != REG_DWORD)
        return 1;   //Windows 默认就是居中
    return (value != 0) ? 1 : 0;
}

//写 TaskbarAl 并通知 explorer 立刻生效（和 Windows 设置面板里改“任务栏对齐方式”等价）
static void WriteTaskbarAl(bool center_align)
{
    HKEY key{};
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kExplorerAdvancedPath, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        DWORD value = center_align ? 1u : 0u;
        ::RegSetValueExW(key, kTaskbarAlValueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        ::RegCloseKey(key);
    }
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"TraySettings"),
        SMTO_NORMAL | SMTO_ABORTIFHUNG, 2000, nullptr);
}

CTaskbarSplitController::CTaskbarSplitController()
{
    m_ack_event = ::CreateEventW(nullptr, TRUE, FALSE, kAckEventName);
    if (m_ack_event == nullptr)
        m_ack_event = ::OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, kAckEventName);
}

CTaskbarSplitController::~CTaskbarSplitController()
{
    //进程退出时不主动摘钩子：助手 DLL 在自己被 explorer 卸载时（DllMain 的 DLL_PROCESS_DETACH）
    //会把写进 explorer 的跳转还原掉，所以这里只需要关掉本地句柄。
    StopWatchingHelperState();
    if (m_ack_event != nullptr)
    {
        ::CloseHandle(m_ack_event);
        m_ack_event = nullptr;
    }
}

CTaskbarSplitController& CTaskbarSplitController::Instance()
{
    static CTaskbarSplitController instance;
    return instance;
}

void CTaskbarSplitController::SetConfigFile(const std::wstring& ini_path)
{
    m_ini_file = ini_path;
}

std::wstring CTaskbarSplitController::GetHelperDllPath() const
{
    //助手 DLL 不是 TM 插件，不能和插件放在同一个 plugins 目录里：
    //TM 的“插件管理”会把该目录下每个 dll 当插件加载，去找 TMPluginGetInstance，
    //找不到就在列表里显示“函数获取失败，故障代码：127”。所以正式位置是 TrafficMonitor.exe 同级目录。
    //老版本（放在 plugins 里）继续兼容，方便就地升级。
    std::vector<std::wstring> dirs;

    //1）主程序所在目录（正式位置）
    wchar_t exe_path[MAX_PATH]{};
    if (::GetModuleFileNameW(nullptr, exe_path, MAX_PATH) != 0)
    {
        std::wstring path(exe_path);
        size_t pos = path.find_last_of(L'\\');
        if (pos != std::wstring::npos)
            dirs.push_back(path.substr(0, pos + 1));
    }

    //2）插件自己所在目录，以及它的上一级（老布局 / 绿色目录随便怎么摆）
    if (g_plugin_module != nullptr)
    {
        wchar_t path[MAX_PATH]{};
        if (::GetModuleFileNameW(g_plugin_module, path, MAX_PATH) != 0)
        {
            std::wstring full(path);
            size_t pos = full.find_last_of(L'\\');
            if (pos != std::wstring::npos)
            {
                std::wstring dir = full.substr(0, pos + 1);
                dirs.push_back(dir);
                std::wstring parent = dir.substr(0, dir.size() - 1);   //去掉结尾的 '\'
                size_t up = parent.find_last_of(L'\\');
                if (up != std::wstring::npos)
                    dirs.push_back(parent.substr(0, up + 1));
            }
        }
    }

    for (const std::wstring& dir : dirs)
    {
        std::wstring candidate = dir + kHelperDllName;
        if (::PathFileExistsW(candidate.c_str()))
            return candidate;
    }
    //一个都没找到：返回主程序目录下的那个名字，让提示里说出的路径是“应该放的位置”
    return dirs.empty() ? std::wstring(kHelperDllName) : dirs[0] + kHelperDllName;
}

void CTaskbarSplitController::LoadConfigAndApply()
{
    if (m_ini_file.empty())
        return;
    m_enabled = (::GetPrivateProfileIntW(L"settings", L"enabled", 0, m_ini_file.c_str()) != 0);
    m_prev_alignment = static_cast<int>(::GetPrivateProfileIntW(L"settings", L"prev_alignment", 1, m_ini_file.c_str()));

    if (!m_enabled)
    {
        //没开就什么都不做（如果上次退出时助手还挂着，它会因为宿主进程消失而自己复原）
        PublishEnabledState(false);
        RefreshStatus();
        return;
    }

    std::wstring msg;
    if (!Apply(true, msg) && m_app != nullptr && !msg.empty())
        m_app->ShowNotifyMessage(msg.c_str());   //启动时没弄成，弹一条提示，别让用户以为插件没反应
}

void CTaskbarSplitController::PublishEnabledState(bool enabled)
{
    //每次改动都让请求序号 +1，助手处理完会把同样的序号写到 AckSeq，插件据此判断“这一轮真的做完了”
    m_req_seq = SplitRegReadDword(kRegReqSeq, 0) + 1;
    SplitRegWriteDword(kRegReqSeq, m_req_seq);
    SplitRegWriteDword(kRegEnabled, enabled ? 1u : 0u);
    SplitRegWriteDword(kRegOwnerPid, ::GetCurrentProcessId());
    if (m_ack_event != nullptr)
        SplitRegWriteString(kRegAckEvent, kAckEventName);
}

bool CTaskbarSplitController::InjectHelper(std::wstring& msg)
{
#ifndef _WIN64
    msg = L"TrafficMonitor 是 32 位版本，无法把助手 DLL 注入 64 位的 explorer.exe，请改用 64 位版本。";
    return false;
#else
    std::wstring dll_path = GetHelperDllPath();
    if (dll_path.empty() || !::PathFileExistsW(dll_path.c_str()))
    {
        msg = L"找不到 " + std::wstring(kHelperDllName) + L"，把它放在 TrafficMonitor.exe 同一个目录（不要放进 plugins，否则会被当成插件加载并报错）。";
        return false;
    }

    HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray == nullptr)
    {
        msg = L"没有发现任务栏窗口（Shell_TrayWnd）。";
        return false;
    }
    DWORD explorer_pid{};
    DWORD explorer_tid = ::GetWindowThreadProcessId(tray, &explorer_pid);
    if (explorer_tid == 0 || explorer_pid == 0)
    {
        msg = L"取不到任务栏所在线程。";
        return false;
    }

    //位数必须一致，否则 LoadLibrary 到 explorer 里会失败
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, explorer_pid);
    if (process != nullptr)
    {
        BOOL wow64{};
        if (::IsWow64Process(process, &wow64) && wow64)
        {
            ::CloseHandle(process);
            msg = L"explorer.exe 是 32 位，助手 DLL 是 64 位，位数不一致。";
            return false;
        }
        ::CloseHandle(process);
    }

    m_helper_module = ::LoadLibraryExW(dll_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (m_helper_module == nullptr)
    {
        msg = L"加载 " + std::wstring(kHelperDllName) + L" 失败（错误码 " + std::to_wstring(::GetLastError()) + L"）。";
        return false;
    }
    FARPROC hook_proc = ::GetProcAddress(m_helper_module, kHelperHookProcName);
    if (hook_proc == nullptr)
    {
        msg = L"助手 DLL 里找不到导出函数 \"" + std::wstring(kHelperHookProcName, kHelperHookProcName + strlen(kHelperHookProcName)) + L"\"。";
        ::FreeLibrary(m_helper_module);
        m_helper_module = nullptr;
        return false;
    }

    PublishEnabledState(true);
    m_hook = ::SetWindowsHookExW(WH_GETMESSAGE, reinterpret_cast<HOOKPROC>(hook_proc), m_helper_module, explorer_tid);
    if (m_hook == nullptr)
    {
        msg = L"SetWindowsHookEx 失败（错误码 " + std::to_wstring(::GetLastError()) + L"）。";
        ::FreeLibrary(m_helper_module);
        m_helper_module = nullptr;
        return false;
    }
    //钩子只有在目标线程“取一条消息”时才会把 DLL 带进去，这里给它发一条空消息
    ::PostMessageW(tray, WM_NULL, 0, 0);
    ::PostThreadMessageW(explorer_tid, WM_NULL, 0, 0);
    return true;
#endif
}

bool CTaskbarSplitController::WaitForHelperAck(DWORD timeout_ms)
{
    if (m_ack_event == nullptr)
        return false;
    DWORD start = ::GetTickCount();
    //助手处理完会把 AckSeq 写成当前的请求序号；事件只是“叫醒我一下”，最终以注册表里的序号为准
    for (;;)
    {
        if (SplitRegReadDword(kRegAckSeq, 0) == m_req_seq)
            return true;
        DWORD elapsed = ::GetTickCount() - start;
        if (elapsed >= timeout_ms)
            return false;
        ::ResetEvent(m_ack_event);
        //复位之后再读一次：万一助手刚好在“写序号”和“置事件”之间，置位被我们抹掉了，这里能补回来
        if (SplitRegReadDword(kRegAckSeq, 0) == m_req_seq)
            return true;
        DWORD wait = ::WaitForSingleObject(m_ack_event, timeout_ms - elapsed);
        if (wait == WAIT_TIMEOUT && SplitRegReadDword(kRegAckSeq, 0) != m_req_seq)
            return false;
    }
}

void CTaskbarSplitController::EjectHelper()
{
    if (m_hook == nullptr && m_helper_module == nullptr)
        return;       //什么都没装上，不用等助手确认
    //顺序很重要：先告诉助手把写进 explorer 代码段的跳转还原，再摘钩子、再卸载模块。
    //（即使这里没等到助手的确认，助手也会在 explorer 卸载它自己的那次 DllMain 里完成还原。）
    PublishEnabledState(false);       //这里也要让序号 +1，否则下面等到的可能是上一轮的确认
    WaitForHelperAck(3000);
    if (m_hook != nullptr)
    {
        ::UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
    }
    if (m_helper_module != nullptr)
    {
        ::FreeLibrary(m_helper_module);
        m_helper_module = nullptr;
    }
}

void CTaskbarSplitController::SwitchTaskbarAlignment(bool enable)
{
    if (!m_status.windows11)
        return;
    if (enable)
    {
        if (m_status.split_active)
        {
            //助手真的在钉 -> Windows 保持“居中”，系统按钮由助手搬到左边，其他图标仍然居中
            WriteTaskbarAl(true);
        }
        else if (m_status.helper_injected)
        {
            //助手已经注入进 explorer，只是还没证明自己能钉住（槽位不对、还没排版过一次等）。
            //这时候绝不能改成“靠左”：那会让所有应用图标跟着一起靠左，用户看到的是“越开越坏”。
            //保持 Windows 现在的样子，等 OnHelperStateChanged 收到 active=1 再切“居中”。
            WriteTaskbarAl(m_prev_alignment != 0);
        }
        else
        {
            //助手根本装不上（dll 缺失、位数不对、钩子失败）-> 退回 Windows 自带的“靠左”
            WriteTaskbarAl(false);
        }
    }
    else
    {
        WriteTaskbarAl(m_prev_alignment != 0);
    }
}

bool CTaskbarSplitController::Apply(bool enable, std::wstring& msg)
{
    m_enabled = enable;
    RefreshStatus();
    bool inject_ok = false;

    if (enable)
    {
        //记下用户原本的对齐方式，取消勾选时好还原
        m_prev_alignment = m_status.taskbar_center_align ? 1 : 0;

        EjectHelper();
        inject_ok = InjectHelper(msg);
        if (inject_ok)
            WaitForHelperAck(3000);       //等助手装好挂钩并回话
        RefreshStatus();
        //能钉住就保持“居中”（由助手把按钮挪到左边），钉不住才退回系统自带的“靠左”。
        //写 TaskbarAl 会广播 TraySettings，任务栏重新排版一次，助手也就能证明自己真的生效了。
        SwitchTaskbarAlignment(true);
        StartWatchingHelperState();       //助手随后上报状态时，这边跟着对齐
    }
    else
    {
        StopWatchingHelperState();
        PublishEnabledState(false);
        //先还原对齐（会触发一次重新排版），助手在这次排版里把自己加过的负边距撤掉，然后才摘钩子
        SwitchTaskbarAlignment(false);
        EjectHelper();
    }

    RefreshStatus();

    //保存插件自己的设置
    if (!m_ini_file.empty())
    {
        ::WritePrivateProfileStringW(L"settings", L"enabled", enable ? L"1" : L"0", m_ini_file.c_str());
        std::wstring prev = std::to_wstring(m_prev_alignment);
        ::WritePrivateProfileStringW(L"settings", L"prev_alignment", prev.c_str(), m_ini_file.c_str());
    }

    if (enable && !inject_ok)
    {
        //上面 SwitchTaskbarAlignment(true) 已经按“助手没生效”处理过了：
        //Win11 上退回系统自带的“靠左”（应用图标会跟着一起靠左），Win10 上不动系统设置。
        if (msg.empty())
            msg = L"助手没能注入 explorer。";
        if (m_status.windows11)
            msg += L"已退回 Windows 自带的“靠左”（应用图标也会一起靠左）。";
        else
            msg += L"本插件只在 Windows 11 上有意义，未改动系统设置。";
        m_last_msg = msg;
        return false;
    }
    if (enable && !m_status.split_active)
    {
        m_last_msg = L"助手已注入，等任务栏排一次版才真正钉住；这期间不动 Windows 的对齐设置。"
            L"下面“诊断”一行会说明助手卡在哪一步。";
        msg = m_last_msg;
        return true;
    }
    if (enable)
        m_last_msg = L"已启用：系统按钮靠左，应用图标居中。";
    else
        m_last_msg = L"未启用：使用 Windows 默认布局。";
    return true;
}

void CTaskbarSplitController::RefreshStatus()
{
    m_status.windows11 = IsWindows11OrLater();
    m_status.host_is_x64 = (sizeof(void*) == 8);
    m_status.helper_injected = (m_hook != nullptr);

    std::wstring helper_path = GetHelperDllPath();
    m_status.helper_exists = (!helper_path.empty() && ::PathFileExistsW(helper_path.c_str()));

    DWORD active = SplitRegReadDword(kRegActive, 0);
    DWORD helper_pid = SplitRegReadDword(kRegPid, 0);
    HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD tray_pid{};
    if (tray != nullptr)
        ::GetWindowThreadProcessId(tray, &tray_pid);
    //助手上报的进程必须就是当前任务栏所在进程，否则 explorer 已经重启过、状态是旧的
    m_status.split_active = (active != 0 && helper_pid != 0 && tray_pid != 0 && helper_pid == tray_pid);
    m_status.block_width = m_status.split_active ? static_cast<int>(SplitRegReadDword(kRegBlock, 0) / 100) : 0;
    //诊断码只有“当前这个 explorer 里的助手”写过的才算数：进程号对不上就是 explorer 重启后留下的旧值
    m_status.helper_step = (helper_pid != 0 && helper_pid == tray_pid)
        ? static_cast<int>(SplitRegReadDword(kRegStep, 0)) : 0;

    DWORD al = ReadTaskbarAl();
    m_status.taskbar_center_align = (al != 0);
}

//监听助手写回来的状态：助手在 explorer 里真正钉好按钮后会把 TaskbarSplitActive 写进注册表，
//这边收到变化就把 Windows 的对齐方式跟一手（能钉住就保持“居中”，钉不住才退回“靠左”）。
//只在 HKCU\Software\TrafficMonitor 上等待通知，不碰 TaskbarAl 那个键，所以不会自己叫醒自己。
void CTaskbarSplitController::StartWatchingHelperState()
{
    if (m_watch_thread != nullptr)
        return;
    if (m_state_event == nullptr)
        m_state_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_stop_event == nullptr)
        m_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_state_event == nullptr || m_stop_event == nullptr)
        return;
    m_watch_thread = ::CreateThread(nullptr, 0, StateWatchThreadEntry, this, 0, nullptr);
}

void CTaskbarSplitController::StopWatchingHelperState()
{
    if (m_watch_thread != nullptr)
    {
        ::SetEvent(m_stop_event);
        ::WaitForSingleObject(m_watch_thread, 2000);
        ::CloseHandle(m_watch_thread);
        m_watch_thread = nullptr;
    }
    if (m_state_event != nullptr)
    {
        ::CloseHandle(m_state_event);
        m_state_event = nullptr;
    }
    if (m_stop_event != nullptr)
    {
        ::CloseHandle(m_stop_event);
        m_stop_event = nullptr;
    }
}

DWORD WINAPI CTaskbarSplitController::StateWatchThreadEntry(LPVOID param)
{
    auto self = static_cast<CTaskbarSplitController*>(param);
    for (;;)
    {
        HKEY key = SplitOpenRegKey(KEY_NOTIFY);
        if (key == nullptr)
            break;
        if (::RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_LAST_SET, self->m_state_event, TRUE) != ERROR_SUCCESS)
        {
            ::RegCloseKey(key);
            break;
        }
        HANDLE handles[2] = { self->m_state_event, self->m_stop_event };
        DWORD wait = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        ::RegCloseKey(key);
        if (wait == WAIT_OBJECT_0 + 1)
            break;          //要求退出
        if (wait == WAIT_OBJECT_0)
            self->OnHelperStateChanged();
    }
    return 0;
}

void CTaskbarSplitController::OnHelperStateChanged()
{
    RefreshStatus();
    if (!m_enabled)
        return;
    //助手证明自己钉住了 -> 把 Windows 切到“居中”（这是拆分布局的前提）。
    //反过来什么都不做：助手掉线或自检没过时绝不能改成“靠左”，那会让所有应用图标一起靠左，
    //比保持原样更糟；这种情况只在“诊断”一行里说清楚卡在哪。
    if (m_status.split_active && !m_status.taskbar_center_align)
        WriteTaskbarAl(true);
}
