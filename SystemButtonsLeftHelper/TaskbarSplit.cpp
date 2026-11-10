// Author: Ctrl-Ling
// Last Modified By: Ctrl-Ling
// Last Modified At: 2026-11-10 10:55:00 +08:00
#include "framework.h"
#include "TaskbarSplit.h"
#include "detour.h"
#include "SplitContract.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
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

static void PublishBlockWidth(double width_dip)
{
    //宽度基本没变就不写注册表（每写一次就是一次跨进程通知）
    if (std::abs(width_dip - g_last_published_dip.load()) < 0.5)
        return;
    g_last_published_dip = width_dip;
    DWORD block = static_cast<DWORD>(std::lround(width_dip * 100.0));
    SplitRegWriteDword(kRegBlock, block);

    //四个按钮各自报上来的右边沿，一行写清楚。为什么要这一行：块宽只有一个数，
    //"谁没报上来"和"谁报大了"看着一样（TM 窗口要么空一条要么压住按钮），
    //有了四格现场值就能一次分清，省掉一轮编译。
    wchar_t edges[128]{};
    ::swprintf_s(edges, L"s=%d search=%d tv=%d w=%d pub=%d",
        static_cast<int>(std::lround(g_button_right_edge_dip[0].load() * 100.0)),
        static_cast<int>(std::lround(g_button_right_edge_dip[1].load() * 100.0)),
        static_cast<int>(std::lround(g_button_right_edge_dip[2].load() * 100.0)),
        static_cast<int>(std::lround(g_button_right_edge_dip[3].load() * 100.0)),
        static_cast<int>(block));
    SplitRegWriteStringIfChanged(kRegEdges, edges);
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

//某个按钮报上自己的右边沿（DIP）：把它那一格记下来，块宽取四格里最大的那格。
//为什么用 Arrange 矩形自己的 X+Width：排版器把按钮放到这个矩形里，最终就是这个矩形说了算；
//以前是拿 ComputeClusterOffset + GetButtonExtent 另算一套（DesiredSize 会跟着天气文字、悬停宽度上下飘），
//而且只会变大不会变小 —— 实测注册表里长期挂着 348，而小组件真实右边沿只有 285，
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

        if (button == SystemButton::Widgets)
        {
            ReportWorkingOnce();
            RememberTouched(element);
            if (WidgetsPositionNeedsUpdate(element, enabled))
                PostToTaskbarThread(element, UpdateWidgetsPosition, enabled);
            //小组件是这一组里最右边的一个，块宽基本就是它的右边沿。
            //这里直接用排版器给它的矩形（X+Width），不再自己按 DesiredSize 估：
            //估出来的值会跟着天气文字的显示/隐藏上下飘，而且旧写法只增不减，
            //结果注册表里长期挂着一个比实际大 60 多 DIP 的数，TM 的窗口就一会儿空一条、一会儿压住小组件。
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
                UpdateBlockEnd(SystemButton::Widgets, rect.X + rect.Width);
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
        //把按钮摆到“前面几个按钮排完之后”的位置；块宽就记这个矩形自己的右边沿
        if (vertical)
        {
            new_rect.Y = static_cast<float>(offset);
            UpdateBlockEnd(button, static_cast<double>(new_rect.X) + new_rect.Width);
        }
        else
        {
            new_rect.X = static_cast<float>(offset);
            UpdateBlockEnd(button, static_cast<double>(new_rect.X) + new_rect.Width);
        }

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
