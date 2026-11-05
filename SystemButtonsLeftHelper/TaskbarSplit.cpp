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
    SplitRegWriteDwordIfChanged(kRegStep, kStepPinning);
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

//自检现场写进注册表（TaskbarSplitSelfTest）：哪一步、计数加了几、返回码。
//“没进钩子”和“进了钩子但跑飞了”看着都是“没生效”，做法却完全相反，只能靠这两个数字分。
static void PublishSelfTest(const wchar_t* stage, long call_delta, HRESULT hr)
{
    wchar_t text[128]{};
    ::swprintf_s(text, L"%s calls=%ld hr=0x%08lX", stage, call_delta, static_cast<unsigned long>(hr));
    SplitRegWriteStringIfChanged(kRegSelfTest, text);
}

//槽地址和实际下补丁的地址一起写出来：两者不一致，就说明调用根本不走我们改的地方。
//M:=槽地址所在模块的加载基址 —— S 减 M 就是这个函数在这个 dll 里的偏移，
//拿这个偏移去查微软公开符号（dbghelp SymFromAddr）就能直接问出“槽位 92 到底叫什么名字”。
static void PublishTargetAddresses(void* slot_address, const InlineDetour& detour)
{
    uintptr_t module_base = 0;
    HMODULE module{};
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(slot_address), &module) && module != nullptr)
        module_base = reinterpret_cast<uintptr_t>(module);
    wchar_t text[160]{};
    ::swprintf_s(text, L"S:0x%016llX T:0x%016llX M:0x%016llX",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(slot_address)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(detour.target)),
        static_cast<unsigned long long>(module_base));
    SplitRegWriteStringIfChanged(kRegTarget, text);
}

//自检：通过“已经被改过的虚表槽”调用一次 Arrange，确认我们的函数真的被执行了，
//并且原函数开头被搬走的指令在 trampoline 上还能正常跑完。
//返回 0 = 通过；否则返回失败步骤码，由调用方写进注册表，插件那边直接显示成中文。
static int ValidateHook()
{
    try
    {
        xaml_shapes::Rectangle probe;
        UIElement ui_element{ probe };
        void* abi = winrt::get_abi(ui_element);
        void** vtable = *reinterpret_cast<void***>(&abi);
        Arrange_func slot = reinterpret_cast<Arrange_func>(vtable[kIUIElementArrangeSlot]);

        const long before = g_arrange_call_count.load();
        HRESULT hr{};
        //这一脚踩下去要么进我们的函数（计数 +1），要么直接崩；两种都算“没确认成功”，但原因要分开报
        if (!SafeCall(slot, abi, Rect{ 0.0f, 0.0f, 40.0f, 40.0f }, &hr))
        {
            PublishSelfTest(L"crash", g_arrange_call_count.load() - before, hr);
            return static_cast<int>(kStepSelfCallCrashed);
        }
        const long delta = g_arrange_call_count.load() - before;
        if (delta == 0)
        {
            //正常返回了却一次都没进我们的函数 = 补丁不在这条调用路径上
            PublishSelfTest(L"nohook", delta, hr);
            return static_cast<int>(kStepSelfCallNotHooked);
        }

        //再直接走一遍 trampoline，确保搬走的指令是好的
        if (!SafeCall(g_arrange_original, abi, Rect{ 0.0f, 0.0f, 42.0f, 42.0f }, &hr))
        {
            PublishSelfTest(L"tramp", g_arrange_call_count.load() - before, hr);
            return static_cast<int>(kStepTrampolineFailed);
        }
        PublishSelfTest(L"ok", g_arrange_call_count.load() - before, hr);
        return 0;
    }
    catch (...)
    {
        //建探针对象/取虚表就抛了，这个线程里的 XAML 运行时状态有问题
        PublishSelfTest(L"cpp", 0, static_cast<HRESULT>(E_UNEXPECTED));
        return static_cast<int>(kStepSelfCallThrew);
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
//S: = 虚表槽里那个地址的开头字节；T: = 真正下补丁的地址（穿过了开头的 jmp 时两者才不同）。
//装成功时 T: 会是 FF2500000000+地址，正好证明补丁还在；装失败时它就是卡住的那段原始指令。
static void PublishPrologue(void* slot_address, const InlineDetour& detour)
{
    wchar_t slot_hex[64]{}, target_hex[64]{};
    BytesToHex(slot_address, slot_hex);
    wchar_t text[160]{};
    size_t pos = 0;
    auto append = [&](const wchar_t* s) { while (*s != L'\0') text[pos++] = *s++; };
    append(L"S:");
    append(slot_hex);
    if (detour.target != nullptr && detour.target != slot_address)
    {
        BytesToHex(detour.target, target_hex);
        append(L" T:");
        append(target_hex);
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
    const int validate_result = ValidateHook();
    if (validate_result != 0)
    {
        //自检没过必须把“已挂钩”这个状态收回：否则下次再装会以为已经装好直接返回，
        //而且 explorer 里还留着指向我们代码的跳转。
        RestoreDetourBytes();
        g_hook_installed = false;
        SplitRegWriteDwordIfChanged(kRegStep, static_cast<DWORD>(validate_result));
        return false;
    }
    SplitRegWriteDwordIfChanged(kRegStep, kStepHookInstalled);
    return true;
}

void RemoveTaskbarSplit()
{
    g_unloading = true;
    RestoreDetourBytes();
    g_hook_installed = false;
    g_seen_taskbar_button = false;
    g_block_width_dip = 0.0;
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
