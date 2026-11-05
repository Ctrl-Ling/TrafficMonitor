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
static thread_local bool g_in_hook{ false };

//第一次真的看到任务栏按钮经过挂钩，说明“钉住”这件事在当前的 explorer 里成立了，
//这一刻才把 active/pid 报出去（装配完成的当场还不能确定，因为要等一次真正的排版）。
//注册表监视是双向的，所以这里用“变了才写”的接口，避免自己叫醒自己空转。
static void ReportWorkingOnce()
{
    if (g_seen_taskbar_button.exchange(true))
        return;
    SplitRegWriteDwordIfChanged(kRegActive, 1u);
    SplitRegWriteDwordIfChanged(kRegPid, ::GetCurrentProcessId());
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
static double ComputeClusterOffset(FrameworkElement const& frame, SystemButton target, bool vertical)
{
    std::vector<PinnedChild> children;
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
            if (enabled)
            {
                auto frame = xaml_media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
                if (frame)
                {
                    bool vertical = IsVerticalTaskbar(frame);
                    double end = ComputeClusterOffset(frame, SystemButton::Widgets, vertical) +
                        GetButtonExtent(element, vertical);
                    double current = g_block_width_dip.load();
                    if (end > current)
                    {
                        g_block_width_dip = end;
                        PublishBlockWidth(end);
                    }
                }
            }
            return g_arrange_original(p_this, rect);
        }

        if (!IsPinnedClusterButton(button))
            return g_arrange_original(p_this, rect);     //应用图标等一律不动

        ReportWorkingOnce();
        RememberTouched(element);

        if (ClusterMarginNeedsUpdate(element, enabled))
            PostToTaskbarThread(element, UpdateClusterCollapse, enabled);

        if (!enabled)
            return g_arrange_original(p_this, rect);

        auto frame = xaml_media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
        if (!frame)
            return g_arrange_original(p_this, rect);

        bool vertical = IsVerticalTaskbar(frame);
        double offset = ComputeClusterOffset(frame, button, vertical);
        double end = offset + GetButtonExtent(element, vertical);

        Rect new_rect = rect;
        if (vertical)
        {
            new_rect.Y = static_cast<float>(offset);
            if (end > g_block_width_dip.load())
                g_block_width_dip = end;
        }
        else
        {
            new_rect.X = static_cast<float>(offset);
            if (end > g_block_width_dip.load())
                g_block_width_dip = end;
        }
        PublishBlockWidth(g_block_width_dip.load());

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

//调用一个可能随时崩的函数：只用裸指针，出异常就返回 false
static bool SafeCall(Arrange_func func, void* abi, Rect rect, HRESULT* hr)
{
    __try
    {
        *hr = func(abi, rect);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

//自检：通过“已经被改过的虚表槽”调用一次 Arrange，确认我们的函数真的被执行了，
//并且原函数开头被搬走的指令在 trampoline 上还能正常跑完。
static bool ValidateHook()
{
    try
    {
        xaml_shapes::Rectangle probe;
        UIElement ui_element{ probe };
        void* abi = winrt::get_abi(ui_element);
        void** vtable = *reinterpret_cast<void***>(&abi);
        Arrange_func slot = reinterpret_cast<Arrange_func>(vtable[kIUIElementArrangeSlot]);

        long before = g_arrange_call_count.load();
        HRESULT hr{};
        if (!SafeCall(slot, abi, Rect{ 0.0f, 0.0f, 40.0f, 40.0f }, &hr))
            return false;
        if (g_arrange_call_count.load() == before)
            return false;       //没走进我们的函数，说明这个槽位不是（或者没生效）

        //再直接走一遍 trampoline，确保搬走的指令是好的
        if (!SafeCall(g_arrange_original, abi, Rect{ 0.0f, 0.0f, 42.0f, 42.0f }, &hr))
            return false;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool InstallTaskbarSplit()
{
    if (g_hook_installed.load())
        return true;

    try
    {
        g_unloading = false;
        xaml_shapes::Rectangle rectangle;
        UIElement ui_element{ rectangle };
        void** vtable = *reinterpret_cast<void***>(winrt::get_abi(ui_element));
        void* arrange_address = vtable[kIUIElementArrangeSlot];
        if (arrange_address == nullptr || !IsInXamlModule(arrange_address))
            return false;

        if (!InstallInlineDetour(g_detour, arrange_address, reinterpret_cast<void*>(&ArrangeHook)))
            return false;
        g_arrange_original = reinterpret_cast<Arrange_func>(g_detour.trampoline);
        if (g_arrange_original == nullptr)
        {
            RestoreDetourBytes();
            return false;
        }

        g_hook_installed = true;
        if (!ValidateHook())
        {
            RestoreDetourBytes();
            return false;
        }
        return true;
    }
    catch (...)
    {
        RestoreDetourBytes();
        return false;
    }
}

void RemoveTaskbarSplit()
{
    g_unloading = true;
    RestoreDetourBytes();
    g_hook_installed = false;
    g_seen_taskbar_button = false;
    g_block_width_dip = 0.0;
    g_last_published_dip = -1.0;
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
