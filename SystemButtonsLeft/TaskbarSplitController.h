// Author: Ctrl-Ling
// Last Modified By: Ctrl-Ling
// Last Modified At: 2026-11-09 17:39:27 +08:00
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

class ITrafficMonitor;

// 当前状态，用于在插件选项对话框里显示
struct SplitStatus
{
    bool windows11{ false };        //是否 Win11 任务栏（拆分只在 Win11 有意义）
    bool host_is_x64{ false };      //TrafficMonitor 是不是 64 位（32 位进程无法把 DLL 注入 64 位 explorer）
    bool explorer_is_x64{ true };   //explorer.exe 是不是 64 位
    bool helper_exists{ false };    //SystemButtonsLeftHelper.dll 是否存在
    bool helper_injected{ false };  //插件是否已经把钩子挂上
    bool split_active{ false };     //助手已经在 explorer 里挂钩成功
    int block_width{ 0 };           //被钉在左侧的系统按钮块宽度（DIP）
    int block_width_100{ 0 };       //同一个数的原始精度：按钮块右边界，1/100 DIP（插件摆 TM 任务栏窗口时用它，避免每次截断丢 1 像素）
    int helper_step{ 0 };           //助手走到哪一步了（kStep*），0 表示当前 explorer 里没有助手在跑
    bool taskbar_center_align{ true };   //Windows 自己的“任务栏图标对齐”当前值
};

// “win按钮居左，软件图标居中”的宿主端控制器（运行在 TrafficMonitor 进程里）。
// 它做三件事：
//   1）把自己的开关状态通过注册表告诉 explorer 里的助手 DLL；
//   2）把助手 DLL 用 SetWindowsHookEx(WH_GETMESSAGE) 注入 explorer 的任务栏线程；
//   3）按助手的实际情况切换 Windows 的 TaskbarAl（助手可用时保持“居中”，不可用时退回“靠左”）。
//   4）把助手报回来的按钮块边界交给 CTaskbarWndAnchor，让 TrafficMonitor 自己的任务栏窗口
//     紧跟在这个块后面——这一步完全在插件里做，主程序代码不用改。
class CTaskbarSplitController
{
public:
    static CTaskbarSplitController& Instance();

    //同时把主程序接口转交给窗口锚定模块（它要靠这个接口拿到 TM 自己的任务栏窗口句柄和 DPI）
    void SetApp(ITrafficMonitor* app);
    void SetConfigFile(const std::wstring& ini_path);

    //读取插件自己的 ini，并把保存下来的开关状态应用到系统上（TM 启动时调用一次）
    void LoadConfigAndApply();
    bool IsEnabled() const { return m_enabled; }
    //选项对话框点“确定”后调用；返回 false 表示没能按预期生效（msg 里带原因）
    bool Apply(bool enable, std::wstring& msg);

    void RefreshStatus();
    const SplitStatus& Status() const { return m_status; }
    //助手在 explorer 里把状态写回注册表后，这边跟着对齐（不轮询，靠注册表变化通知）
    void StartWatchingHelperState();
    void StopWatchingHelperState();
    //最近一次应用的结果说明（用于在选项对话框里显示）
    const std::wstring& LastMessage() const { return m_last_msg; }

    ~CTaskbarSplitController();

private:
    CTaskbarSplitController();
    CTaskbarSplitController(const CTaskbarSplitController&) = delete;
    CTaskbarSplitController& operator=(const CTaskbarSplitController&) = delete;

    bool InjectHelper(std::wstring& msg);       //把助手 DLL 带进 explorer
    void EjectHelper();                         //通知助手复原，然后摘钩子、卸载模块
    bool WaitForHelperAck(DWORD timeout_ms);    //等助手把这一轮请求做完（序号对上了才算完）

    void PublishEnabledState(bool enabled);
    void SwitchTaskbarAlignment(bool enable);
    std::wstring GetHelperDllPath() const;

    static DWORD WINAPI StateWatchThreadEntry(LPVOID param);
    void OnHelperStateChanged();

    ITrafficMonitor* m_app{};
    std::wstring m_ini_file;
    bool m_enabled{ false };
    int m_prev_alignment{ 1 };      //启用插件之前 Windows 的对齐方式（1:居中 0:靠左）
    HMODULE m_helper_module{};
    HHOOK m_hook{};
    HANDLE m_ack_event{};
    HANDLE m_state_event{};     //注册表变化 -> 助手上报了新状态
    HANDLE m_stop_event{};      //让监视线程退出
    HANDLE m_watch_thread{};
    DWORD m_req_seq{};          //当前请求序号（和注册表里的 TaskbarSplitReqSeq 一致）
    SplitStatus m_status{};
    std::wstring m_last_msg;
};
