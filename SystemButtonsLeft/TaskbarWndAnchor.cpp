// Author: Ctrl-Ling
// Last Modified By: Ctrl-Ling
// Last Modified At: 2026-11-09 17:39:27 +08:00
#include "framework.h"
#include <commctrl.h>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include "TaskbarWndAnchor.h"
#include "PluginInterface.h"
#include "SplitContract.h"

#pragma comment(lib, "comctl32.lib")

//// 本模块只做一件事：在 TM 任务栏窗口每次移动落地前，把它的 X 改成“左侧按钮块右边界 + 间距”。
//// 见 TaskbarWndAnchor.h 里的物理画面。这里所有判断都是当场取状态，没有定时器、没有轮询。

//子类化标识，只用来在移除时对上号
static constexpr UINT_PTR kAnchorSubclassId = 0x5CA1;
//按钮块右边界之后留给 TM 任务栏窗口的间距（DIP）
static constexpr int kAnchorGapDip = 8;

//以下三个是跨线程的：控制器的工作线程只写这几个原子量（UpdateState / SetApp），
//界面线程（窗口过程、Sync）读；注册表、窗口这些真正的动作都留在界面线程上做。
static std::atomic<bool> g_split_active{ false };
static std::atomic<int> g_block_100{ 0 };        //左块右边界，1/100 DIP
static std::atomic<bool> g_diag_dirty{ false };  //有新状态还没写进注册表，等下一次 Sync 时写

//以下只在 TM 的界面线程访问（Sync、窗口过程都在这个线程上）
static ITrafficMonitor* g_app{};
static HWND g_subclassed{};                      //当前挂了子类过程的那个窗口
static WNDPROC g_manual_original{};              //退路（老版通用控件）下保存的原窗口过程
static bool g_manual_mode{ false };
static bool g_parent_is_primary_tray{ false };   //最后一次判断：父窗口是不是主屏任务栏
static long g_last_x_before{};
static long g_last_x_after{};

void CTaskbarWndAnchor::SetApp(ITrafficMonitor* app)
{
    g_app = app;
    g_diag_dirty.store(true);
}

void CTaskbarWndAnchor::UpdateState(bool split_active, int block_right_edge_100)
{
    if (block_right_edge_100 < 0)
        block_right_edge_100 = 0;
    bool active_changed = (g_split_active.load() != split_active);
    bool block_changed = (g_block_100.load() != block_right_edge_100);
    if (active_changed)
        g_split_active.store(split_active);
    if (block_changed)
        g_block_100.store(block_right_edge_100);
    //数值没变就不要标脏：注册表键两边都在监视，写一遍相同的内容等于自己叫醒自己
    if (active_changed || block_changed)
        g_diag_dirty.store(true);
}

//算出这次移动应该落在哪个 X（父窗口客户区坐标）。
//返回 false = 这次不该插手：拆分没生效、副屏、或 TM 没能嵌进任务栏。
//后两种绝不能改：副屏任务栏上根本没有这套系统按钮；嵌入失败时主程序用的是屏幕绝对坐标，
//再插一脚就是双重偏移，比保持原样更糟。
static bool GetAnchorX(HWND hwnd, long* out_x)
{
    if (!g_split_active.load())
        return false;
    const int block = g_block_100.load();
    if (block <= 0)
        return false;

    //主屏任务栏就是 FindWindow 拿到的那一个（和主程序 FindTaskbarHandle 的口径一致；
    //副屏的任务栏是另一个同名窗口，本体要从兄弟窗口里挑，插件这边天然就区分开了）
    const HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    const HWND parent = ::GetAncestor(hwnd, GA_PARENT);
    g_parent_is_primary_tray = (tray != nullptr && parent == tray);
    if (!g_parent_is_primary_tray)
        return false;

    //和主程序一样的换算：像素 = DIP * 任务栏DPI / 96，只是这里精度用到 1/100 DIP
    int dpi = 96;
    if (g_app != nullptr)
    {
        const int taskbar_dpi = g_app->GetDPI(ITrafficMonitor::DPI_TASKBAR);
        if (taskbar_dpi > 0)
            dpi = taskbar_dpi;
    }
    *out_x = ::MulDiv(block + kAnchorGapDip * 100, dpi, 9600);
    return true;
}

//把这次移动交给下一个窗口过程
static LRESULT CallNext(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
{
    if (g_manual_mode && g_manual_original != nullptr)
        return ::CallWindowProcW(g_manual_original, hwnd, message, w_param, l_param);
    return ::DefSubclassProc(hwnd, message, w_param, l_param);
}

static LRESULT CALLBACK AnchorWndProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param,
    UINT_PTR subclass_id, DWORD_PTR ref_data);

//两种模式共用的处理。注意：子类模式和手工换窗口过程的模式，函数签名不一样
//（前者多两个参数），装错签名在 32 位下会把调用栈踩坏，所以两个入口分开写、这里只放共同逻辑。
static LRESULT HandleAnchorMessage(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
{
    switch (message)
    {
    case WM_WINDOWPOSCHANGING:
    {
        auto pos = reinterpret_cast<WINDOWPOS*>(l_param);
        //带 SWP_NOMOVE 时 x 字段根本不参与，别去动它；只改写水平坐标，尺寸、垂直、z-order 一律不碰
        if (pos != nullptr && (pos->flags & SWP_NOMOVE) == 0)
        {
            long want_x{};
            if (GetAnchorX(hwnd, &want_x) && want_x != pos->x)
            {
                g_last_x_before = pos->x;
                g_last_x_after = want_x;
                pos->x = want_x;
            }
        }
        break;
    }

    case WM_NCDESTROY:
        //窗口要没了：子类过程随窗口一起消失（挂摘都在这个线程上，这里只把本地记录清掉）
        if (!g_manual_mode)
            ::RemoveWindowSubclass(hwnd, &AnchorWndProc, kAnchorSubclassId);
        if (hwnd == g_subclassed)
        {
            g_subclassed = nullptr;
            g_manual_original = nullptr;
            g_manual_mode = false;
            g_diag_dirty.store(true);
        }
        return CallNext(hwnd, message, w_param, l_param);
    }
    return CallNext(hwnd, message, w_param, l_param);
}

//SetWindowSubclass 用的入口（comctl32 v6）
static LRESULT CALLBACK AnchorWndProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param,
    UINT_PTR subclass_id, DWORD_PTR ref_data)
{
    return HandleAnchorMessage(hwnd, message, w_param, l_param);
}

//手工换 GWLP_WNDPROC 用的入口（老版通用控件的退路），必须是标准 WNDPROC 四个参数
static LRESULT CALLBACK AnchorManualProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
{
    return HandleAnchorMessage(hwnd, message, w_param, l_param);
}

static void AnchorDetach();

//挂上子类过程。优先用 SetWindowSubclass（comctl32 v6），拿不到就退回手工换窗口过程，
//两种都是同进程内的行为，不会影响到别的程序。
static bool AnchorAttach(HWND hwnd)
{
    if (hwnd == nullptr || !::IsWindow(hwnd))
        return false;
    if (hwnd == g_subclassed)
        return true;
    AnchorDetach();

    if (::SetWindowSubclass(hwnd, &AnchorWndProc, kAnchorSubclassId, 0))
    {
        g_subclassed = hwnd;
        g_manual_mode = false;
        g_manual_original = nullptr;
        return true;
    }
    WNDPROC original = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&AnchorManualProc)));
    if (original != nullptr)
    {
        g_subclassed = hwnd;
        g_manual_mode = true;
        g_manual_original = original;
        return true;
    }
    g_diag_dirty.store(true);
    return false;
}

static void AnchorDetach()
{
    if (g_subclassed == nullptr)
        return;
    if (::IsWindow(g_subclassed))
    {
        if (g_manual_mode)
        {
            if (g_manual_original != nullptr)
                ::SetWindowLongPtrW(g_subclassed, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_manual_original));
        }
        else
            ::RemoveWindowSubclass(g_subclassed, &AnchorWndProc, kAnchorSubclassId);
    }
    g_subclassed = nullptr;
    g_manual_original = nullptr;
    g_manual_mode = false;
    g_diag_dirty.store(true);
}

//诊断：一行 ASCII 写进注册表，插件选项里翻译成人话。
//为什么需要它——以后“TM 窗口没跟上”有三种卡法（没挂上 / 父窗口判定没过 / 数不对），
//不看这一行就得再来一轮编译才能分清，所以每次状态变化都记下来。
static void PublishDiag(HWND taskbar_wnd)
{
    wchar_t text[256]{};
    const bool attached = (g_subclassed != nullptr);
    if (g_app == nullptr)
        ::swprintf_s(text, L"sub=off reason=no-app");
    else if (taskbar_wnd == nullptr)
        ::swprintf_s(text, L"sub=off reason=no-taskbar-wnd active=%d block=%d",
            g_split_active.load() ? 1 : 0, g_block_100.load());
    else
        ::swprintf_s(text, L"sub=%s mode=%s wnd=0x%llX par=%s active=%d block=%d x=%ld->%ld",
            attached ? L"on" : L"off",
            attached ? (g_manual_mode ? L"manual" : L"subclass") : L"none",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(taskbar_wnd)),
            g_parent_is_primary_tray ? L"tray" : L"other",
            g_split_active.load() ? 1 : 0, g_block_100.load(),
            g_last_x_before, g_last_x_after);
    SplitRegWriteStringIfChanged(kRegWnd, text);
}

void CTaskbarWndAnchor::Sync()
{
    if (g_app == nullptr)
    {
        if (g_diag_dirty.exchange(false))
            PublishDiag(nullptr);
        return;
    }
    //取任务栏窗口句柄是接口 v1 才加的（它是 ITrafficMonitor 最后两个虚函数之一），
    //老宿主上直接调会踩到虚表末尾之外，所以先问版本。
    if (g_app->GetAPIVersion() < 1)
    {
        AnchorDetach();
        if (g_diag_dirty.exchange(false))
            PublishDiag(nullptr);
        return;
    }

    HWND taskbar_wnd = static_cast<HWND>(g_app->GetTaskbarWindowHwnd());
    if (!::IsWindow(taskbar_wnd))
        taskbar_wnd = nullptr;

    //任务栏窗口关掉又打开、或者从主窗口模式换过来，句柄会变；跟着挪一次子类化
    if (taskbar_wnd != g_subclassed)
    {
        if (taskbar_wnd != nullptr)
            AnchorAttach(taskbar_wnd);
        else
            AnchorDetach();
    }

    if (g_diag_dirty.exchange(false))
        PublishDiag(taskbar_wnd);
}
