// Author: Ctrl-Ling
// Last Modified By: Ctrl-Ling
// Last Modified At: 2026-11-10 19:54:31 +08:00
#include "framework.h"
#include "TaskbarSplit.h"
#include "detour.h"
#include "SplitContract.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Automation.Peers.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <cwctype>
#include <cmath>        //std::abs/std::lround/std::isfinite：块宽（DIP）的比较与换算
#include <cstdio>       //swprintf_s：把自检现场拼成一句话写进注册表
#include <mutex>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::UI::Xaml;
namespace xaml_automation = Windows::UI::Xaml::Automation;
namespace xaml_peers = Windows::UI::Xaml::Automation::Peers;
namespace xaml_media = Windows::UI::Xaml::Media;
namespace xaml_shapes = Windows::UI::Xaml::Shapes;

//本文件的机制（大白话）：
//Windows 11 的任务栏是一块 XAML 界面，中间那一组（开始、搜索、多任务视图、正在运行的程序）由一个
//“居中的排版器”决定每个按钮的位置，决定完就叫 IUIElement::Arrange(矩形) 把按钮放到那个矩形上。
//我们把这个 Arrange 换成自己的函数：看到系统按钮就把矩形改成“从最左边开始排”，
//同时给按钮加一个负的右边距，让排版器以为它们不占地方，于是应用图标仍然整组居中。

//挂钩点：IUIElement::Arrange 在 XAML 对象主虚表里的槽位（不同 Windows 版本可能是别的值，
//所以装完一定要做“自检”，看到任务栏按钮真的经过挂钩才上报 active）。
static constexpr int kIUIElementArrangeSlot = 92;

using Arrange_func = HRESULT(WINAPI*)(void* p_this, Rect rect);

enum class SystemButton
{
    None = -1,
    Start = 0,
    Search = 1,
    TaskView = 2,
    Widgets = 3,
};

static InlineDetour g_detour;
static Arrange_func g_arrange_original{};      //trampoline（搬走的原指令）
static std::atomic<bool> g_unloading{ false };
static std::atomic<bool> g_enabled_cache{ false };
static std::atomic<bool> g_hook_installed{ false };
static std::atomic<bool> g_seen_taskbar_button{ false };
static std::atomic<double> g_block_width_dip{ 0.0 };
static std::atomic<double> g_last_published_dip{ -1.0 };
static std::atomic<long> g_arrange_call_count{ 0 };
//左侧按钮块里每个按钮“右边沿到哪儿了”（DIP），下标 = (int)SystemButton。
//块宽不是算出来的，是这四个按钮各自报上来的右边沿里最大的那个。
static std::atomic<double> g_button_right_edge_dip[4]{ 0.0, 0.0, 0.0, 0.0 };
//块宽到底是按“看得见的边界”算的还是退回排版矩形算的，写进诊断：
//这两种数差着一两个按钮位，光看数字分不清是算法错了还是这个系统上拿不到 peer。
static std::atomic<bool> g_edge_from_uia{ false };
//每个按钮累计经过挂钩多少次（下标 = (int)SystemButton）。
//为什么要这个数：开关小组件后 TM 不动，光看块宽分不清是两种完全不同的毛病——
//  ①那个按钮根本没走到我们的 Arrange（识别条件在这个系统上不成立 / 它是另一个控件），
//  ②走到了，但当场量到个空边界（出现动画中间态），于是那一格被抹成 0。
//两者的修法一处在这个文件的事件订阅、一处在测量口径，所以每个按钮各留一个计数器，一次编译就能分开。
static std::atomic<long> g_button_arrange_count[4]{ 0, 0, 0, 0 };
//小组件自己的事件进来了几次（Unloaded / Loaded / SizeChanged 合在一起数）。
//为什么也要这个数：n= 只在块宽真的变了时才写注册表，"什么都没变"的时候一行都不落，
//于是分不清"没变化"和"没发生"。ev 每进来一次就 +1，字符串跟着变，
//注册表必然被重写一次 —— 他一按开关、TaskbarSplitEdges 没动，就是事件压根没来，
//动了但 n 的最后一位没涨，就是事件来了而按钮始终没走到 Arrange。两种毛病一眼分开。
static std::atomic<long> g_widgets_event_count{ 0 };
//小组件按钮的"尺寸/挂载刚变"事件订在哪个实例上：同一个实例只订一次，换了实例（重新创建）就换人。
//光比 abi 指针不行：旧元素被回收后，新元素完全可能拿到同一个地址，那就漏订了，
//所以再存一份弱引用 —— 引用失效（旧元素死了）就说明指针相同也不可能是同一个东西，必须重订。
//只在任务栏线程访问（Arrange 和这些事件都在那里）。
static void* g_widgets_watched_abi{ nullptr };
static winrt::weak_ref<FrameworkElement> g_widgets_watched;
static thread_local bool g_in_hook{ false };

//explorer 不重启的话，磁盘上换了新 dll，进程里可能还是旧镜像（旧代码在跑、新文件在盘上）。
//这样注册表里那个块宽到底是"新算法算错了"还是"根本没跑到新代码"，看一眼就分得清。
static void WriteBuildStamp()
{
    char stamp[64]{};
    sprintf_s(stamp, "build=%s %s", __DATE__, __TIME__);
    wchar_t wide[64]{};
    if (::MultiByteToWideChar(CP_UTF8, 0, stamp, -1, wide, 64) <= 0)
        return;
    SplitRegWriteStringIfChanged(kRegBuild, wide);
}

//第一次真的看到任务栏按钮经过挂钩，说明“钉住”这件事在当前的 explorer 里成立了，
//这一刻才把 active/pid 报出去（装配完成的当场还不能确定，因为要等一次真正的排版）。
//注册表监视是双向的，所以这里用“变了才写”的接口，避免自己叫醒自己空转。
static void ReportWorkingOnce()
{
    if (g_seen_taskbar_button.exchange(true))
        return;
    SplitRegWriteDwordIfChanged(kRegActive, 1u);
    SplitRegWriteDwordIfChanged(kRegPid, ::GetCurrentProcessId());
    SplitRegWriteDwordIfChanged(kRegStep, kStepPinning);
    WriteBuildStamp();
}

//被我们动过边距的按钮，禁用时要一个个还原回去（用弱引用，元素被回收就自动失效）
static std::mutex g_touched_mutex;
static std::vector<winrt::weak_ref<FrameworkElement>> g_touched_elements;

static SystemButton IdentifySystemButton(FrameworkElement const& element);
static bool IsPinnedClusterButton(SystemButton button);
static void UpdateClusterCollapse(FrameworkElement const& element, bool enabled);
static void UpdateWidgetsPosition(FrameworkElement const& element, bool enabled);
static void PostToTaskbarThread(FrameworkElement const& element,
    void(*func)(FrameworkElement const&, bool), bool enabled);

//windows.h 里有 max 宏，std::max 会被展开坏，这里自己做“负数就归零”
static double NotNegative(double value)
{
    return value < 0.0 ? 0.0 : value;
}

static void RememberTouched(FrameworkElement const& element)
{
    void* abi = winrt::get_abi(element);
    std::lock_guard<std::mutex> lock(g_touched_mutex);
    for (auto& weak : g_touched_elements)
    {
        try
        {
            auto resolved = weak.get();
            if (resolved && winrt::get_abi(resolved) == abi)
                return;
        }
        catch (...)
        {
        }
    }
    try
    {
        g_touched_elements.emplace_back(element);
    }
    catch (...)
    {
    }
}

void SetSplitEnabled(bool enabled)
{
    g_enabled_cache = enabled;
}

void RestoreMarginsAsync()
{
    std::vector<FrameworkElement> elements;
    {
        std::lock_guard<std::mutex> lock(g_touched_mutex);
        for (auto& weak : g_touched_elements)
        {
            try
            {
                auto element = weak.get();
                if (element)
                    elements.push_back(element);
            }
            catch (...)
            {
            }
        }
        g_touched_elements.clear();
    }

    for (auto& element : elements)
    {
        SystemButton button = IdentifySystemButton(element);
        if (button == SystemButton::Widgets)
            PostToTaskbarThread(element, UpdateWidgetsPosition, false);
        else if (IsPinnedClusterButton(button))
            PostToTaskbarThread(element, UpdateClusterCollapse, false);
    }

    g_block_width_dip = 0.0;
    for (auto& edge : g_button_right_edge_dip)
        edge = 0.0;
    g_last_published_dip = -1.0;
    g_edge_from_uia = false;
    SplitRegWriteDwordIfChanged(kRegBlock, 0u);
}

//按钮固定在左侧时，居中的那一组（应用图标）不该被它们挤开：用负的边距让排版器当它们不占位
struct PinnedChild
{
    SystemButton button{ SystemButton::None };
    double extent{};
};

static bool IsPinnedClusterButton(SystemButton button)
{
    return button == SystemButton::Start || button == SystemButton::Search || button == SystemButton::TaskView;
}

static SystemButton IdentifySystemButton(FrameworkElement const& element)
{
    try
    {
        auto class_name = get_class_name(element);
        if (class_name == L"Taskbar.ExperienceToggleButton")
        {
            auto automation_id = xaml_automation::AutomationProperties::GetAutomationId(element);
            if (automation_id == L"StartButton")
                return SystemButton::Start;
            if (automation_id == L"TaskViewButton")
                return SystemButton::TaskView;
        }
        else if (class_name == L"Taskbar.AugmentedEntryPointButton")
        {
            if (element.Name() == L"AugmentedEntryPointButton")
                return SystemButton::Widgets;
        }
        else if (class_name == L"Taskbar.TaskbarExtensionElement")
        {
            return SystemButton::Search;
        }
    }
    catch (...)
    {
    }
    return SystemButton::None;
}

static bool IsVerticalTaskbar(FrameworkElement const& frame)
{
    return frame.ActualHeight() > frame.ActualWidth();
}

//按钮不塌陷时占多宽（ActualWidth 里含了负边距，所以优先看第一个子元素的 DesiredSize）
static double GetButtonExtent(FrameworkElement const& element, bool vertical)
{
    try
    {
        if (xaml_media::VisualTreeHelper::GetChildrenCount(element) > 0)
        {
            auto child = xaml_media::VisualTreeHelper::GetChild(element, 0).try_as<FrameworkElement>();
            if (child)
            {
                auto size = child.DesiredSize();
                return vertical ? size.Height : size.Width;
            }
        }
        return vertical ? element.ActualHeight() : element.ActualWidth();
    }
    catch (...)
    {
        return 0.0;
    }
}

//递归找出某个容器下面所有系统按钮（不依赖 ItemsRepeater 的投影类型，那套头文件不在 Windows SDK 里）
static void CollectPinnedChildren(FrameworkElement const& root, std::vector<PinnedChild>& out, int depth)
{
    if (depth > 6 || out.size() >= 8)
        return;
    try
    {
        int count = xaml_media::VisualTreeHelper::GetChildrenCount(root);
        for (int i = 0; i < count; i++)
        {
            auto child = xaml_media::VisualTreeHelper::GetChild(root, i).try_as<FrameworkElement>();
            if (!child)
                continue;
            SystemButton button = IdentifySystemButton(child);
            if (button != SystemButton::None)
            {
                bool exists = false;
                for (auto& c : out)
                {
                    if (c.button == button)
                        exists = true;
                }
                if (!exists)
                    out.push_back(PinnedChild{ button, GetButtonExtent(child, IsVerticalTaskbar(root)) });
            }
            CollectPinnedChildren(child, out, depth + 1);
        }
    }
    catch (...)
    {
    }
}

//排在 target 前面的那几个按钮一共多宽（顺序按 Windows 自己的排法：开始、搜索、多任务视图）
//children_out 带上时，把这次看到的按钮一并交出去：块宽要判断"这一组里还有谁"，
//同一轮排版里已经走过的树不该再走第二遍（Arrange 的调用非常频繁）。
static double ComputeClusterOffset(FrameworkElement const& frame, SystemButton target, bool vertical,
    std::vector<PinnedChild>* children_out = nullptr)
{
    std::vector<PinnedChild> local;
    std::vector<PinnedChild>& children = (children_out != nullptr) ? *children_out : local;
    CollectPinnedChildren(frame, children, 0);
    double offset = 0.0;
    int target_rank = static_cast<int>(target);
    for (auto& c : children)
    {
        int rank = static_cast<int>(c.button);
        if (rank >= 0 && rank < target_rank && IsPinnedClusterButton(c.button))
            offset += c.extent;
    }
    return offset;
}

static void UpdateClusterCollapse(FrameworkElement const& element, bool enabled)
{
    try
    {
        auto frame = xaml_media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
        if (!frame)
            return;

        bool vertical = IsVerticalTaskbar(frame);
        Thickness margin = element.Margin();
        Thickness new_margin = margin;

        //只还原本模块加过的塌陷边距（两个方向都清掉，任务栏可能在上下/左右之间切换过）
        new_margin.Right = NotNegative(new_margin.Right);
        new_margin.Bottom = NotNegative(new_margin.Bottom);

        if (enabled)
        {
            double extent = GetButtonExtent(element, vertical);
            if (vertical)
                new_margin.Bottom = -extent;
            else
                new_margin.Right = -extent;
        }

        if (new_margin.Right != margin.Right || new_margin.Bottom != margin.Bottom)
            element.Margin(new_margin);
    }
    catch (...)
    {
    }
}

//小组件按钮是 Windows 钉在最前面的，用前边距把它挪到这一组按钮后面
static void UpdateWidgetsPosition(FrameworkElement const& element, bool enabled)
{
    try
    {
        auto frame = xaml_media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
        if (!frame)
            return;

        bool vertical = IsVerticalTaskbar(frame);
        double leading = enabled ? ComputeClusterOffset(frame, SystemButton::Widgets, vertical) : 0.0;

        Thickness margin = element.Margin();
        Thickness new_margin = margin;
        if (vertical)
        {
            new_margin.Left = 0;
            new_margin.Top = leading;
        }
        else
        {
            new_margin.Top = 0;
            new_margin.Left = leading;
        }

        if (new_margin.Left != margin.Left || new_margin.Top != margin.Top)
            element.Margin(new_margin);
    }
    catch (...)
    {
    }
}

static bool ClusterMarginNeedsUpdate(FrameworkElement const& element, bool enabled)
{
    try
    {
        auto frame = xaml_media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
        if (!frame)
            return false;

        bool vertical = IsVerticalTaskbar(frame);
        Thickness margin = element.Margin();
        double trailing = vertical ? margin.Bottom : margin.Right;
        double other = vertical ? margin.Right : margin.Bottom;

        if (other < -0.5)
            return true;

        if (enabled)
        {
            double extent = GetButtonExtent(element, vertical);
            return trailing > -extent + 0.5;
        }
        return trailing < -0.5;
    }
    catch (...)
    {
        return false;
    }
}

static bool WidgetsPositionNeedsUpdate(FrameworkElement const& element, bool enabled)
{
    try
    {
        auto frame = xaml_media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
        if (!frame)
            return false;

        bool vertical = IsVerticalTaskbar(frame);
        double leading = enabled ? ComputeClusterOffset(frame, SystemButton::Widgets, vertical) : 0.0;
        Thickness margin = element.Margin();
        double current = vertical ? margin.Top : margin.Left;
        return std::abs(current - leading) > 0.5;
    }
    catch (...)
    {
        return false;
    }
}

//改边距会引起重新排版，正在排版时不能改，投递到该线程的消息队列里去做
static void PostToTaskbarThread(FrameworkElement const& element, void(*func)(FrameworkElement const&, bool), bool enabled)
{
    try
    {
        element.Dispatcher().TryRunAsync(
            Windows::UI::Core::CoreDispatcherPriority::High,
            [element, func, enabled]() { func(element, enabled); });
    }
    catch (...)
    {
    }
}

//眼睛看到的右边界，以 UIA 的 BoundingRectangle 为准 —— 那才是屏幕上真正占位的那一块。
//为什么不能继续用排版矩形（X+Width）：实测四格里只有开始按钮对得上，其余全都偏：
//  搜索那个元素排出来宽是 0（报 44.80，可见图标在 44~88）；
//  多任务视图报 88.80，可见按钮其实在 88~132；
//  小组件按钮的矩形比天气图标宽出一大截（报 348.80，可见右边界只有 184）。
//也就是说排版矩形和可见内容是两套坐标，拿它当块宽，TM 一会儿压住按钮、一会儿空一大条。
//拿不到 peer、或算出个离谱的数，就退回排版矩形（保持原来的行为，不会更糟）。
//out_visible=false 表示这个按钮现在根本不占位：小组件在设置里关掉后元素还挂在树上，
//但已经折叠了，它自己再也不会来报"我不在了"，只能靠这个判断把它那一格抹掉。
static double VisibleRightEdgeDip(FrameworkElement const& element, double fallback_edge_dip,
    bool* out_visible)
{
    if (out_visible != nullptr)
        *out_visible = true;
    try
    {
        auto peer = xaml_peers::FrameworkElementAutomationPeer::CreatePeerForElement(element);
        if (!peer)
            return fallback_edge_dip;

        //BoundingRectangle 是屏幕物理像素；宽或高为 0 = 这个按钮现在看不见
        Rect bounds = peer.GetBoundingRectangle();
        if (!std::isfinite(bounds.Width) || !std::isfinite(bounds.Height) ||
            bounds.Width <= 0.0 || bounds.Height <= 0.0)
        {
            if (out_visible != nullptr)
                *out_visible = false;
            return 0.0;
        }

        const HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
        RECT tray_rc{};
        UINT dpi = 96;
        if (tray == nullptr || !::GetWindowRect(tray, &tray_rc))
            return fallback_edge_dip;
        dpi = ::GetDpiForWindow(tray);
        if (dpi < 96)
            dpi = 96;

        const double edge = (static_cast<double>(bounds.X) + bounds.Width - tray_rc.left) * 96.0 / dpi;
        //任务栏再宽也不会到十万 DIP，越界说明这个版本的 peer 给的坐标不是这个意思
        if (!std::isfinite(edge) || edge <= 0.0 || edge > 100000.0)
            return fallback_edge_dip;
        g_edge_from_uia.store(true);
        return edge;
    }
    catch (...)
    {
        return fallback_edge_dip;
    }
}

//四个按钮各自报上来的右边沿，一行写清楚。为什么要这一行：块宽只有一个数，
//"谁没报上来"和"谁报大了"看着一样（TM 窗口要么空一条要么压住按钮），
//有了四格现场值就能一次分清，省掉一轮编译。src 说明这些数是"看得见的边界"还是退回了排版矩形。
//n= 是这四个按钮各自累计经过挂钩几次（顺序还是 s/se/tv/w）：
//某一格是 0 就说明它根本没来排版（识别或事件没覆盖到），有次数却报 0 就是量到空边界。
//ev= 是小组件自己的事件进来了几次：光有 n= 时，"什么都没变"和"根本没发生"看着一样，
//所以小组件事件每次都顺手强制发一行 —— 他按了开关而这一行没动，就是事件压根没来。
//pub 不在这里重复了，"助手已挂钩，块宽 …"那一行给的就是同一个数。
static void PublishEdges()
{
    wchar_t edges[128]{};
    ::swprintf_s(edges, L"s=%d se=%d tv=%d w=%d src=%s n=%d/%d/%d/%d ev=%d",
        static_cast<int>(std::lround(g_button_right_edge_dip[0].load() * 100.0)),
        static_cast<int>(std::lround(g_button_right_edge_dip[1].load() * 100.0)),
        static_cast<int>(std::lround(g_button_right_edge_dip[2].load() * 100.0)),
        static_cast<int>(std::lround(g_button_right_edge_dip[3].load() * 100.0)),
        g_edge_from_uia.load() ? L"uia" : L"rect",
        static_cast<int>(g_button_arrange_count[0].load()),
        static_cast<int>(g_button_arrange_count[1].load()),
        static_cast<int>(g_button_arrange_count[2].load()),
        static_cast<int>(g_button_arrange_count[3].load()),
        static_cast<int>(g_widgets_event_count.load()));
    SplitRegWriteStringIfChanged(kRegEdges, edges);
}

static void PublishBlockWidth(double width_dip)
{
    //宽度基本没变就不写注册表（每写一次就是一次跨进程通知）
    if (std::abs(width_dip - g_last_published_dip.load()) < 0.5)
        return;
    g_last_published_dip = width_dip;
    DWORD block = static_cast<DWORD>(std::lround(width_dip * 100.0));
    SplitRegWriteDword(kRegBlock, block);

    PublishEdges();
}

//这一轮排版实际排了哪几个按钮，不在树上的那一格必须清零。
//关键：小组件按钮在 Windows 设置里关掉以后，它是从树上整个消失的，
//再也不会走到我们的 Arrange —— 等它自己说"我不在了"永远等不到，
//块宽就一直挂着它的旧值（实测关掉小组件后仍是 348，而真实右边界只有 132），
//TM 的窗口就会在左侧按钮后面空出一大条。
//所以在别的按钮排版时顺手看一眼这组里还有谁。一个按钮都没找到时什么都不清：
//那种时候多半是树还没建好或容器是空的，清错了块宽掉到 0，TM 会一头撞进开始按钮。
//返回"真的抹掉了某一格"，调用方据此重新发布块宽——抹完不发就等于白抹。
//children 由调用方从同一轮排版里拿到的那份交进来：Arrange 调用非常频繁，
//同一棵树不该为了算块宽再走第二遍。
static bool ForgetAbsentButtons(std::vector<PinnedChild> const& children)
{
    if (children.empty())
        return false;

    bool present[4]{ false, false, false, false };
    for (auto& child : children)
    {
        const int index = static_cast<int>(child.button);
        if (index >= 0 && index < 4)
            present[index] = true;
    }
    //拿“开始按钮在场 + 至少两个按钮”当作“这个容器就是那一组钉住的按钮”的凭据。
    //万一带上来的容器只是半个组（比如小组件挂在另一个容器里），它看不见别的按钮就会把
    //别人那一格抹掉，块宽反而变小、TM 就压住按钮了 —— 认不准就不清，宁可维持上一次的值。
    if (!present[static_cast<int>(SystemButton::Start)] || children.size() < 2)
        return false;

    bool changed = false;
    for (int i = 0; i < 4; i++)
    {
        if (!present[i] && g_button_right_edge_dip[i].load() != 0.0)
        {
            g_button_right_edge_dip[i] = 0.0;
            changed = true;
        }
    }
    return changed;
}

static double MaxBlockEnd()
{
    double block = 0.0;
    for (int i = 0; i < 4; i++)
    {
        const double edge = g_button_right_edge_dip[i].load();
        if (edge > block)
            block = edge;
    }
    return block;
}

//四格现值 -> 块宽 -> 写注册表（PublishBlockWidth 内部自己判断够不够变化）
static void PublishBlockFromSlots()
{
    const double block = MaxBlockEnd();
    g_block_width_dip = block;
    PublishBlockWidth(block);
}

//某个按钮报上自己“看得见”的右边沿（DIP）：把它那一格记下来，块宽取四格里最大的那格。
//数的来源见上面 VisibleRightEdgeDip 的说明（UIA 边界为准，排版矩形只当兜底）；
//更早的版本是拿 ComputeClusterOffset + GetButtonExtent 另算一套（DesiredSize 会跟着天气文字、
//悬停宽度上下飘），而且只会变大不会变小 —— 实测注册表里长期挂着 348，而小组件真实右边沿只有 184，
//TM 把窗口钉在这个数后面就一会儿空一条、一会儿压住小组件。
static void UpdateBlockEnd(SystemButton button, double right_edge_dip)
{
    const int index = static_cast<int>(button);
    if (index < 0 || index >= 4 || !std::isfinite(right_edge_dip) || right_edge_dip <= 0.0)
        return;
    if (std::abs(right_edge_dip - g_button_right_edge_dip[index].load()) < 0.05)
        return;         //这一格没变，不用重算也不用写注册表
    g_button_right_edge_dip[index] = right_edge_dip;
    PublishBlockFromSlots();
}

//某个按钮不见了（比如小组件被关掉）：清掉那一格，块宽才能跟着缩回去
static void ClearBlockEnd(SystemButton button)
{
    const int index = static_cast<int>(button);
    if (index < 0 || index >= 4 || g_button_right_edge_dip[index].load() == 0.0)
        return;
    g_button_right_edge_dip[index] = 0.0;
    PublishBlockFromSlots();
}

//小组件那一格重新量一次并记账（它自己的事件进来时走这里）。
//量不到就什么都不做：绝不能把"这会儿量不到"当成"它不在了"抹掉，
//抹掉的后果就是他看到的那个 bug —— 小组件明明开了，块宽还停在没开的那个数，TM 压在小组件上。
static void RepublishWidgetsEdge(FrameworkElement const& element)
{
    if (!g_enabled_cache.load())
        return;
    bool visible = true;
    const double edge = VisibleRightEdgeDip(element, 0.0, &visible);
    if (visible && edge > 0.0)
        UpdateBlockEnd(SystemButton::Widgets, edge);
    else if (!element.IsVisible())
        ClearBlockEnd(SystemButton::Widgets);
}

//小组件事件的统一处理：记一笔 ev、按类型补账，然后无条件发一行现场。
//为什么"无条件发"：只有 ev 变了才保证注册表被动一次，这样"我按了开关但什么都没发生"
//在注册表里是看得见的（那一行不动），而不是只能靠"数字没变"去猜。
static void OnWidgetsEvent(winrt::weak_ref<FrameworkElement> const& weak, bool unloaded)
{
    g_widgets_event_count.fetch_add(1, std::memory_order_relaxed);
    try
    {
        auto element = weak.get();
        if (!element)
            return;
        if (unloaded)
            ClearBlockEnd(SystemButton::Widgets);       //元素从树上摘掉了，那一格不能留
        else
            RepublishWidgetsEdge(element);
    }
    catch (...)
    {
    }
    PublishEdges();
}

//给小组件按钮订一次它自己的事件。为什么非得订：块宽过去只在"有按钮走到 Arrange"时才重新记账，
//可开关小组件这件事不一定让别的按钮重排一次版——
//  关掉时：那个元素整个从树上摘掉，再也不会来报"我不在了"，旧边界就一直挂着（TM 空一大条）；
//  打开时：它刚出现那一下量到的可能是空边界（动画中间态），我们当时把那一格抹成 0（TM 压住小组件）。
//两种都得等一个外部事件（实测是系统截图）逼任务栏完整排一次版才对得上。
//这三个事件都是 XAML 自己发的，不用定时器（UWP 的 FrameworkElement 上没有"可见性变化"这个事件，
//但折叠成 0 宽必然走 SizeChanged，效果一样）：
//  Unloaded —— 元素被摘掉：那一格必须清零；
//  Loaded —— 元素又挂回来（重新打开小组件 / 任务栏重建）：先量一次，量不到也还有下面的 SizeChanged；
//  SizeChanged —— 尺寸刚落定（出现动画走完、天气文字变长变短、被折叠成 0）：重量一次。
static void WatchWidgetsEvents(FrameworkElement const& element)
{
    void* abi = winrt::get_abi(element);
    if (g_widgets_watched_abi == abi)
    {
        try
        {
            //指针相同还不够，弱引用必须还能取出同一个对象，才算"订在这一个实例上"
            if (g_widgets_watched.get())
                return;                 //同一个实例订过了，别一次排版订一遍
        }
        catch (...)
        {
        }
    }

    g_widgets_watched_abi = abi;
    try
    {
        //弱引用：元素被回收就自动失效，不能让它为了听事件而永远活着
        winrt::weak_ref<FrameworkElement> weak{ element };
        g_widgets_watched = weak;

        //摘掉（小组件在设置里关掉，这个按钮就从树上消失了）：那一格必须清零
        element.Unloaded([weak](auto&&, auto&&) { OnWidgetsEvent(weak, true); });

        //挂回来（重新打开小组件、任务栏重建）：出现动画这一步可能还量不到，
        //但紧接着一定会有 SizeChanged，那里再量一次
        element.Loaded([weak](auto&&, auto&&) { OnWidgetsEvent(weak, false); });

        //刚落定一次尺寸（出现动画走完、天气文字变长变短、整组折叠成 0）：重量一次
        element.SizeChanged([weak](auto&&, auto&&) { OnWidgetsEvent(weak, false); });
    }
    catch (...)
    {
        g_widgets_watched_abi = nullptr;    //没订上（这个元素不支持），下次排版再试
        g_widgets_watched = nullptr;
    }
}

//Arrange 挂钩：排版器每次给按钮定位都会走到这里
static HRESULT WINAPI ArrangeHook(void* p_this, Rect rect)
{
    g_arrange_call_count.fetch_add(1, std::memory_order_relaxed);

    if (g_unloading.load() || g_in_hook)
        return g_arrange_original(p_this, rect);

    g_in_hook = true;
    struct Guard
    {
        ~Guard() { g_in_hook = false; }
    } guard;

    try
    {
        FrameworkElement element{ nullptr };
        reinterpret_cast<::IUnknown*>(p_this)->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(element));
        if (!element)
            return g_arrange_original(p_this, rect);

        const bool enabled = g_enabled_cache.load();
        SystemButton button = IdentifySystemButton(element);

        //每一格都记一次"这个按钮走到 Arrange 没有"。诊断里 n=开始/搜索/视图/小组件 就是这四个数：
        //开关小组件之后 w 没动，先看这里——n 的最后一位没涨 = 它压根没来排版（那就是元素被摘掉，
        //靠它自己的 Unloaded 事件补账）；涨了但 w 还是旧值 = 来了、量到的是空边界（动画中间态）。
        if (button != SystemButton::None)
            g_button_arrange_count[static_cast<int>(button)].fetch_add(1, std::memory_order_relaxed);

        if (button == SystemButton::Widgets)
        {
            ReportWorkingOnce();
            RememberTouched(element);
            //从此以后这一格不再只靠排版轮次：元素自己会报"我变了/我不在了"
            WatchWidgetsEvents(element);
            if (WidgetsPositionNeedsUpdate(element, enabled))
                PostToTaskbarThread(element, UpdateWidgetsPosition, enabled);
            //小组件是这一组里最右边的一个，块宽基本就是它的右边沿。
            //数从 UIA 的可见边界拿（拿不到才退回排版矩形）：这个按钮的排版矩形比天气图标宽出一大截，
            //照着矩形报，TM 的窗口就会在小组件后面空出 160 多 DIP。
            if (enabled)
            {
                auto frame = xaml_media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
                if (frame)
                {
                    std::vector<PinnedChild> children;
                    CollectPinnedChildren(frame, children, 0);
                    if (ForgetAbsentButtons(children))
                        PublishBlockFromSlots();
                }
                bool visible = true;
                const double edge = VisibleRightEdgeDip(element, rect.X + rect.Width, &visible);
                if (visible)
                    UpdateBlockEnd(SystemButton::Widgets, edge);
                else if (!element.IsVisible())
                    ClearBlockEnd(SystemButton::Widgets);      //小组件真的被关掉：元素还在树上，但不占位了
                //visible==false 但 IsVisible()==true：这是出现动画的中间态（元素说要画、peer 还没给出边界）。
                //这时候绝不能把那一格抹成 0 —— 抹了就是"小组件开着、块宽却按没开算"，TM 直接压上去。
                //留着旧值，等它自己的 Loaded / SizeChanged 来重量一遍。
            }
            else
                ClearBlockEnd(SystemButton::Widgets);
            return g_arrange_original(p_this, rect);
        }

        if (!IsPinnedClusterButton(button))
            return g_arrange_original(p_this, rect);     //应用图标等一律不动

        ReportWorkingOnce();
        RememberTouched(element);

        if (ClusterMarginNeedsUpdate(element, enabled))
            PostToTaskbarThread(element, UpdateClusterCollapse, enabled);

        if (!enabled)
        {
            ClearBlockEnd(button);
            return g_arrange_original(p_this, rect);
        }

        auto frame = xaml_media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
        if (!frame)
            return g_arrange_original(p_this, rect);

        bool vertical = IsVerticalTaskbar(frame);
        //算偏移用的那份"这一组里有谁"，顺手交给块宽记账：这一轮树上没有的按钮，
        //它那一格必须抹掉（小组件关掉后它再也不会来报），不然块宽永远缩不回去
        std::vector<PinnedChild> children;
        double offset = ComputeClusterOffset(frame, button, vertical, &children);
        if (ForgetAbsentButtons(children))
            PublishBlockFromSlots();

        Rect new_rect = rect;
        //把按钮摆到“前面几个按钮排完之后”的位置
        if (vertical)
            new_rect.Y = static_cast<float>(offset);
        else
            new_rect.X = static_cast<float>(offset);

        //块宽记的是这个按钮“看得见”的右边沿（排版矩形只当作拿不到 peer 时的兜底）：
        //实测搜索那一格排出来宽是 0、多任务视图的可见图标比它的排版矩形整整右移一个按钮位，
        //照着排版矩形报，块宽就永远比屏幕上的按钮窄一截，TM 的窗口会压住按钮。
        bool visible = true;
        const double rect_edge = static_cast<double>(new_rect.X) + new_rect.Width;
        const double edge = VisibleRightEdgeDip(element, rect_edge, &visible);
        if (visible)
            UpdateBlockEnd(button, edge);
        else
            ClearBlockEnd(button);

        return g_arrange_original(p_this, new_rect);
    }
    catch (...)
    {
        return g_arrange_original(p_this, rect);
    }
}

//虚表槽拿到的地址必须落在 XAML 模块里，否则说明这个版本的槽位不是 Arrange
static bool IsInXamlModule(void* address)
{
    HMODULE module{};
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &module) || module == nullptr)
        return false;
    wchar_t path[MAX_PATH]{};
    if (::GetModuleFileNameW(module, path, MAX_PATH) == 0)
        return false;
    std::wstring name(path);
    for (auto& c : name)
        c = static_cast<wchar_t>(towlower(c));
    return name.find(L"xaml") != std::wstring::npos;
}

//调用一个可能随时崩的函数：只用裸指针，出异常就返回 false，并把异常码带出来。
//异常码才是真证据：0xC000061B=控制保护异常（CET：入口的 endbr64 被盖掉了，间接调用被 CPU 拦下）；
//0xC0000005=普通访问违例（搬走的指令跑飞了 / 参数不对）；0xC0000617=CFG 违规。
//这三种的下一步做法完全不一样，只报“崩了”等于没报。
static bool SafeCall(Arrange_func func, void* abi, Rect rect, HRESULT* hr, DWORD* se_code)
{
    __try
    {
        *hr = func(abi, rect);
        return true;
    }
    __except ((*se_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER))
    {
        return false;
    }
}

//自检现场写进注册表（TaskbarSplitSelfTest）：哪一步、计数加了几、返回码、异常码，后面还能再挂一串地址。
//“没进钩子”和“进了钩子但跑飞了”看着都是“没生效”，做法却完全相反，只能靠这几个数字分。
static void PublishSelfTest(const wchar_t* stage, long call_delta, HRESULT hr, DWORD se_code,
    const wchar_t* extra = nullptr)
{
    wchar_t text[400]{};
    if (extra != nullptr && extra[0] != L'\0')
    {
        ::swprintf_s(text, L"%s calls=%ld hr=0x%08lX se=0x%08lX %s", stage, call_delta,
            static_cast<unsigned long>(hr), static_cast<unsigned long>(se_code), extra);
    }
    else
    {
        ::swprintf_s(text, L"%s calls=%ld hr=0x%08lX se=0x%08lX", stage, call_delta,
            static_cast<unsigned long>(hr), static_cast<unsigned long>(se_code));
    }
    SplitRegWriteStringIfChanged(kRegSelfTest, text);
}

//槽地址、函数入口、真正下补丁的地址一起写出来：
//入口和补丁地址差 3 字节 = 入口是 endbr64 着陆垫，我们没盖它（CET 要求，盖了必崩）；
//入口和槽地址不一致 = 穿过了开头的 jmp，那调用可能根本不走我们改的地方。
//M:=槽地址所在模块的加载基址 —— S 减 M 就是这个函数在这个 dll 里的偏移，
//拿这个偏移去查微软公开符号（dbghelp SymFromAddr）就能直接问出“槽位 92 到底叫什么名字”。
static void PublishTargetAddresses(void* slot_address, const InlineDetour& detour)
{
    uintptr_t module_base = 0;
    HMODULE module{};
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(slot_address), &module) && module != nullptr)
        module_base = reinterpret_cast<uintptr_t>(module);
    wchar_t text[200]{};
    ::swprintf_s(text, L"S:0x%016llX T:0x%016llX A:0x%016llX M:0x%016llX",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(slot_address)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(detour.target)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(DetourPatchAddress(detour))),
        static_cast<unsigned long long>(module_base));
    SplitRegWriteStringIfChanged(kRegTarget, text);
}

//自检的结果：补丁留不留 + 该报哪个进度码。
struct SelfCheck
{
    bool keep_hook{ false };    //false = 把补丁拆掉（我们不确信它是好的，不能留在 explorer 里）
    DWORD step{ 0 };            //0 = 通过（由调用方写成 kStepHookInstalled）；非 0 = 直接写进注册表的进度码
};

//自检（2026-11-06 重做）：
//以前是“重新取一次探针的虚表槽，调它”，两个坑：
//  ① 探针是个没 Measure 过、也没挂在任何窗口上的孤儿 Rectangle，XAML 自己走进 Arrange 就飞了，
//     崩了到底是“补丁坏了”还是“探针本来就不能用”分不开 —— 实测连报 241、calls=0，
//     而注册表里补丁字节明明是活的（E9 跳进我们的函数）。
//  ② 拆掉补丁的权力交给了一个本身就不靠谱的调用。
//现在的顺序：先做“不执行任何代码”的地址对照（S/V/A/H），再用 trampoline 打一发基线（判断探针到底能不能用），
//最后才踩“真正下了补丁的那个地址”（不是重新读虚表槽 —— 踩自己改过的地址才是唯一的自检目标）。
//每一步的结论都写进 SelfTest，keep_hook 决定补丁留不留。
static SelfCheck ValidateHook(void* install_slot)
{
    SelfCheck result;
    try
    {
        xaml_shapes::Rectangle probe;
        UIElement ui_element{ probe };
        void* abi = winrt::get_abi(ui_element);
        //注意是“取 abi 指向的虚表指针”，不是“取存放 abi 的那个局部变量的值”——
        //写成 &abi 就等于把接口指针本身当虚表，往后数 92 格拿到的是完全无关的字节：
        //以前自检“一踩就崩、calls=0、se=0xC0000005”就是这么来的（崩在一个随机数当函数指针上），
        //跟补丁、跟着陆垫都没关系，却把已经写好的补丁给拆了。
        void** vtable = *reinterpret_cast<void***>(abi);
        const uintptr_t probe_slot = reinterpret_cast<uintptr_t>(vtable[kIUIElementArrangeSlot]);
        void* const patch_at = DetourPatchAddress(g_detour);

        //把“挂钩时读的槽 / 探针现在的槽 / 补丁地址 / 我们的钩子”四个地址一次性写出来。
        //S 和 V 不相等 = 这个虚表槽每个对象各有一份（各自的小转发），那我们改的那一份
        //压根不是任务栏按钮走的那一份 —— 换成再多的自检也测不出来，只能换找挂钩点的办法。
        wchar_t addrs[256]{};
        ::swprintf_s(addrs,
            L"S:0x%016llX V:0x%016llX %s A:0x%016llX H:0x%016llX",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(install_slot)),
            static_cast<unsigned long long>(probe_slot),
            (probe_slot == reinterpret_cast<uintptr_t>(install_slot)) ? L"V=S" : L"V!=S",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(patch_at)),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&ArrangeHook)));

        if (patch_at == nullptr)
        {
            PublishSelfTest(L"no_patch", 0, static_cast<HRESULT>(E_UNEXPECTED), 0, addrs);
            result.step = kStepDetourBadArgs;
            return result;                //keep_hook 保持 false
        }

        //一次“踩一脚”，回来时 delta = 期间进过我们钩子的次数，out_hr/out_se = 现场
        auto Stomp = [&](Arrange_func func, float size, long* delta, HRESULT* out_hr, DWORD* out_se) -> bool
        {
            const long before = g_arrange_call_count.load();
            const bool ok = SafeCall(func, abi, Rect{ 0.0f, 0.0f, size, size }, out_hr, out_se);
            *delta = g_arrange_call_count.load() - before;
            return ok;
        };

        //①基线：只走 trampoline（原函数的真身，完全不经过我们的代码）。
        //  这一脚就崩 = 孤儿探针本身不能拿来排版，跟补丁无关；
        //  再多踩几次只是让 explorer 多吐几个异常，一点信息都换不来 —— 收工，补丁留着（字节已回读核对过），
        //  真凭据是后面任务栏自己排一次版（进度码 40）。
        long delta{};
        HRESULT hr{};
        DWORD se_code{};
        if (!Stomp(g_arrange_original, 40.0f, &delta, &hr, &se_code))
        {
            PublishSelfTest(L"probe_unsafe", delta, hr, se_code, addrs);
            result.keep_hook = true;
            result.step = kStepSelfCallProbeUnsafe;
            return result;
        }

        //②踩“真正下了补丁的地址”。第一条指令就是我们写的 jmp，理论上必定进钩子：
        //  计数动了 = 挂钩点和钩子本体都是好的（哪怕随后在 XAML 深处崩，那也是孤儿探针的锅）；
        //  计数没动还没崩 = 那段字节被执行了却没跳到我们这儿（写进去的不是真的 / 页被还原）；
        //  计数没动且崩了   = 头几个字节本身就是坏的，必须马上拆，不然 explorer 迟早栽在它手上。
        long hook_delta{};
        const bool hook_ok = Stomp(reinterpret_cast<Arrange_func>(patch_at), 41.0f, &hook_delta, &hr, &se_code);
        if (hook_delta > 0)
        {
            PublishSelfTest(hook_ok ? L"hook_ok" : L"hooked_then_probe_crash", hook_delta, hr, se_code, addrs);
            result.keep_hook = true;
            result.step = 0;
            return result;
        }
        if (!hook_ok)
        {
            PublishSelfTest(L"patch_dead_crash", hook_delta, hr, se_code, addrs);
            result.keep_hook = false;
            result.step = kStepSelfCallHookFaulted;
            return result;
        }
        PublishSelfTest(L"patch_dead_clean", hook_delta, hr, se_code, addrs);
        result.keep_hook = false;
        result.step = kStepSelfCallNotHooked;
        return result;
    }
    catch (...)
    {
        //建探针对象/取虚表就抛了，这个线程里的 XAML 运行时状态有问题
        PublishSelfTest(L"cpp", 0, static_cast<HRESULT>(E_UNEXPECTED), 0, nullptr);
        result.keep_hook = false;
        result.step = static_cast<DWORD>(kStepSelfCallThrew);
        return result;
    }
}

//地址开头的 24 字节 -> 十六进制字符串（读不到写 NA）。out 至少 49 个 wchar_t。
static void BytesToHex(void* address, wchar_t* out)
{
    static constexpr size_t kDumpBytes = 24;
    static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
    unsigned char bytes[kDumpBytes]{};
    const size_t read = ReadTargetBytes(address, bytes, kDumpBytes);
    if (read == 0)
    {
        out[0] = L'N';
        out[1] = L'A';
        out[2] = L'\0';
        return;
    }
    size_t pos = 0;
    for (size_t n = 0; n < read; n++)
    {
        out[pos++] = kHex[bytes[n] >> 4];
        out[pos++] = kHex[bytes[n] & 0x0F];
    }
    out[pos] = L'\0';
}

//把挂钩现场写进注册表（TaskbarSplitPrologue）：
//S: = 虚表槽里那个地址（函数入口）的开头 24 字节；A: = 真正下补丁那个地址的开头 24 字节。
//入口若以 0F1EFA 开头就是 endbr64 着陆垫，补丁必须从它后面第 3 字节开始（A: 比 S: 晚 3 字节）；
//装成功时 A: 会是 FF2500000000+钩子地址，正好证明补丁还在；装失败时这两串就是卡住的原始指令。
static void PublishPrologue(void* slot_address, const InlineDetour& detour)
{
    wchar_t slot_hex[64]{}, patch_hex[64]{};
    BytesToHex(slot_address, slot_hex);
    wchar_t text[160]{};
    size_t pos = 0;
    auto append = [&](const wchar_t* s) { while (*s != L'\0') text[pos++] = *s++; };
    append(L"S:");
    append(slot_hex);
    if (detour.target != nullptr)
    {
        BytesToHex(DetourPatchAddress(detour), patch_hex);
        append(L" A:");
        append(patch_hex);
    }
    text[pos] = L'\0';
    SplitRegWriteStringIfChanged(kRegPrologue, text);
}

//内联跳转的失败岔口 -> 进度码（23*10 + 原因号），插件那边一一对上中文
static DWORD DetourFailToStep(DetourFail fail)
{
    switch (fail)
    {
    case kDetourFailBadArgs:        return kStepDetourBadArgs;
    case kDetourFailUnreadable:     return kStepDetourUnreadable;
    case kDetourFailBadInstruction: return kStepDetourBadInstruction;
    case kDetourFailTooLong:        return kStepDetourTooLong;
    case kDetourFailFarTarget:      return kStepDetourFarTarget;
    case kDetourFailNoMemory:       return kStepDetourNoMemory;
    case kDetourFailProtect:        return kStepDetourProtect;
    }
    return kStepDetourFailed;
}

bool InstallTaskbarSplit()
{
    if (g_hook_installed.load())
        return true;

    g_unloading = false;        //上一轮 RemoveTaskbarSplit 置过真，这里必须清掉，否则钩子会一直“只转发不干活”
    void* arrange_address{};
    try
    {
        //这一步最容易失败：这个线程里没有 XAML 运行时的话，建对象就抛异常
        xaml_shapes::Rectangle rectangle;
        UIElement ui_element{ rectangle };
        void** vtable = *reinterpret_cast<void***>(winrt::get_abi(ui_element));
        arrange_address = vtable[kIUIElementArrangeSlot];
    }
    catch (...)
    {
        SplitRegWriteDwordIfChanged(kRegStep, kStepProbeObjectFailed);
        return false;
    }

    if (arrange_address == nullptr || !IsInXamlModule(arrange_address))
    {
        SplitRegWriteDwordIfChanged(kRegStep, kStepSlotNotXaml);
        return false;
    }

    //第一选择是“槽位指哪就打哪”：这样凡是走这个虚表槽的调用必定进我们的钩子。
    //只有那里开头实在搬不动，才退一步穿过开头的 jmp 把补丁打到真身（实测打过一次真身，
    //自检时调用虚表槽根本没进钩子 —— 说明那条 jmp 的目标不是调用真正去的地方）。
    DetourFail detour_fail = kDetourFailNone;
    void* const hook_address = reinterpret_cast<void*>(&ArrangeHook);
    bool detour_ok = InstallInlineDetour(g_detour, arrange_address, hook_address, &detour_fail, false);
    if (!detour_ok)
        detour_ok = InstallInlineDetour(g_detour, arrange_address, hook_address, &detour_fail, true);
    //不管成没成，都把挂钩现场（两处地址 + 两处开头字节）写进注册表：
    //出错时凭这两串就能离线还原现场，不用再盲改一轮。
    PublishTargetAddresses(arrange_address, g_detour);
    PublishPrologue(arrange_address, g_detour);

    if (!detour_ok)
    {
        SplitRegWriteDwordIfChanged(kRegStep, DetourFailToStep(detour_fail));
        return false;
    }
    g_arrange_original = reinterpret_cast<Arrange_func>(g_detour.trampoline);
    if (g_arrange_original == nullptr)
    {
        RestoreDetourBytes();
        SplitRegWriteDwordIfChanged(kRegStep, kStepDetourNoMemory);
        return false;
    }

    g_hook_installed = true;
    const SelfCheck check = ValidateHook(arrange_address);
    if (!check.keep_hook)
    {
        //自检没过必须把“已挂钩”这个状态收回：否则下次再装会以为已经装好直接返回，
        //而且 explorer 里还留着指向我们代码的跳转。
        RestoreDetourBytes();
        g_hook_installed = false;
        SplitRegWriteDwordIfChanged(kRegStep, check.step != 0 ? check.step : kStepSelfCallMissed);
        return false;
    }
    //245 = 补丁保住了，但孤儿探针排不了版，自检这条路给不出结论；
    //这种时候不能报“挂钩成功”（他没看到过），也不能报“失败”（会去拆一个我们确信写进去了的补丁），
    //所以原样把 245 写出去，插件那句话会解释清楚。
    SplitRegWriteDwordIfChanged(kRegStep, check.step != 0 ? check.step : kStepHookInstalled);
    return true;
}

void RemoveTaskbarSplit()
{
    g_unloading = true;
    RestoreDetourBytes();
    g_hook_installed = false;
    g_seen_taskbar_button = false;
    g_block_width_dip = 0.0;
    for (auto& edge : g_button_right_edge_dip)
        edge = 0.0;
    g_last_published_dip = -1.0;
    g_edge_from_uia = false;
    SplitRegWriteDwordIfChanged(kRegStep, kStepNone);
}

void RestoreDetourBytes()
{
    //只还原代码字节：模块被 explorer 卸载前必须走到这里，否则残留的跳转会指向已经不存在的代码。
    //负边距不在这里还原（那时候已经没有 XAML 线程可用了），禁用时由 enabled=false 的那次排版还原。
    RemoveInlineDetour(g_detour);
    g_arrange_original = nullptr;
}

bool IsArrangeHookInstalled()
{
    return g_hook_installed.load();
}

bool IsTaskbarSplitWorking()
{
    return g_hook_installed.load() && g_seen_taskbar_button.load();
}
