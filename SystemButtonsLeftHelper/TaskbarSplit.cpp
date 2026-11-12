// Author: Ctrl-Ling
// Last Modified By: Ctrl-Ling
// Last Modified At: 2026-11-12 14:13:15 +08:00
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
//每个按钮"看得见"的左沿和宽度（DIP），下标同上。块宽只看右沿，但摆位置不看右沿：
//摆第 N 个按钮要的是"前面那 N-1 个各占多宽"，这个宽度必须用眼睛量出来的那份。
//为什么另起一格存：排位置那一路原来用 GetButtonExtent（第一个孩子的 DesiredSize），
//搜索切成"搜索图标和标签"以后，那个壳的 DesiredSize 还是空壳的 106 DIP，
//而屏幕上搜索框实际占 44~268 DIP —— 于是多任务视图被摆在 151 DIP（189 像素）处，
//正好压进搜索框肚子里，小组件跟着叠上去（就是他这张截图）。
static std::atomic<double> g_button_visible_left_dip[4]{ 0.0, 0.0, 0.0, 0.0 };
static std::atomic<double> g_button_visible_width_dip[4]{ 0.0, 0.0, 0.0, 0.0 };
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
//小组件元素现场：可见性（0=Visible 1=Collapsed 2=Hidden -1=还没量过）和 ActualWidth（1/100 DIP）。
//关掉小组件到底是"折叠"、"摘掉"还是"仍说可见但宽 0"，靠这两个数分辨，写进诊断那一行。
static std::atomic<int> g_widgets_visibility_state{ -1 };
static std::atomic<int> g_widgets_actual_width_100{ 0 };
//上一格"抹掉 / 不抹"的结论是哪一层判出来的（1~5=判成不在了，判据见 WidgetsGoneReason；0=判不出来所以维持旧值，-1=还没判过）。
//为什么要写出来：这台 shell 关掉小组件时元素自己照样报 Visible、宽 216，我们只看它自己永远判不出来，
//只能靠往上翻祖先。到底是祖先折叠、还是被排成 0 尺寸、还是整块挪出任务栏、还是几条都不认，
//注册表里一个 why= 就知道，不用为了试判据再编一轮。
static std::atomic<int> g_widgets_gone_reason{ -1 };
//顺带记一笔直系父亲的现场（可见性 / ActualWidth，1/100 DIP）：
//如果 why=0 还在，就看这两个数 —— 父亲说 Visible 且宽度还是满的，说明收起来的那层在更上面或压根不是靠折叠。
static std::atomic<int> g_widgets_parent_state{ -1 };
static std::atomic<int> g_widgets_parent_width_100{ 0 };
//最后一轮对小组件那一格的两条结论：ld= IsLoaded（1 在活树上 / 0 不在了 / -1 问不出来），
//ms= 这一次到底量到了没有（1 量到 / 0 没量到）。
//为什么两个都要：ms= 是为了把 why=0 的两种意思分开 —— "量到了所以不需要判" 和
//"判不出来所以维持旧值"在上一版长得一模一样，白看一眼。
static std::atomic<int> g_widgets_loaded_state{ -1 };
static std::atomic<int> g_widgets_measured_flag{ -1 };
//小组件按钮的"尺寸/挂载刚变"事件订在哪个实例上：同一个实例只订一次，换了实例（重新创建）就换人。
//光比 abi 指针不行：旧元素被回收后，新元素完全可能拿到同一个地址，那就漏订了，
//所以再存一份弱引用 —— 引用失效（旧元素死了）就说明指针相同也不可能是同一个东西，必须重订。
//只在任务栏线程访问（Arrange 和这些事件都在那里）。
static void* g_widgets_watched_abi{ nullptr };
static std::vector<winrt::weak_ref<FrameworkElement>> g_widgets_watched;     //就一个元素：订过的那个实例（和 g_touched_elements 同一套用法）
//父亲那层的订阅现场：直系父亲的 abi + 它的弱引用（和上面元素那一对同样的用法，防地址复用）。
//为什么要往外层订：关掉小组件时改的是外面那一层的尺寸，按钮自己那身现场一个字节都没变
//（可见性还是 Visible、ActualWidth 还是 216），所以它自己的 SizeChanged/Loaded 压根不会响 ——
//只订元素自己，"关闭"这一刀就叫不醒我们，块宽永远挂着开着时的数（TM 贴不回去）。
//父亲换人就往上把整条链重订一遍：上层可能重复订到同一个实例，
//多出来的只是一次重测（处理函数拿的是元素自己的弱引用，元素没了就直接返回），不值得再加一层记账。
static void* g_widgets_watched_up_abi{ nullptr };
static std::vector<winrt::weak_ref<FrameworkElement>> g_widgets_watched_up;
//往上翻几层祖先：判据（WidgetsGoneReason）和订阅（WatchWidgetsAncestors）必须同一个数，
//翻得比订得深没用（判出来也没人叫醒）、订得比翻得深浪费。
//12 层是留的余量：任务栏那棵树光按钮外面就套着 presenter/面板/根好几种壳，
//收起来的那一层到底在第几层没实测过，宁多勿少（这一层循环一次就是一个 GetParent，不花钱）。
static constexpr int kWidgetsAncestorWalk = 12;
static thread_local bool g_in_hook{ false };

//【片4 · 先认名字，再谈居中】把"不是那四个系统按钮"的控件类型名报出来。
//为什么必须先报名字：B 路线剩下的唯一一件事就是把应用图标那一组整组居中，
//而下这一刀之前必须知道三件事 —— 应用按钮的类名、它父亲是谁、排版器把它摆在 x=几、宽 w=几。
//这三个数一个都猜不得：猜错类名，偏移会打在托盘时钟/网络图标或者一堆 Grid 上，任务栏直接排坏。
//只收 Taskbar. 开头的那一族：任务栏自己的控件（应用图标、托盘、飞岛宿主）全在这一族里，
//而排版器每帧还要过成百上千个 Grid/Border/TextBlock，收进来只会把有用名字挤出限额。
//探测有预算：每过一次就扣一格，扣完就再也不问类型名（get_class_name 是一次 WinRT 调用，
//不该在 Arrange 这种每帧几十次的热路径上长期背着）。该看到的名字在头几帧就都看到了。
struct ClassNameRecord
{
    std::wstring name;
    std::wstring parent;
    long count{ 0 };
    int x_100{ 0 };
    int w_100{ 0 };
};
static constexpr size_t kClassNameMax = 12;
static constexpr long kClassNameProbeBudget = 20000;
static std::atomic<long> g_class_probe_budget{ kClassNameProbeBudget };
static std::mutex g_class_mutex;
static std::vector<ClassNameRecord> g_class_records;      //顺序 = 第一次出现的顺序

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
    PublishClasses();       //此刻报名字：让"应用图标那一组叫什么"这一行在挂钩刚证明自己能跑时就存在
}

//被我们动过边距的按钮，禁用时要一个个还原回去（用弱引用，元素被回收就自动失效）
static std::mutex g_touched_mutex;
static std::vector<winrt::weak_ref<FrameworkElement>> g_touched_elements;

static SystemButton IdentifySystemButton(FrameworkElement const& element);
static bool IsPinnedClusterButton(SystemButton button);
static bool RecordClassName(FrameworkElement const& element, Rect rect);
static void PublishClasses();
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

//排在前面这一格到底占多宽：优先用眼睛量出来的可见宽度（RecordButtonSpan 存的），
//量不到（这一格还没排过 / peer 拿不到）才退回第一个孩子的 DesiredSize。
//为什么要留退回那一路：第一轮排版时四个按钮谁都没量过，全 0 就会把后面三个挤进开始按钮里。
//任务栏竖着摆的时候不许用可见宽度：那一路量的是横坐标（左右边界），和竖排的"占多高"不是一回事，
//拿它去排只会把按钮排飞，所以竖排一律维持原来的 DesiredSize。
static double ClusterSlotWidth(PinnedChild const& child, bool vertical)
{
    if (!vertical)
    {
        const int index = static_cast<int>(child.button);
        if (index >= 0 && index < 4)
        {
            const double visible = g_button_visible_width_dip[index].load();
            if (visible > 8.0)
                return visible;
        }
    }
    return child.extent;
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
            offset += ClusterSlotWidth(c, vertical);
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

//这一组函数管一件事：眼睛看到的边界，以 UIA 的 BoundingRectangle 为准 —— 那才是屏幕上真正占位的那一块。
//为什么不能继续用排版矩形（X+Width）：实测四格里只有开始按钮对得上，其余全都偏：
//  搜索那个元素排出来宽是 0（报 44.80，可见搜索框其实在 44~268）；
//  多任务视图报 88.80，可见按钮其实在 88~132；
//  小组件按钮的矩形比天气图标宽出一大截（报 348.80，可见右边界只有 184）。
//也就是说排版矩形和可见内容是两套坐标，拿它当块宽、更拿它排后面按钮的位置，
//TM 一会儿压住按钮、一会儿空一大条，左侧几个按钮还会叠在一起。
//拿不到 peer、或算出个离谱的数，就退回排版矩形（保持原来的行为，不会更糟）。
//out_visible=false 表示这个按钮现在根本不占位：小组件在设置里关掉后元素还挂在树上，
//但已经折叠了，它自己再也不会来报"我不在了"，只能靠这个判断把它那一格抹掉。
//一个按钮"看得见"的范围：左右边界和宽度，都换算成相对任务栏左沿的 DIP。
//ok = 这一格现在占位；from_uia = 这几个数是可见边界（false 时只有 right 是调用方给的兜底排版右沿，
//宽度未知，不能拿它去排后面的按钮）；visible 由调用方的 out_visible 带出去。
struct VisibleSpan
{
    bool ok = false;
    bool from_uia = false;
    double left = 0.0;
    double right = 0.0;
    double width = 0.0;
};

//任务栏的左沿和它的窗口 DPI：UIA 给的是屏幕物理像素，必须减掉左沿、再按 DPI 折算，
//才是别的函数用的那套 DIP 坐标。拿不到就当这一轮没量到（调用方退回排版矩形）。
static bool TrayFrame(RECT* out_rc, UINT* out_dpi)
{
    const HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray == nullptr || !::GetWindowRect(tray, out_rc))
        return false;
    UINT dpi = ::GetDpiForWindow(tray);
    if (dpi < 96)
        dpi = 96;
    *out_dpi = dpi;
    return true;
}

//把一段物理像素边界并进 span（相对任务栏左沿、按窗口 DPI 折算成 DIP）。
//宽或高为 0 = 这块现在看不见，不收。
static bool UnionBoundsIntoSpan(Rect const& bounds, RECT const& tray_rc, UINT dpi, VisibleSpan* io)
{
    if (!std::isfinite(bounds.Width) || !std::isfinite(bounds.Height) ||
        bounds.Width <= 0.0 || bounds.Height <= 0.0)
        return false;

    const double scale = 96.0 / dpi;
    const double left = (static_cast<double>(bounds.X) - tray_rc.left) * scale;
    const double right = (static_cast<double>(bounds.X) + bounds.Width - tray_rc.left) * scale;
    //任务栏再宽也不会到十万 DIP，越界说明这个版本的 peer 给的坐标不是这个意思
    if (!std::isfinite(left) || !std::isfinite(right) || right <= 0.0 || right > 100000.0)
        return false;
    //只算"竖着落在任务栏那一条里"的：往下并孩子的时候，模板里可能挂着开始菜单/搜索面板那种
    //整屏大的内容宿主（它也在视觉树上，但它是弹出来的岛），并进来的话这一格会一口气报出半屏宽，
    //TM 直接被顶到屏幕中间去。判据就用最直白的：这块的中心点得在任务栏那条带子里。
    const double mid_y = static_cast<double>(bounds.Y) + bounds.Height / 2.0;
    if (mid_y < static_cast<double>(tray_rc.top) || mid_y >= static_cast<double>(tray_rc.bottom))
        return false;

    if (!io->ok)
    {
        io->ok = true;
        io->left = left;
        io->right = right;
    }
    else
    {
        if (left < io->left)
            io->left = left;
        if (right > io->right)
            io->right = right;
    }
    io->width = io->right - io->left;
    io->from_uia = true;
    return true;
}

//往下走两层，把孩子的可见边界并进父亲的范围里。
//为什么必须往下走：搜索那一格挂的是 Taskbar.TaskbarExtensionElement，它自己是个宽约 0 的空壳，
//真正撑开的是壳里面的搜索框按钮。实测（UIA 原始树，物理像素，1920 宽屏 @125%）：
//  开始 0~56、搜索 55~335、多任务视图 189~244、小组件 251~440。
//搜索那一格只量壳的话报的是 44.8~44.8（约等于没占位），于是两件事同时坏：
//  1) 多任务视图被摆在"开始+搜索的 DesiredSize"= 151 DIP（189 像素）处，正好压进搜索框肚子里；
//  2) 小组件跟着叠上去，块宽也少算一整个搜索框。
//只在"自己量出来不足 8 DIP"时才往下并孩子（开始/多任务/小组件自己就是可见那一块，一次都不多走）：
//Arrange 每帧都跑，不能每趟都把子树扫一遍。
static void UnionChildSpans(FrameworkElement const& element, RECT const& tray_rc, UINT dpi,
    VisibleSpan* io, int depth)
{
    if (depth > 2)
        return;
    try
    {
        const int count = xaml_media::VisualTreeHelper::GetChildrenCount(element);
        for (int i = 0; i < count && i < 8; i++)
        {
            auto child = xaml_media::VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>();
            if (!child)
                continue;
            try
            {
                auto peer = xaml_peers::FrameworkElementAutomationPeer::CreatePeerForElement(child);
                if (peer)
                    UnionBoundsIntoSpan(peer.GetBoundingRectangle(), tray_rc, dpi, io);
            }
            catch (...)
            {
            }
            UnionChildSpans(child, tray_rc, dpi, io, depth + 1);
        }
    }
    catch (...)
    {
    }
}

//只并"叶子"的可见边界：往下走，还有孩子的容器自己不并、只往里走。
//为什么小组件那一格要这么量：它的按钮边界是 251~440，可右边那 100 像素是什么都没画的点击热区
//（天气图标在 269~299、温度文字到 340 就结束）。按按钮边界报块宽，TM 就得在小组件后面空出 80 多像素；
//再赶上搜索框切成"图标和标签"、整组右移，TM 会直接压到第一个应用图标上。
//反过来，搜索那一格不许这么量：它的圆角底是模板里的 Border，UIA 里压根没这个孩子，
//只看叶子只剩放大镜和两个字（171~221），TM 会压进搜索框肚子里。
//所以这条只给小组件用，别的三格照旧用按钮自己的边界。
static void CollectLeafSpans(FrameworkElement const& element, RECT const& tray_rc, UINT dpi,
    VisibleSpan* io, int depth)
{
    int count = 0;
    try
    {
        if (depth <= 4)
            count = xaml_media::VisualTreeHelper::GetChildrenCount(element);
    }
    catch (...)
    {
        count = 0;
    }

    if (count <= 0)
    {
        try
        {
            auto peer = xaml_peers::FrameworkElementAutomationPeer::CreatePeerForElement(element);
            if (peer)
                UnionBoundsIntoSpan(peer.GetBoundingRectangle(), tray_rc, dpi, io);
        }
        catch (...)
        {
        }
        return;
    }

    for (int i = 0; i < count && i < 8; i++)
    {
        try
        {
            auto child = xaml_media::VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>();
            if (child)
                CollectLeafSpans(child, tray_rc, dpi, io, depth + 1);
        }
        catch (...)
        {
        }
    }
}

//VisibleWidgetsSpanDip 量不到叶子时要退回 VisibleSpanDip，那个函数定义在下面，先在这儿前置声明。
static VisibleSpan VisibleSpanDip(FrameworkElement const& element, double fallback_edge_dip,
    bool* out_visible);

//小组件那一格专用的"看得见"范围：先按叶子量，叶子一个都没有（这个版本的模板换了、
//或者它现在整个折叠着）才退回按钮自己的边界，再退回排版矩形。
//返回的 visible=false 含义和 VisibleSpanDip 一致：这一格现在不占位，该把格子抹掉。
static VisibleSpan VisibleWidgetsSpanDip(FrameworkElement const& element, double fallback_edge_dip,
    bool* out_visible)
{
    if (out_visible != nullptr)
        *out_visible = true;

    RECT tray_rc{};
    UINT dpi = 96;
    if (!TrayFrame(&tray_rc, &dpi))
    {
        VisibleSpan span;
        span.ok = true;
        span.right = fallback_edge_dip;
        return span;
    }

    VisibleSpan leaf;
    try
    {
        CollectLeafSpans(element, tray_rc, dpi, &leaf, 0);
    }
    catch (...)
    {
    }
    if (leaf.from_uia)
    {
        g_edge_from_uia.store(true);
        return leaf;
    }
    return VisibleSpanDip(element, fallback_edge_dip, out_visible);
}

static VisibleSpan VisibleSpanDip(FrameworkElement const& element, double fallback_edge_dip,
    bool* out_visible)
{
    if (out_visible != nullptr)
        *out_visible = true;

    VisibleSpan span;
    bool peer_ok = false;
    bool frame_ok = false;
    RECT tray_rc{};
    UINT dpi = 96;

    try
    {
        auto peer = xaml_peers::FrameworkElementAutomationPeer::CreatePeerForElement(element);
        if (peer)
            peer_ok = true;

        frame_ok = TrayFrame(&tray_rc, &dpi);

        if (peer_ok && frame_ok)
        {
            UnionBoundsIntoSpan(peer.GetBoundingRectangle(), tray_rc, dpi, &span);
            if (!span.from_uia || span.width < 8.0)
                UnionChildSpans(element, tray_rc, dpi, &span, 0);
        }
    }
    catch (...)
    {
    }

    if (span.from_uia)
    {
        g_edge_from_uia.store(true);
        return span;
    }

    //量不到任何可见边界，分两种：peer 和坐标都拿到、却连孩子都是空的 —— 这一格现在确实不占位
    //（小组件在设置里关掉就是这种：元素还挂在树上但整个折叠了），按"看不见"交回调用方去抹格子；
    //剩下那种（拿不到 peer / 拿不到任务栏 / 数离谱）才是真的没量到，退回排版矩形，行为跟以前一样。
    if (peer_ok && frame_ok)
    {
        if (out_visible != nullptr)
            *out_visible = false;
        return span;
    }

    span.ok = true;
    span.right = fallback_edge_dip;
    return span;
}

//【片4】记下一个"不是那四个系统按钮"的控件类型名，第一次见到就报一行。
//返回 true 只在"这一趟第一次见到这个名字"时成立：名字集合一旦稳定，这一行就不再重写，
//省得把每帧排版变成一次跨进程写盘（注册表写一次就会叫醒一次监视它的插件，那是白烧 CPU）。
//x=/w= 收的是排版器这一次给它的位置和宽度（1/100 DIP，相对任务栏左沿）：
//应用图标在 x 中间一段、托盘图标在 x 右端、飞岛宿主宽会顶到屏幕边，一列看下来谁是谁不用猜。
//p= 是直系父亲的类名：居中这一刀要么下在每个应用按钮自己身上、要么下在它们共同的那个容器上，
//父亲是谁决定哪一种成立，所以这个数必须和名字一起拿到。
static bool RecordClassName(FrameworkElement const& element, Rect rect)
{
    if (g_class_probe_budget.fetch_sub(1, std::memory_order_relaxed) <= 0)
        return false;

    std::wstring name;
    try
    {
        name = get_class_name(element);
    }
    catch (...)
    {
        return false;
    }
    if (name.rfind(L"Taskbar.", 0) != 0)
        return false;

    ClassNameRecord record;
    record.name = name;
    try
    {
        auto parent = xaml_media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
        if (parent)
            record.parent = get_class_name(parent);
    }
    catch (...)
    {
    }
    RECT tray_rc{};
    UINT tray_dpi = 96;
    const bool have_frame = TrayFrame(&tray_rc, &tray_dpi);
    const double scale = 96.0 / tray_dpi;
    record.x_100 = have_frame
        ? static_cast<int>(std::lround((static_cast<double>(rect.X) - tray_rc.left) * scale * 100.0))
        : static_cast<int>(std::lround(rect.X * 100.0));
    record.w_100 = static_cast<int>(std::lround(static_cast<double>(rect.Width) * 100.0));
    record.count = 1;

    std::lock_guard<std::mutex> lock(g_class_mutex);
    for (auto& item : g_class_records)
    {
        if (item.name == name)
        {
            item.count++;
            return false;
        }
    }
    if (g_class_records.size() >= kClassNameMax)
        return false;
    g_class_records.push_back(record);
    return true;
}

//把上面那张名字表拼成一行写进注册表。ID 只在第一次见到时留一次现场，
//所以这一行是给"下一轮编译前先看注册表"用的证据，不是运行时要读的东西。
static void PublishClasses()
{
    wchar_t buf[1024]{};
    int used = 0;

    std::lock_guard<std::mutex> lock(g_class_mutex);
    used = ::swprintf_s(buf, L"n=%d", static_cast<int>(g_class_records.size()));
    if (used < 0)
        return;
    for (auto& item : g_class_records)
    {
        if (used > 860)
            break;
        const int added = ::swprintf_s(buf + used, L" | %s c=%d x=%d w=%d p=%s",
            item.name.c_str(), static_cast<int>(item.count), item.x_100, item.w_100,
            item.parent.empty() ? L"?" : item.parent.c_str());
        if (added < 0)
            break;
        used += added;
    }
    SplitRegWriteStringIfChanged(kRegClasses, buf);
}

//四个按钮各自报上来的右边沿，一行写清楚。为什么要这一行：块宽只有一个数，
//"谁没报上来"和"谁报大了"看着一样（TM 窗口要么空一条要么压住按钮），
//有了四格现场值就能一次分清，省掉一轮编译。src 说明这些数是"看得见的边界"还是退回了排版矩形。
//n= 是这四个按钮各自累计经过挂钩几次（顺序还是 s/se/tv/w）：
//某一格是 0 就说明它根本没来排版（识别或事件没覆盖到），有次数却报 0 就是量到空边界。
//ev= 是小组件那一格收到的事件次数（它自己的 Unloaded/Loaded/SizeChanged + 祖先链的 SizeChanged 一起数）：
//光有 n= 时，"什么都没变"和"根本没发生"看着一样，所以元素自己的事件每次都顺手强制发一行 ——
//他按了开关而这一行没动，就是事件压根没来。（祖先那些只在真量出变化时才发，理由见 OnWidgetsEvent。）
//vis=/aw= 是小组件元素自己的现场（可见性和 ActualWidth，单位见 SplitContract.h），
//用来判断"关掉小组件"在系统里到底长什么样：折叠(1)/隐藏(2)/摘掉(靠 Unloaded)/还是仍可见但宽 0。
//why= 是"这一格该不该抹掉"最终采纳的判据（见 WidgetsGoneReason）；pv=/pw= 是直系父亲的现场，
//why=0（几条都不认）时靠这两个数看收起来的那一层到底在不在父亲身上。
//pub 不在这里重复了，"助手已挂钩，块宽 …"那一行给的就是同一个数。
//dw= 是这四个按钮各自"看得见"的宽度（开始/搜索/多任务/小组件，1/100 DIP）—— 摆位置那一路用的就是这四个数：
//第 N 个按钮摆在"前面几格宽度之和"处。搜索切成"图标和标签"时这里应当从 ~10600 跳到 ~22400，
//没跳就是可见宽度没量到（那一格还在用 DesiredSize 兜底），多任务和小组件会继续叠在搜索框肚子里。
//小组件那一格（dw 最后一位）量的是天气图标+温度文字，不含按钮右边那段没画东西的热区。
static void PublishEdges()
{
    wchar_t edges[256]{};
    ::swprintf_s(edges, L"s=%d se=%d tv=%d w=%d src=%s n=%d/%d/%d/%d ev=%d vis=%d aw=%d why=%d pv=%d pw=%d ld=%d ms=%d dw=%d/%d/%d/%d",
        static_cast<int>(std::lround(g_button_right_edge_dip[0].load() * 100.0)),
        static_cast<int>(std::lround(g_button_right_edge_dip[1].load() * 100.0)),
        static_cast<int>(std::lround(g_button_right_edge_dip[2].load() * 100.0)),
        static_cast<int>(std::lround(g_button_right_edge_dip[3].load() * 100.0)),
        g_edge_from_uia.load() ? L"uia" : L"rect",
        static_cast<int>(g_button_arrange_count[0].load()),
        static_cast<int>(g_button_arrange_count[1].load()),
        static_cast<int>(g_button_arrange_count[2].load()),
        static_cast<int>(g_button_arrange_count[3].load()),
        static_cast<int>(g_widgets_event_count.load()),
        g_widgets_visibility_state.load(),
        g_widgets_actual_width_100.load(),
        g_widgets_gone_reason.load(),
        g_widgets_parent_state.load(),
        g_widgets_parent_width_100.load(),
        g_widgets_loaded_state.load(),
        g_widgets_measured_flag.load(),
        static_cast<int>(std::lround(g_button_visible_width_dip[0].load() * 100.0)),
        static_cast<int>(std::lround(g_button_visible_width_dip[1].load() * 100.0)),
        static_cast<int>(std::lround(g_button_visible_width_dip[2].load() * 100.0)),
        static_cast<int>(std::lround(g_button_visible_width_dip[3].load() * 100.0)));
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
        if (!present[i])
        {
            //这一格不在树上了，宽度那一路也要一起清掉：不清的话后面几个按钮还会
            //按"它还在占 224 DIP"往后排，左边就空出一条谁都不占的位子。
            g_button_visible_left_dip[i] = 0.0;
            g_button_visible_width_dip[i] = 0.0;
        }
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

//记下某个按钮"看得见"的左沿和宽度：块宽用右沿（走 UpdateBlockEnd），
//摆后面按钮的位置用这里的宽度（走 ClusterSlotWidth）。量不到就把这两格清零，
//让后面那一格退回 DesiredSize 那条路 —— 宁可沿用系统的排法，也别拿一个 0 宽去排，
//那会把多任务和小组件全叠到开始按钮上。
static void RecordButtonSpan(SystemButton button, VisibleSpan const& span)
{
    const int index = static_cast<int>(button);
    if (index < 0 || index >= 4)
        return;
    if (!span.from_uia || span.width <= 0.0)
    {
        g_button_visible_left_dip[index] = 0.0;
        g_button_visible_width_dip[index] = 0.0;
        return;
    }
    g_button_visible_left_dip[index] = span.left;
    g_button_visible_width_dip[index] = span.width;
}

//某个按钮报上自己“看得见”的右边沿（DIP）：把它那一格记下来，块宽取四格里最大的那格。
//数的来源见上面 VisibleSpanDip 的说明（UIA 边界为准，排版矩形只当兜底）；
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
    if (index < 0 || index >= 4)
        return;
    g_button_visible_left_dip[index] = 0.0;
    g_button_visible_width_dip[index] = 0.0;
    if (g_button_right_edge_dip[index].load() == 0.0)
        return;
    g_button_right_edge_dip[index] = 0.0;
    PublishBlockFromSlots();
}

//可见性折成诊断用的一个整数：0=Visible 1=Collapsed 2=其余（枚举只有三个值，不是前两个就只剩这一个）。
//为什么要绕这一圈：Windows SDK 里有个叫 Hidden 的宏（mciapi 那套），
//写 Visibility::Hidden 会被展开成整数、直接 C2838/C2065 —— CI 踩过，别再拼那个名字。
static int VisibilityCode(Visibility visibility)
{
    if (visibility == Visibility::Visible)
        return 0;
    if (visibility == Visibility::Collapsed)
        return 1;
    return 2;
}

//把小组件元素的现场（可见性 + ActualWidth）记一笔，供诊断那一行打印。
//为什么要记：上一版就是靠这两个数看出"关掉小组件后按钮自己还是 Visible/宽 216"，
//从而把病根从"事件没来"改判到"眼神不对"。留着它们，下次再错也是一行注册表的事。
static void RecordWidgetsState(FrameworkElement const& element)
{
    try
    {
        g_widgets_visibility_state = VisibilityCode(element.Visibility());
        g_widgets_actual_width_100 =
            static_cast<int>(std::lround(NotNegative(element.ActualWidth()) * 100.0));
    }
    catch (...)
    {
    }
}

//直系父亲的现场（可见性 + ActualWidth）。
//why=0（几条判据都不认）时就靠这两个数：父亲说 Visible 且宽度还是满的，
//说明收起来的那一层不在父亲身上（在更上面，或者根本不用折叠这一招），得换眼神；
//父亲宽度已经是 0，那不管它是 Hidden 还是被排成 0 宽，都算"这格不占地方了"。
static void RecordParentState(FrameworkElement const& parent)
{
    try
    {
        g_widgets_parent_state = VisibilityCode(parent.Visibility());
        g_widgets_parent_width_100 =
            static_cast<int>(std::lround(NotNegative(parent.ActualWidth()) * 100.0));
    }
    catch (...)
    {
    }
}

//元素还在不在"活树"上：1=在（已加载），0=不在了，-1=问不出来（当成在，别乱抹）。
//这是比可见性、比 UIA 边界都更硬的一条判据：FrameworkElement.IsLoaded 只有在元素挂在一个
//已加载的根上时才为真，被从树上摘掉、被塞回模板缓存、岛被拆掉 —— 全都立刻变 false，
//而它的 ActualWidth 和 peer 边界却可能原封不动地留着旧值（实测就是：关掉小组件后
//vis 还是 Visible、aw 还是 216、peer 还能给出一个 291 DIP 的右边界，全是死人的遗照）。
//所以这一条要在信任任何测量结果之前问一遍，否则那一格会被一个已死元素的旧边界一直续命。
static int WidgetsLoadedCode(FrameworkElement const& element)
{
    try
    {
        return element.IsLoaded() ? 1 : 0;
    }
    catch (...)
    {
        return -1;      //这个版本的投影没有这个属性/问不出来：不许据此抹，维持原判
    }
}

//小组件到底"在不在"，判出来的是哪一种情况（写进诊断 why=，一编译就能看到这台 shell 用的是哪一招）：
//  6 = 元素已经不在活树上了（IsLoaded=false）—— 最硬的一条，见 WidgetsLoadedCode；
//  1 = 它自己 Visibility 不是 Visible；
//  2 = 往上某一层祖先被折叠了 —— 上一版怀疑的就是这个：关掉小组件后按钮自己还是 vis=0、aw=21600；
//  3 = UIA 给了个非空边界，但整个在任务栏之外（被挪走了，不是"暂时量不到"）；
//  4 = 祖先虽然写着 Visible，却被排成某个方向 0 尺寸（不占地方，但不是用折叠做到的）；
//  5 = （这条不在这儿产生，是 OnWidgetsEvent 记的）Unloaded —— 元素整个从树上摘掉了；
//  0 = 一条都没判出来 —— 那才是出现动画的中间态，维持上一次的值，等下一个事件重量。
//为什么进这个函数要先"量不到"：只有 VisibleWidgetsSpanDip 说看不见（自己和叶子都是空的）才来问原因，
//量得到就直接用量的数，免得判据把好端端的现场误抹掉。
//为什么不看 ActualWidth 或"边界为空"就断定不在了：出现动画那一下它们本来就是 0，
//照着抹等于把"刚打开、马上就有"当成"不在了"，块宽停在没开的那个数 —— TM 压住小组件。
//注：UWP（Windows.UI.Xaml）的投影里没有 FrameworkElement.IsVisible()，那是 WinUI3 才有的（CI 报过 C2039）；
//IsLoaded 不一样，它是 UWP 的 FrameworkElement 上就有的只读属性。万一这个投影也不认，
//CI 会报 C2039 'IsLoaded'，那就把 WidgetsLoadedCode 整块删掉（第 6 层判据作废），其余几层照编。
//这里用的 GetParent/try_as/Visibility/ActualWidth/GetBoundingRectangle/FindWindow 都是本文件已经编过的写法。
static int WidgetsGoneReason(FrameworkElement const& element)
{
    try
    {
        if (WidgetsLoadedCode(element) == 0)
            return 6;

        if (VisibilityCode(element.Visibility()) != 0)
            return 1;

        auto current = element;
        for (int depth = 0; depth < kWidgetsAncestorWalk; depth++)
        {
            auto parent = xaml_media::VisualTreeHelper::GetParent(current).try_as<FrameworkElement>();
            if (!parent)
                break;
            if (depth == 0)
                RecordParentState(parent);
            if (VisibilityCode(parent.Visibility()) != 0)
                return 2;
            //祖先被排成"某个方向一点地方都不占"：它肚子里那个还挂着 216 宽的小组件按钮
            //不可能真的在屏幕上，那个 216 是上一次布局留下的旧值，别信。
            //0 高/0 宽两种都算（任务是横着还是竖着摆的，这里不用管）。
            if (parent.ActualWidth() <= 0.0 || parent.ActualHeight() <= 0.0)
                return 4;
            current = parent;
        }

        auto peer = xaml_peers::FrameworkElementAutomationPeer::CreatePeerForElement(element);
        if (peer)
        {
            const Rect bounds = peer.GetBoundingRectangle();
            const HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
            RECT tray_rc{};
            if (tray != nullptr && ::GetWindowRect(tray, &tray_rc) &&
                std::isfinite(bounds.Width) && std::isfinite(bounds.Height) &&
                bounds.Width > 0.0 && bounds.Height > 0.0)
            {
                const double right = static_cast<double>(bounds.X) + bounds.Width;
                const double bottom = static_cast<double>(bounds.Y) + bounds.Height;
                if (right <= static_cast<double>(tray_rc.left) ||
                    static_cast<double>(bounds.X) >= static_cast<double>(tray_rc.right) ||
                    bottom <= static_cast<double>(tray_rc.top) ||
                    static_cast<double>(bounds.Y) >= static_cast<double>(tray_rc.bottom))
                    return 3;
            }
        }
    }
    catch (...)
    {
    }
    return 0;
}

//量一次小组件的右边界并记账（它自己的事件、它祖先的事件、和它自己走到 Arrange，三条路都走这里）。
//量得到就报；量不到按 WidgetsGoneReason 的几条判据决定"抹掉那一格"还是"维持上一次的值"。
//fallback_edge_dip 是排版矩形算出来的边界（只有 Arrange 那一路有；事件里传 0 = 不去猜）。
static void RepublishWidgetsEdge(FrameworkElement const& element, double fallback_edge_dip)
{
    if (!g_enabled_cache.load())
        return;
    RecordWidgetsState(element);
    //第一问不是"能不能量到"，而是"这个元素还挂在活树上吗"：被摘掉的元素照样能报出
    //上一个版本的 ActualWidth 和 UIA 边界（见 WidgetsLoadedCode 里那组实测数字），
    //让它过这一关就等于一个死按钮天天给自己那一格续命，块宽永远缩不回去。
    const int loaded = WidgetsLoadedCode(element);
    bool visible = true;
    //这一格按"看得见的叶子"量（天气图标 + 温度文字），不按按钮自己的边界量：
    //按钮边界 251~440 里右边那 100 像素是什么都没画的热区，照着它报块宽，
    //搜索框一切成"图标和标签"、整组右移之后，TM 会一路压到第一个应用图标上。
    const VisibleSpan span = VisibleWidgetsSpanDip(element, fallback_edge_dip, &visible);
    const double edge = visible ? span.right : 0.0;
    RecordButtonSpan(SystemButton::Widgets, span);
    const bool measured = (loaded != 0 && visible && edge > 0.0);
    const int reason = measured ? 0 : WidgetsGoneReason(element);
    const int last_reason = g_widgets_gone_reason.exchange(reason);
    //ms= 把两种 why=0 分开：量到了(1) vs 几条判据都不认(0)。上一版这两个都写 0，
    //结果你看到 why=0 根本分不清是"它明明还在"还是"我眼神不够"。
    g_widgets_measured_flag = measured ? 1 : 0;
    g_widgets_loaded_state = loaded;

    if (measured)
        UpdateBlockEnd(SystemButton::Widgets, edge);
    else if (reason != 0)
        ClearBlockEnd(SystemButton::Widgets);

    //判据结论变了、块宽却没变（那一格本来就是 0，抹不抹出来的数一样）：
    //上面两条路都不会写注册表，why= 就出不来，只能靠"没变化"去猜。这里补发一行。
    if (last_reason != reason)
        PublishEdges();
}

//别的按钮（开始/搜索/多任务视图）每次排版，顺手把小组件那一格重量一遍 —— 这一条才是治"关闭不回填"的。
//为什么非借它们的节拍：实测关掉小组件这件事，小组件自己那一路一条事件都没送到（ev 一直停在 3），
//祖先那一路也指望不上 —— 它父亲就是整条任务栏那么宽（pw=153600 = 1536 DIP）的面板，
//少掉最右边一个孩子，这种面板的尺寸根本不会变，SizeChanged 自然一声不响。
//而这三个按钮每次任务栏重排都要走我们的 Arrange（n=9/9/9），是现成、便宜、方向正确的节拍器：
//小组件关掉后必然有一次任务栏重排，那一次就够我们把这一格判掉、抹掉、重发块宽。
//什么时候不查：那一格本来就是 0（没开着小组件，没什么可抹的）—— 省掉每轮排版一次 UIA。
static void RefreshWidgetsSlotFromSiblings()
{
    if (g_widgets_watched.empty())
        return;
    if (g_button_right_edge_dip[static_cast<int>(SystemButton::Widgets)].load() == 0.0)
        return;
    try
    {
        auto element = g_widgets_watched[0].get();
        if (!element)
            return;
        //传 0 而不是排版矩形：这本来就是"别人在排版，我顺带看一眼小组件还在不在"，
        //拿别人的矩形去猜它的边界只会把错的数再坐实一遍。
        RepublishWidgetsEdge(element, 0.0);
    }
    catch (...)
    {
    }
}

//小组件事件的统一处理：记一笔 ev、按类型补账，然后（元素自己那一路）无条件发一行现场。
//为什么"无条件发"：只有 ev 变了才保证注册表被动一次，这样"我按了开关但什么都没发生"
//在注册表里是看得见的（那一行不动），而不是只能靠"数字没变"去猜。
//为什么祖先那一路不无条件发：祖先（往上好几层，最外面就是整条任务栏）的尺寸变化很频繁，
//每来一次就写一次注册表 = 每来一次就把插件从 RegNotifyChangeKeyValue 上叫醒一遍，
//那是拿任务栏的日常抖动去骚扰另一个进程。祖先事件照样量、照样判，
//只有真的量出变化（那一格的边界变了 / 判据结论变了）才写盘 —— 该叫得动的时候一定叫得动。
//OnWidgetsEvent 最后一个参数的两种取法（写在调用处比裸 true/false 好读）：
static constexpr bool kForceLine = true;      //元素自己的事件：无条件发一行现场（他按开关就看这一行动没动）
static constexpr bool kQuietLine = false;     //祖先的事件：只在真量出变化时发（任务栏天天在改尺寸，别拿它吵醒插件）
static void OnWidgetsEvent(winrt::weak_ref<FrameworkElement> const& weak, bool unloaded, bool announce)
{
    g_widgets_event_count.fetch_add(1, std::memory_order_relaxed);
    try
    {
        auto element = weak.get();
        if (!element)
            return;
        //只认"当前在跟的那个元素"：按钮换了实例（任务栏重建）以后，旧元素身上和它祖先身上
        //挂着的旧处理器还会晚点到几次 —— 让旧元素那身过时现场去量、去抹那一格，比不量还糟。
        if (g_widgets_watched_abi != nullptr && winrt::get_abi(element) != g_widgets_watched_abi)
            return;
        if (unloaded)
        {
            g_widgets_gone_reason = 5;                  //5 = 元素整个从树上摘掉了，不用再问可见性
            ClearBlockEnd(SystemButton::Widgets);       //那一格不能留
        }
        else
            RepublishWidgetsEdge(element, 0.0);
    }
    catch (...)
    {
    }
    if (announce)
        PublishEdges();
}

//给小组件的祖先链订一次"尺寸变了"。为什么非得订外层（这是这一版的关键）：
//元素自己的三个事件只能覆盖"它自己变了"，可实测关掉小组件后它自己那身现场一动不动
//（vis 还是 0=Visible、aw 还是 21600），缩起来的是外面某一层 —— 那一层缩到 0 尺寸，
//元素这边什么都听不到，于是块宽一直挂着"开着"的 291，TM 就贴不回多任务视图右边。
//只订 SizeChanged 这一件事：折叠、被排成 0 宽、被撑回去，任何一种都必然路过它；
//可见性变化在 UWP 上没有事件可订，但改可见性一定改尺寸，效果一样（不用定时器）。
//处理函数拿的还是"元素自己"的弱引用：外面哪一层变了都不要紧，要量的始终是小组件的右边界。
static void WatchWidgetsAncestors(FrameworkElement const& element)
{
    auto parent = xaml_media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
    if (!parent)
        return;

    void* parent_abi = winrt::get_abi(parent);
    if (g_widgets_watched_up_abi == parent_abi && !g_widgets_watched_up.empty())
    {
        try
        {
            auto alive = g_widgets_watched_up[0].get();
            if (alive && winrt::get_abi(alive) == parent_abi)
                return;         //还是那个父亲，已经订过了
        }
        catch (...)
        {
        }
    }

    g_widgets_watched_up_abi = nullptr;
    g_widgets_watched_up.clear();
    try
    {
        winrt::weak_ref<FrameworkElement> weak{ element };
        auto current = element;
        for (int depth = 0; depth < kWidgetsAncestorWalk; depth++)
        {
            auto up = xaml_media::VisualTreeHelper::GetParent(current).try_as<FrameworkElement>();
            if (!up)
                break;
            if (depth == 0)
                g_widgets_watched_up.emplace_back(up);        //父亲留一份弱引用，当作"订过了"的凭据
            up.SizeChanged([weak](auto&&, auto&&) { OnWidgetsEvent(weak, false, kQuietLine); });
            current = up;
        }
        if (!g_widgets_watched_up.empty())
            g_widgets_watched_up_abi = parent_abi;
    }
    catch (...)
    {
        g_widgets_watched_up_abi = nullptr;
        g_widgets_watched_up.clear();
    }
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
//返回"这一轮真的重订了"（true）：祖先那一路只在换实例的时候跟着重订一次，理由见 WatchWidgetsEvents。
static bool WatchWidgetsSelf(FrameworkElement const& element)
{
    void* abi = winrt::get_abi(element);

    //同一个实例只订一次（Arrange 每轮都会走到这里）。光比指针不够：旧元素被回收后，
    //新元素完全可能拿到同一个地址，那就漏订了 —— 所以还要弱引用能把同一个对象取回来才算数，
    //取不出来就说明旧元素早没了，必须重订。
    if (g_widgets_watched_abi == abi && !g_widgets_watched.empty())
    {
        try
        {
            auto alive = g_widgets_watched[0].get();
            if (alive && winrt::get_abi(alive) == abi)
                return false;
        }
        catch (...)
        {
        }
    }

    g_widgets_watched_abi = nullptr;
    g_widgets_watched.clear();
    //元素换了实例，祖先那一路的记账一起作废：那批处理器手里攥的是旧元素的弱引用，
    //不清掉这道闸，同一父亲就直接早退、新元素永远无人叫醒。
    g_widgets_watched_up_abi = nullptr;
    g_widgets_watched_up.clear();
    try
    {
        //弱引用：元素被回收就自动失效，不能让它为了听事件而永远活着
        winrt::weak_ref<FrameworkElement> weak{ element };

        //摘掉（小组件在设置里关掉，这个按钮就从树上消失了）：那一格必须清零
        element.Unloaded([weak](auto&&, auto&&) { OnWidgetsEvent(weak, true, kForceLine); });

        //挂回来（重新打开小组件、任务栏重建）：出现动画这一步可能还量不到，
        //但紧接着一定会有 SizeChanged，那里再量一次
        element.Loaded([weak](auto&&, auto&&) { OnWidgetsEvent(weak, false, kForceLine); });

        //刚落定一次尺寸（出现动画走完、天气文字变长变短、整组折叠成 0）：重量一次
        element.SizeChanged([weak](auto&&, auto&&) { OnWidgetsEvent(weak, false, kForceLine); });

        //三个都订上了才记账：半途抛了就当作没订，下一轮排版重来
        g_widgets_watched.emplace_back(element);
        g_widgets_watched_abi = abi;
        return true;
    }
    catch (...)
    {
        g_widgets_watched_abi = nullptr;
        g_widgets_watched.clear();
        return false;
    }
}

//把"元素自己"和"它的祖先链"两层订阅串起来。
//为什么祖先那一路跟着"元素换实例"走，而不是每轮排版都查一遍父亲：
//Arrange 每轮都进来，一旦父亲那个身份判定哪天对不上（同一个原生元素被投影成两个包装对象之类），
//每轮就白订 12 个处理器 —— 任务栏按钮的处理器只增不减，那是个越滚越大的漏。
//而祖先本来就只有在新实例出现时才需要重订，元素这一层已经把那个时机报得准准的了。
static void WatchWidgetsEvents(FrameworkElement const& element)
{
    if (!WatchWidgetsSelf(element))
        return;
    try
    {
        WatchWidgetsAncestors(element);
    }
    catch (...)
    {
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
                //量 + 判 + 记账全走 RepublishWidgetsEdge（和它自己的事件、祖先的事件同一条路，
                //省得两处判据以后各改各的）。这里把排版矩形交进去当兜底：
                //UIA 拿不到边界时才用它，拿得到的一律用"看得见的边界"。
                RepublishWidgetsEdge(element, static_cast<double>(rect.X) + rect.Width);
            }
            else
                ClearBlockEnd(SystemButton::Widgets);
            return g_arrange_original(p_this, rect);
        }

        if (!IsPinnedClusterButton(button))
        {
            //不是那四个系统按钮的一切（应用图标、托盘、外壳自己的容器）：
            //先把类型名报上去，让"居中该下在哪个元素上"这件事用一行注册表回答，别靠猜（见 RecordClassName）。
            if (button == SystemButton::None && RecordClassName(element, rect))
                PublishClasses();
            return g_arrange_original(p_this, rect);
        }

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
        //借这一轮排版把小组件那一格也复核一次（关闭小组件时它是唯一会响的节拍，理由见函数上那段）
        RefreshWidgetsSlotFromSiblings();

        Rect new_rect = rect;
        //把按钮摆到“前面几个按钮排完之后”的位置
        if (vertical)
            new_rect.Y = static_cast<float>(offset);
        else
            new_rect.X = static_cast<float>(offset);

        //块宽记的是这个按钮“看得见”的右边沿（排版矩形只当作拿不到 peer 时的兜底）：
        //实测搜索那一格排出来宽是 0、多任务视图的可见图标比它的排版矩形整整右移一个按钮位，
        //照着排版矩形报，块宽就永远比屏幕上的按钮窄一截，TM 的窗口会压住按钮。
        //同一趟里把这个按钮可见的左沿和宽度也记下来，下一轮排后面的按钮时就按这个宽度让位子
        //（见 ClusterSlotWidth）—— 上面那个"多任务叠进搜索框"的毛病就是这两路用了两套宽度。
        bool visible = true;
        const double rect_edge = static_cast<double>(new_rect.X) + new_rect.Width;
        const VisibleSpan span = VisibleSpanDip(element, rect_edge, &visible);
        RecordButtonSpan(button, span);
        if (visible)
            UpdateBlockEnd(button, span.right);
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
