// Author: Ctrl-Ling
// Last Modified By: Ctrl-Ling
// Last Modified At: 2026-11-10 19:09:21 +08:00
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
//TM 主程序给任务栏窗口设的标题（TaskBarDlg.cpp 里 SetWindowText(TASKBAR_WINDOW_NAME)）。
//官方发布版的宿主接口还停在 v0，拿不到句柄，就靠这个标题自己找。
static constexpr const wchar_t* kTaskbarWndTitle = L"TrafficMonitorTaskbarWindow";

//以下这几个是跨线程的：控制器的工作线程只写这几个原子量（UpdateState / SetApp），
//界面线程（窗口过程、Sync）读；挂/摘子类过程这类会碰加载器锁的动作仍留在界面线程做，
//工作线程只允许通过 ApplyNow 发一条移动消息。
static std::atomic<bool> g_split_active{ false };
static std::atomic<int> g_block_100{ 0 };        //左块右边界，1/100 DIP
static std::atomic<bool> g_diag_dirty{ false };  //有新状态还没写进注册表，等下一次 Sync 时写
//下面这几个计数/坐标是给"TM 的窗口没跟上"这种情况定性的：光看注册表分不清是
//"本体压根没回调插件（Sync 没跑）"还是"跑了但数不对"，所以把这两件事各留一个数。
//它们会被注册表监视线程和界面线程同时读写，只当证据用，不参与任何判断。
static std::atomic<int> g_sync_count{ 0 };       //Sync 被调了几次（0 = 这个宿主不通知插件）
static std::atomic<int> g_apply_count{ 0 };      //插件自己搬动 TM 任务栏窗口几次
static std::atomic<long> g_last_x_before{ 0 };   //最近一次搬运：搬之前在哪儿（父窗口坐标，像素）
static std::atomic<long> g_last_x_after{ 0 };    //最近一次搬运：算出来该在哪儿
static std::atomic<int> g_taskbar_dpi{ 0 };      //本体接口报的任务栏 DPI，只在界面线程的 Sync 里问一次

//以下只在 TM 的界面线程访问（Sync、窗口过程都在这个线程上）；
//ApplyNow 从注册表监视线程只读 g_subclassed 那一个句柄值（读到的是刚验过 IsWindow 的窗口，
//拿它发 SetWindowPos 是系统允许的跨线程动作），挂/摘子类过程仍然只在界面线程做。
static ITrafficMonitor* g_app{};
static HWND g_subclassed{};                      //当前挂了子类过程的那个窗口
static WNDPROC g_manual_original{};              //退路（老版通用控件）下保存的原窗口过程
static bool g_manual_mode{ false };
static bool g_parent_is_primary_tray{ false };   //最后一次判断：父窗口是不是主屏任务栏
static int g_api_version{};                      //宿主的接口版本（诊断用）
static const wchar_t* g_found_by{ L"none" };     //任务栏窗口句柄是哪一路拿到的：api / child / top / none

void CTaskbarWndAnchor::SetApp(ITrafficMonitor* app)
{
    g_app = app;
    g_diag_dirty.store(true);
}

//块宽/生效状态变了才返回 true：调用方（注册表监视线程）据此立刻搬一次窗口，
//而不是等本体下一次回调——本体可能根本不回调插件，那样"块宽变了"就只能落在纸上。
bool CTaskbarWndAnchor::UpdateState(bool split_active, int block_right_edge_100)
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
    return active_changed || block_changed;
}

//问系统要某个窗口的 DPI。为什么不直接写 ::GetDpiForWindow：它是 Win10 1607 才有的 user32 导出，
//插件工程又没有像助手工程那样定义 _WIN32_WINNT=0x0A00，头文件里不一定有这个声明（写死就是编译不过）。
//函数指针取到一次就缓存下来；取不到返回 0，调用方自己退回 96，不会因此算错方向。
static int QueryDpiForTrayWindow(HWND hwnd)
{
    using GetDpiForWindow_fn = UINT(WINAPI*)(HWND);
    static GetDpiForWindow_fn s_get_dpi = []() -> GetDpiForWindow_fn {
        const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (user32 == nullptr)
            return nullptr;
        return reinterpret_cast<GetDpiForWindow_fn>(::GetProcAddress(user32, "GetDpiForWindow"));
    }();
    if (s_get_dpi == nullptr)
        return 0;
    const UINT dpi = s_get_dpi(hwnd);
    return (dpi >= 96) ? static_cast<int>(dpi) : 0;
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

    //和主程序一样的换算：像素 = DIP * 任务栏DPI / 96，只是这里精度用到 1/100 DIP。
    //DPI 优先用本体接口报的那一份（口径和主程序完全一致），但那只能在界面线程的 Sync 里问，
    //这里读的是问到的缓存；缓存还没有（宿主一次都没回调过插件）就直接问系统要任务栏窗口的 DPI，
    //总之注册表监视线程这条路上一个宿主对象的方法都不用碰。
    int dpi = g_taskbar_dpi.load();
    if (dpi < 96)
        dpi = QueryDpiForTrayWindow(tray);
    if (dpi < 96)
        dpi = 96;
    *out_x = ::MulDiv(block + kAnchorGapDip * 100, dpi, 9600);
    return true;
}

//宿主观测不到任务栏窗口句柄时（官方发布版的 ITrafficMonitor 还是 v0，没有 GetTaskbarWindowHwnd），
//按窗口标题自己找。顺序很关键：TM 嵌进任务栏以后，这个窗口是 Shell_TrayWnd 的**子窗口**，
//而 EnumWindows 只走顶层窗口，根本看不见它（实测 reason=no-taskbar-wnd 就是这么来的）。
//所以先按子窗口找：FindWindowEx 限定在 Shell_TrayWnd 下面，副屏那个挂在 Shell_SecondaryTrayWnd 下，
//天然被排除；嵌入失败时它是顶层窗口，再用 FindWindow 兜底。
//两路都核对进程号是自己，别的程序就算抄了这个标题也找不到我们头上。
static bool IsOwnWindow(HWND hwnd)
{
    DWORD pid = 0;
    return ::GetWindowThreadProcessId(hwnd, &pid) != 0 && pid == ::GetCurrentProcessId();
}

static HWND FindOwnTaskbarWnd(const wchar_t** out_found_by)
{
    const HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray != nullptr)
    {
        //类名给 NULL = 什么类都匹配，只按标题在子窗口里找
        HWND child = ::FindWindowExW(tray, nullptr, nullptr, kTaskbarWndTitle);
        if (child != nullptr && IsOwnWindow(child))
        {
            if (out_found_by != nullptr)
                *out_found_by = L"child";
            return child;
        }
    }
    HWND top = ::FindWindowW(nullptr, kTaskbarWndTitle);
    if (top != nullptr && IsOwnWindow(top))
    {
        if (out_found_by != nullptr)
            *out_found_by = L"top";      //没嵌进任务栏，坐标是屏幕绝对值，这里不该插手
        return top;
    }
    if (out_found_by != nullptr)
        *out_found_by = L"none";
    return nullptr;
}

//块宽变了、但主程序这一次没有重新摆窗口（它只在通知区宽度/开始按钮位置变了才摆），
//就当场替它摆一次。判断只用当前状态比较，不记标志位：值已经对了就一个消息都不发，
//所以不会和主程序自己发的移动来回打架。
//hwnd 由调用方给：界面线程（Sync）通常给已挂子类的那个，注册表监视线程（ApplyNow）可能得自己找。
static void ApplyAnchorTo(HWND hwnd)
{
    if (hwnd == nullptr || !::IsWindow(hwnd))
        return;
    long want_x{};
    if (!GetAnchorX(hwnd, &want_x))
        return;

    const HWND parent = ::GetAncestor(hwnd, GA_PARENT);
    RECT rc{};
    if (!::GetWindowRect(hwnd, &rc))
        return;
    //MapWindowPoints 要的是连续的 POINT 数组，左上/右下一起换算到父窗口坐标
    POINT corners[2]{ { rc.left, rc.top }, { rc.right, rc.bottom } };
    if (::MapWindowPoints(nullptr, parent, corners, 2) == 0)
        return;
    //先把"算出来的目标"和"当时实际在哪"记下来：哪怕这次没搬（差 1 像素以内），
    //这一对数也能说明锚定模块看到的是新块宽还是旧块宽。
    g_last_x_before.store(corners[0].x);
    g_last_x_after.store(want_x);
    //1 像素以内的误差不折腾（DPI 换算本来就可能有 1px 的取整差）
    const long delta = corners[0].x > want_x ? corners[0].x - want_x : want_x - corners[0].x;
    if (delta <= 1)
        return;
    ::SetWindowPos(hwnd, nullptr, want_x, corners[0].y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_apply_count.fetch_add(1, std::memory_order_relaxed);
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
                g_last_x_before.store(pos->x);
                g_last_x_after.store(want_x);
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
//reason 非空表示这次根本没挂上，并把原因写在同一行里（老宿主接口太旧、任务栏窗口没显示……），
//省得以后看到 sub=off 还得猜是哪一种。
static void PublishDiag(HWND taskbar_wnd, const wchar_t* reason)
{
    wchar_t text[256]{};
    const bool attached = (g_subclassed != nullptr);
    if (reason != nullptr)
        ::swprintf_s(text, L"sub=off reason=%s api=%d active=%d block=%d",
            reason, g_api_version, g_split_active.load() ? 1 : 0, g_block_100.load());
    else
        //sy = Sync 跑过几次（本体回调那条路通不通）；ap = 插件自己主动搬了几次窗口。
        //两个数一起看就能定性："TM 不跟手"到底是 ①宿主没回调、②块宽没送到、③数算错：
        //  sy=0 ap>0  ①，靠注册表叫醒这条路在干活；
        //  sy>0 ap=0 且 x=两边相等 → ②或③（插件看到的还是旧边界）；
        //  sy>0 ap>0 → 都在干活，那就不是这条路的问题。
        ::swprintf_s(text, L"sub=%s find=%s m=%s sy=%d ap=%d x=%ld->%ld",
            attached ? L"on" : L"off",
            g_found_by,
            attached ? (g_manual_mode ? L"manual" : L"sub") : L"none",
            g_sync_count.load(), g_apply_count.load(),
            g_last_x_before.load(), g_last_x_after.load());
    SplitRegWriteStringIfChanged(kRegWnd, text);
    //句柄值本身没有可读性，出问题时用 reg query 看注册表更快，所以不放进这一行：
    //选项里那行静态文本只有约 55 个字符的宽度，长了会被截断（实测就是这样丢了后半句）。
    //find 已经把"父窗口对不对"说清楚了：child = 就在主屏 Shell_TrayWnd 底下；
    //top = 没嵌进任务栏，GetAnchorX 会自己拒绝改写，表现就是 x=0->0。
    (void)taskbar_wnd;
}

//注册表监视线程专用：块宽一变就把 TM 的任务栏窗口搬到新边界后面。
//这条路不要求子类过程挂上过，也不要求宿主回调过插件——句柄可以自己按标题找，
//搬位置只用 SetWindowPos（系统允许跨线程调用），不碰子类过程、不碰宿主接口对象，
//所以在工作线程上是安全的。
void CTaskbarWndAnchor::ApplyNow()
{
    HWND hwnd = g_subclassed;
    if (hwnd != nullptr && !::IsWindow(hwnd))
        hwnd = nullptr;
    if (hwnd == nullptr)
        hwnd = FindOwnTaskbarWnd(&g_found_by);   //g_found_by 只是诊断字段，跨线程写一个字符串指针没有副作用
    if (hwnd == nullptr)
        return;

    const int before = g_apply_count.load();
    ApplyAnchorTo(hwnd);
    //真的搬过了才发这一行诊断；没搬就别写注册表，写了等于自己再叫醒自己一轮
    if (g_apply_count.load() != before)
        PublishDiag(hwnd, nullptr);
}

void CTaskbarWndAnchor::Sync()
{
    g_sync_count.fetch_add(1, std::memory_order_relaxed);

    if (g_app == nullptr)
    {
        g_found_by = L"none";
        if (g_diag_dirty.exchange(false))
            PublishDiag(nullptr, L"no-app");
        return;
    }

    g_api_version = g_app->GetAPIVersion();
    //把主程序口径的任务栏 DPI 存一份，供注册表监视线程那条路用（那边的线程不碰宿主对象）
    const int host_dpi = g_app->GetDPI(ITrafficMonitor::DPI_TASKBAR);
    if (host_dpi >= 96)
        g_taskbar_dpi.store(host_dpi);
    g_found_by = L"none";
    HWND taskbar_wnd = nullptr;

    //接口 v1 的宿主（我们自己编的 TM）直接问它要句柄，这是最准的一路
    if (g_api_version >= 1)
    {
        HWND from_api = static_cast<HWND>(g_app->GetTaskbarWindowHwnd());
        if (::IsWindow(from_api))
        {
            taskbar_wnd = from_api;
            g_found_by = L"api";
        }
    }

    //官方发布版的 ITrafficMonitor 还报 v0（没有 GetTaskbarWindowHwnd 这个虚函数，
    //在它的虚表之外，绝不能调），就按窗口标题自己找
    if (taskbar_wnd == nullptr)
        taskbar_wnd = FindOwnTaskbarWnd(&g_found_by);

    //任务栏窗口关掉又打开、或者从主窗口模式换过来，句柄会变；跟着挪一次子类化
    if (taskbar_wnd != g_subclassed)
    {
        if (taskbar_wnd != nullptr)
            AnchorAttach(taskbar_wnd);
        else
            AnchorDetach();
        //挂没挂上、走哪一路拿到的句柄，都变了，把这一行诊断重新发一次
        g_diag_dirty.store(true);
    }

    //挂上之后每次同步都核对一遍当前 X：块宽变了而主程序没重摆时，由这里补一次
    //（宿主不回调插件时这条路一次都不会走，那时的兜底是 ApplyNow，由注册表变化叫醒）
    ApplyAnchorTo(g_subclassed);

    if (g_diag_dirty.exchange(false))
        PublishDiag(taskbar_wnd, taskbar_wnd == nullptr ? L"no-taskbar-wnd" : nullptr);
}
