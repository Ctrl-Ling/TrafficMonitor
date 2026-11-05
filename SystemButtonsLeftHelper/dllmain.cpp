#include "framework.h"
#include "SplitContract.h"
#include "TaskbarSplit.h"

//助手 DLL：被 TrafficMonitor 插件用 SetWindowsHookEx(WH_GETMESSAGE) 带进 explorer 的任务栏线程。
//它只干一件事：按注册表里的开关，把 XAML 排版器的 Arrange 换掉，让系统按钮钉在左边。
//
//通信全靠事件/消息，没有定时器：
//  插件改开关 -> 注册表变化 -> 监视线程醒来 -> 往任务栏线程投一条空消息 -> 消息钩子里干活 -> 回写状态并置事件。

static HMODULE g_module{};
static DWORD g_taskbar_tid{};
static std::atomic<bool> g_request_pending{ false };
static std::atomic<bool> g_first_pass_done{ false };
static std::atomic<bool> g_watcher_started{ false };
static HANDLE g_owner_wait{};             //等宿主（TrafficMonitor）进程退出
static DWORD g_watched_owner_pid{};
static CRITICAL_SECTION g_owner_section;

//把活推到任务栏线程上去做（XAML 对象只能在它自己的线程上碰）
static void WakeTaskbarThread()
{
    g_request_pending = true;
    if (g_taskbar_tid != 0)
        ::PostThreadMessageW(g_taskbar_tid, WM_NULL, 0, 0);
}

static void SetAckEvent()
{
    wchar_t name[256]{};
    if (!SplitRegReadString(kRegAckEvent, name, static_cast<DWORD>(sizeof(name) / sizeof(name[0]))))
        return;
    if (name[0] == L'\0')
        return;
    HANDLE event = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (event != nullptr)
    {
        ::SetEvent(event);
        ::CloseHandle(event);
    }
}

//监听宿主进程：TrafficMonitor 退出了，我们就自己复原，不给 explorer 留东西
static void WatchOwnerProcess(DWORD owner_pid)
{
    if (owner_pid == 0 || owner_pid == g_watched_owner_pid)
        return;

    ::EnterCriticalSection(&g_owner_section);
    if (g_owner_wait != nullptr)
    {
        ::UnregisterWait(g_owner_wait);
        g_owner_wait = nullptr;
    }
    g_watched_owner_pid = owner_pid;
    HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, owner_pid);
    if (process != nullptr)
    {
        if (!::RegisterWaitForSingleObject(&g_owner_wait, process,
            [](PVOID context, BOOLEAN timer_or_wait) {
                (void)context;
                (void)timer_or_wait;
                //宿主没了：先关掉开关，再把活推到任务栏线程去复原
                SplitRegWriteDword(kRegEnabled, 0u);
                WakeTaskbarThread();
            }, nullptr, INFINITE, WT_EXECUTEONLYONCE))
        {
            g_owner_wait = nullptr;
        }
        ::CloseHandle(process);
    }
    ::LeaveCriticalSection(&g_owner_section);
}

//已经落实过的开关状态：-1 表示还没动过。
//注册表键一变（包括我们自己写回去的上报值）监视线程就会叫醒一次，所以要认出“这次其实没新活”直接回话。
static std::atomic<int> g_applied_enabled{ -1 };

static void ProcessRequest()
{
    const DWORD enabled = SplitRegReadDword(kRegEnabled, 0);
    WatchOwnerProcess(SplitRegReadDword(kRegOwnerPid, 0));

    if (g_applied_enabled.load() != static_cast<int>(enabled))
    {
        g_applied_enabled = static_cast<int>(enabled);

        if (enabled != 0)
        {
            InstallTaskbarSplit();
            SetSplitEnabled(true);
        }
        else
        {
            SetSplitEnabled(false);
            RestoreMarginsAsync();
            RemoveTaskbarSplit();
        }

        //刚装好时按钮还没经过挂钩，这里只能是 0；真正钉住后由挂钩自己补发 active=1
        const bool working = IsTaskbarSplitWorking();
        SplitRegWriteDwordIfChanged(kRegActive, working ? 1u : 0u);
        SplitRegWriteDwordIfChanged(kRegPid, working ? ::GetCurrentProcessId() : 0u);
        if (!working)
            SplitRegWriteDwordIfChanged(kRegBlock, 0u);
    }

    //不管这轮有没有实际动作，都要把“请求序号”回给插件，否则插件会白等超时
    SplitRegWriteDwordIfChanged(kRegAckSeq, SplitRegReadDword(kRegReqSeq, 0));
    SetAckEvent();
}

//监视注册表开关：插件一改，这里就醒
static DWORD WINAPI WatchRegistryThread(LPVOID param)
{
    (void)param;
    HANDLE change_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (change_event == nullptr)
        return 0;

    for (;;)
    {
        HKEY key = SplitOpenRegKey(KEY_NOTIFY);
        if (key == nullptr)
            break;
        if (::RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_LAST_SET, change_event, TRUE) != ERROR_SUCCESS)
        {
            ::RegCloseKey(key);
            break;
        }
        ::WaitForSingleObject(change_event, INFINITE);
        ::RegCloseKey(key);
        WakeTaskbarThread();
    }

    ::CloseHandle(change_event);
    return 0;
}

static void EnsureWatcherStarted()
{
    if (g_watcher_started.exchange(true))
        return;
    HANDLE thread = ::CreateThread(nullptr, 0, WatchRegistryThread, nullptr, 0, nullptr);
    if (thread != nullptr)
        ::CloseHandle(thread);
}

//SetWindowsHookEx 用的消息钩子：任务栏线程每取一条消息就走到这里一次
extern "C" __declspec(dllexport) LRESULT CALLBACK TaskbarSplitMessageHook(int code, WPARAM w_param, LPARAM l_param)
{
    if (code == HC_ACTION)
    {
        g_taskbar_tid = ::GetCurrentThreadId();
        if (!g_first_pass_done.exchange(true) || g_request_pending.exchange(false))
        {
            EnsureWatcherStarted();
            ProcessRequest();
        }
    }
    return ::CallNextHookEx(nullptr, code, w_param, l_param);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        ::InitializeCriticalSection(&g_owner_section);
        ::DisableThreadLibraryCalls(module);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        //必须先把写进 explorer 的跳转还原掉，否则模块没了、跳转还指着这块地址
        RestoreDetourBytes();
        SplitRegWriteDword(kRegActive, 0u);
        SplitRegWriteDword(kRegBlock, 0u);
        ::DeleteCriticalSection(&g_owner_section);
    }
    return TRUE;
}
