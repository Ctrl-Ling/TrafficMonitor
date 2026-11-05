// ==WindhawkMod==
// @id              trafficmonitor-system-buttons-left
// @name            TrafficMonitor: system buttons on the left
// @description     Pins the Start, Search and Task View buttons to the left edge of the Windows 11 taskbar while the taskbar icon alignment stays "Center", so the running-app icons remain centered. Companion of TrafficMonitor's "win按钮居左，软件图标居中" option.
// @version         1.0.0
// @author          Ctrl-Ling (TrafficMonitor)
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# TrafficMonitor: system buttons on the left

Moves the **Start**, **Search** and **Task View** buttons to the left edge of
the Windows 11 taskbar and takes them out of the centered group, while the
taskbar icon alignment stays on **Center** so the running-app icons stay
centered:

`[Start][Search][Task View][Widgets]  [TM]     [app icons]     [tray]`

This is the companion of TrafficMonitor's **“win按钮居左，软件图标居中”**
option (设置 → 任务栏窗口设置 → 任务栏窗口). The module only does anything
while that option is enabled:

- TrafficMonitor writes `HKCU\Software\TrafficMonitor\TaskbarSplitEnabled`
  (1 = option enabled) and keeps the Windows alignment on “Center”.
- This module reads that value and publishes back
  `TaskbarSplitActive`, `TaskbarSplitPid` and `TaskbarSplitBlock` (the width of
  the pinned block, in 1/100 DIP), so TrafficMonitor can put its own taskbar
  window right after the pinned buttons.

With the option disabled, or with this module not loaded, TrafficMonitor falls
back to the Windows alignment “Left”, which moves everything left.

Known limitations:

- Windows 11 only (the taskbar is only a XAML surface there).
- The primary taskbar only; secondary taskbars keep the default layout.
- The Start menu and the Search flyout still open where Windows puts them
  (only the buttons themselves are moved).
*/
// ==/WindhawkModReadme==

#include <windhawk_utils.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/base.h>
#define WH_WINRT_WINUI2
#include <winrt/Microsoft.UI.Xaml.Controls.h>

using namespace winrt::Windows::UI::Xaml;

// Registry contract with TrafficMonitor (see WindowsSettingHelper.cpp):
//   TaskbarSplitEnabled  DWORD written by TrafficMonitor, 1 = option enabled
//   TaskbarSplitActive    DWORD written here, 1 while the buttons are pinned
//   TaskbarSplitPid       DWORD written here, explorer.exe pid
//   TaskbarSplitBlock     DWORD written here, pinned block width in 1/100 DIP
static constexpr PCWSTR kRegSubKey = L"Software\\TrafficMonitor";
static constexpr PCWSTR kRegEnabled = L"TaskbarSplitEnabled";
static constexpr PCWSTR kRegActive = L"TaskbarSplitActive";
static constexpr PCWSTR kRegPid = L"TaskbarSplitPid";
static constexpr PCWSTR kRegBlock = L"TaskbarSplitBlock";

std::atomic<bool> g_unloading;
std::atomic<bool> g_taskbarViewDllLoaded;
HKEY g_regKey = nullptr;

thread_local bool g_inArrangeOverride;
thread_local double g_blockWidthDip;

enum class SystemButton {
    None = -1,
    Start = 0,
    Search = 1,
    TaskView = 2,
    Widgets = 3,
};

bool IsSplitEnabled() {
    if (!g_regKey) {
        return false;
    }

    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegQueryValueExW(g_regKey, kRegEnabled, nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value),
                         &size) != ERROR_SUCCESS) {
        return false;
    }

    return type == REG_DWORD && value != 0;
}

void PublishState(bool active, double blockWidthDip) {
    if (!g_regKey) {
        return;
    }

    // Cached so the registry is only touched when something actually changed.
    static bool lastActive = false;
    static DWORD lastBlock = static_cast<DWORD>(-1);

    DWORD block = static_cast<DWORD>(std::lround(blockWidthDip * 100.0));
    if (active == lastActive && block == lastBlock) {
        return;
    }

    DWORD activeValue = active ? 1u : 0u;
    RegSetValueExW(g_regKey, kRegActive, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&activeValue),
                   sizeof(activeValue));

    if (active) {
        DWORD pid = GetCurrentProcessId();
        RegSetValueExW(g_regKey, kRegPid, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&pid), sizeof(pid));
        RegSetValueExW(g_regKey, kRegBlock, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&block), sizeof(block));
    }

    lastActive = active;
    lastBlock = block;
}

SystemButton IdentifySystemButton(FrameworkElement const& element) {
    auto className = winrt::get_class_name(element);

    if (className == L"Taskbar.ExperienceToggleButton") {
        auto automationId =
            Automation::AutomationProperties::GetAutomationId(element);
        if (automationId == L"StartButton") {
            return SystemButton::Start;
        }
        if (automationId == L"TaskViewButton") {
            return SystemButton::TaskView;
        }
    } else if (className == L"Taskbar.AugmentedEntryPointButton") {
        if (element.Name() == L"AugmentedEntryPointButton") {
            return SystemButton::Widgets;
        }
    } else if (className == L"Taskbar.TaskbarExtensionElement") {
        return SystemButton::Search;
    }

    return SystemButton::None;
}

// The three buttons pinned to the left edge, in left-to-right order. The
// widgets button is handled separately: it stays in the taskbar's own layout
// and only gets a leading margin that puts it after this group.
bool IsPinnedClusterButton(SystemButton button) {
    return button == SystemButton::Start || button == SystemButton::Search ||
           button == SystemButton::TaskView;
}

bool IsVerticalTaskbar(FrameworkElement const& taskbarFrame) {
    return taskbarFrame.ActualHeight() > taskbarFrame.ActualWidth();
}

// The extent a cluster button takes up when it is not collapsed. The element's
// own ActualWidth includes the collapse margin (-extent), so the content child's
// DesiredSize is used instead.
double GetButtonExtent(FrameworkElement const& element, bool vertical) {
    if (Media::VisualTreeHelper::GetChildrenCount(element) > 0) {
        auto child =
            Media::VisualTreeHelper::GetChild(element, 0).try_as<FrameworkElement>();
        if (child) {
            auto size = child.DesiredSize();
            return vertical ? size.Height : size.Width;
        }
    }

    return vertical ? element.ActualHeight() : element.ActualWidth();
}

// Walks the taskbar frame's realized items, skipping the repeater's virtualized
// cache so recycled elements don't get counted twice.
template <typename TFunc>
void ForEachTaskbarFrameItem(FrameworkElement const& taskbarFrame, TFunc&& func) {
    if (auto repeater =
            taskbarFrame.try_as<winrt::Microsoft::UI::Xaml::Controls::ItemsRepeater>()) {
        auto itemsSourceView = repeater.ItemsSourceView();
        int count = itemsSourceView ? itemsSourceView.Count() : 0;
        for (int i = 0; i < count; i++) {
            if (auto item = repeater.TryGetElement(i)) {
                if (auto element = item.try_as<FrameworkElement>()) {
                    func(element);
                }
            }
        }
        return;
    }

    int count = Media::VisualTreeHelper::GetChildrenCount(taskbarFrame);
    for (int i = 0; i < count; i++) {
        if (auto element =
                Media::VisualTreeHelper::GetChild(taskbarFrame, i).try_as<FrameworkElement>()) {
            func(element);
        }
    }
}

// Sum of the extents of the pinned buttons that come before `target` in
// cluster order (Start, then Search, then Task View), independent of the order
// the layout arranges its children in.
double ComputeClusterOffset(FrameworkElement const& taskbarFrame,
                            SystemButton target,
                            bool vertical) {
    int targetRank = static_cast<int>(target);
    double offset = 0;

    ForEachTaskbarFrameItem(taskbarFrame, [&](FrameworkElement const& child) {
        SystemButton button = IdentifySystemButton(child);
        int rank = static_cast<int>(button);
        if (rank >= 0 && rank < targetRank && IsPinnedClusterButton(button)) {
            offset += GetButtonExtent(child, vertical);
        }
    });

    return offset;
}

// Takes a pinned button out of the centered group: a trailing margin of
// -extent makes the layout treat it as taking no space, so the app icons stay
// centered on the whole taskbar instead of being pushed right.
void UpdateClusterCollapse(FrameworkElement const& element, bool enabled) {
    auto taskbarFrame =
        Media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
    if (!taskbarFrame) {
        return;
    }

    bool vertical = IsVerticalTaskbar(taskbarFrame);
    Thickness margin = element.Margin();
    Thickness newMargin = margin;

    // Restore only the collapse applied by this module. Both axes are cleared,
    // since the taskbar orientation can change.
    newMargin.Right = std::max(newMargin.Right, 0.0);
    newMargin.Bottom = std::max(newMargin.Bottom, 0.0);

    if (enabled) {
        double extent = GetButtonExtent(element, vertical);
        if (vertical) {
            newMargin.Bottom = -extent;
        } else {
            newMargin.Right = -extent;
        }
    }

    if (newMargin.Right != margin.Right || newMargin.Bottom != margin.Bottom) {
        element.Margin(newMargin);
    }
}

// The widgets button is pinned by Windows at the leading edge, where the Start
// button goes now. A leading margin moves it right after the pinned cluster.
void UpdateWidgetsPosition(FrameworkElement const& element, bool enabled) {
    auto taskbarFrame =
        Media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
    if (!taskbarFrame) {
        return;
    }

    bool vertical = IsVerticalTaskbar(taskbarFrame);
    double leading = enabled
                         ? ComputeClusterOffset(taskbarFrame, SystemButton::Widgets,
                                                vertical)
                         : 0.0;

    Thickness margin = element.Margin();
    Thickness newMargin = margin;
    // Clear the margin applied for the other taskbar orientation.
    if (vertical) {
        newMargin.Left = 0;
        newMargin.Top = leading;
    } else {
        newMargin.Top = 0;
        newMargin.Left = leading;
    }

    if (newMargin.Left != margin.Left || newMargin.Top != margin.Top) {
        element.Margin(newMargin);
    }
}

// Margin changes are deferred off the current layout pass: changing a margin
// while arranging would re-enter layout.
void PostToTaskbarThread(FrameworkElement const& element,
                         void (*func)(FrameworkElement const&, bool)) {
    try {
        bool enabled = IsSplitEnabled();
        element.Dispatcher().TryRunAsync(
            winrt::Windows::UI::Core::CoreDispatcherPriority::High,
            [element, func, enabled]() { func(element, enabled); });
    } catch (...) {
    }
}

bool ClusterMarginNeedsUpdate(FrameworkElement const& element, bool enabled) {
    auto taskbarFrame =
        Media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
    if (!taskbarFrame) {
        return false;
    }

    bool vertical = IsVerticalTaskbar(taskbarFrame);
    Thickness margin = element.Margin();
    double trailing = vertical ? margin.Bottom : margin.Right;
    double other = vertical ? margin.Right : margin.Bottom;

    if (other < -0.5) {
        return true;
    }

    if (enabled) {
        double extent = GetButtonExtent(element, vertical);
        return trailing > -extent + 0.5;
    }

    return trailing < -0.5;
}

bool WidgetsPositionNeedsUpdate(FrameworkElement const& element, bool enabled) {
    auto taskbarFrame =
        Media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
    if (!taskbarFrame) {
        return false;
    }

    bool vertical = IsVerticalTaskbar(taskbarFrame);
    double leading = enabled
                         ? ComputeClusterOffset(taskbarFrame, SystemButton::Widgets,
                                                vertical)
                         : 0.0;
    Thickness margin = element.Margin();
    double current = vertical ? margin.Top : margin.Left;

    return std::abs(current - leading) > 0.5;
}

using IUIElement_Arrange_t =
    HRESULT(WINAPI*)(void* pThis, winrt::Windows::Foundation::Rect rect);
IUIElement_Arrange_t IUIElement_Arrange_Original;

HRESULT WINAPI IUIElement_Arrange_Hook(void* pThis,
                                       winrt::Windows::Foundation::Rect rect) {
    auto original = [=] { return IUIElement_Arrange_Original(pThis, rect); };

    if (!g_inArrangeOverride || g_unloading) {
        return original();
    }

    // Raw ABI boundary (a process-wide vtable slot): an exception escaping here
    // would fail-fast the whole explorer process.
    try {
        bool enabled = IsSplitEnabled();

        FrameworkElement element = nullptr;
        static_cast<IUnknown*>(pThis)->QueryInterface(
            winrt::guid_of<FrameworkElement>(), winrt::put_abi(element));
        if (!element) {
            return original();
        }

        SystemButton button = IdentifySystemButton(element);

        if (button == SystemButton::Widgets) {
            if (WidgetsPositionNeedsUpdate(element, enabled)) {
                PostToTaskbarThread(element, UpdateWidgetsPosition);
            }
            if (enabled) {
                auto taskbarFrame =
                    Media::VisualTreeHelper::GetParent(element)
                        .try_as<FrameworkElement>();
                if (taskbarFrame) {
                    bool vertical = IsVerticalTaskbar(taskbarFrame);
                    double end =
                        ComputeClusterOffset(taskbarFrame, SystemButton::Widgets,
                                             vertical) +
                        GetButtonExtent(element, vertical);
                    if (end > g_blockWidthDip) {
                        g_blockWidthDip = end;
                    }
                }
            }
            return original();
        }

        if (!IsPinnedClusterButton(button)) {
            return original();  // app icons and everything else stay untouched
        }

        if (ClusterMarginNeedsUpdate(element, enabled)) {
            PostToTaskbarThread(element, UpdateClusterCollapse);
        }

        if (!enabled) {
            return original();  // margins restored, no repositioning
        }

        auto taskbarFrame =
            Media::VisualTreeHelper::GetParent(element).try_as<FrameworkElement>();
        if (!taskbarFrame) {
            return original();
        }

        bool vertical = IsVerticalTaskbar(taskbarFrame);
        double offset = ComputeClusterOffset(taskbarFrame, button, vertical);

        double end = offset + GetButtonExtent(element, vertical);
        if (end > g_blockWidthDip) {
            g_blockWidthDip = end;
        }

        winrt::Windows::Foundation::Rect newRect = rect;
        if (vertical) {
            newRect.Y = static_cast<float>(offset);
        } else {
            newRect.X = static_cast<float>(offset);
        }

        return IUIElement_Arrange_Original(pThis, newRect);
    } catch (...) {
        return original();
    }
}

using TaskbarCollapsibleLayoutXamlTraits_ArrangeOverride_t =
    HRESULT(WINAPI*)(void* pThis,
                     void* context,
                     winrt::Windows::Foundation::Size size,
                     winrt::Windows::Foundation::Size* resultSize);
TaskbarCollapsibleLayoutXamlTraits_ArrangeOverride_t
    TaskbarCollapsibleLayoutXamlTraits_ArrangeOverride_Original;

HRESULT WINAPI TaskbarCollapsibleLayoutXamlTraits_ArrangeOverride_Hook(
    void* pThis,
    void* context,
    winrt::Windows::Foundation::Size size,
    winrt::Windows::Foundation::Size* resultSize) {
    // The IUIElement vtable is only reachable from a live XAML element, so the
    // Arrange hook is installed once, on the first layout pass.
    [[maybe_unused]] static bool hooked = [] {
        Shapes::Rectangle rectangle;
        IUIElement element = rectangle;

        void** vtable = *reinterpret_cast<void***>(winrt::get_abi(element));
        auto arrange = reinterpret_cast<IUIElement_Arrange_t>(vtable[92]);

        WindhawkUtils::SetFunctionHook(arrange, IUIElement_Arrange_Hook,
                                       &IUIElement_Arrange_Original);
        Wh_ApplyHookOperations();
        return true;
    }();

    bool enabled = IsSplitEnabled();

    g_blockWidthDip = 0;
    g_inArrangeOverride = enabled;
    HRESULT ret = TaskbarCollapsibleLayoutXamlTraits_ArrangeOverride_Original(
        pThis, context, size, resultSize);
    g_inArrangeOverride = false;

    PublishState(enabled, g_blockWidthDip);

    return ret;
}

bool HookTaskbarViewDllSymbols(HMODULE module) {
    // Taskbar.View.dll, Windows 11.
    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskbarCollapsibleLayout,struct winrt::Microsoft::UI::Xaml::Controls::IVirtualizingLayoutOverrides>::ArrangeOverride(void *,struct winrt::Windows::Foundation::Size,struct winrt::Windows::Foundation::Size *))"},
            &TaskbarCollapsibleLayoutXamlTraits_ArrangeOverride_Original,
            TaskbarCollapsibleLayoutXamlTraits_ArrangeOverride_Hook,
        },
    };

    return HookSymbols(module, symbolHooks, ARRAYSIZE(symbolHooks));
}

HMODULE GetTaskbarViewModuleHandle() {
    HMODULE module = GetModuleHandle(L"Taskbar.View.dll");
    if (!module) {
        module = GetModuleHandle(L"ExplorerExtensions.dll");
    }

    return module;
}

void HandleLoadedModuleIfTaskbarView(HMODULE module, LPCWSTR lpLibFileName) {
    if (!g_taskbarViewDllLoaded && GetTaskbarViewModuleHandle() == module &&
        !g_taskbarViewDllLoaded.exchange(true)) {
        Wh_Log(L"Loaded %s", lpLibFileName);

        if (HookTaskbarViewDllSymbols(module)) {
            Wh_ApplyHookOperations();
        }
    }
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original;
HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName,
                                   HANDLE hFile,
                                   DWORD dwFlags) {
    HMODULE module = LoadLibraryExW_Original(lpLibFileName, hFile, dwFlags);
    if (module) {
        HandleLoadedModuleIfTaskbarView(module, lpLibFileName);
    }

    return module;
}

BOOL Wh_ModInit() {
    Wh_Log(L">");

    // Kept open for the lifetime of the mod: TrafficMonitor's option state is
    // read from here on every layout pass, without opening the key each time.
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, nullptr, 0,
                        KEY_READ | KEY_SET_VALUE, nullptr, &g_regKey,
                        nullptr) != ERROR_SUCCESS) {
        g_regKey = nullptr;
    }

    if (HMODULE taskbarViewModule = GetTaskbarViewModuleHandle()) {
        g_taskbarViewDllLoaded = true;
        if (!HookTaskbarViewDllSymbols(taskbarViewModule)) {
            return FALSE;
        }
    } else {
        Wh_Log(L"Taskbar view module not loaded yet");

        HMODULE kernelBaseModule = GetModuleHandle(L"kernelbase.dll");
        auto pKernelBaseLoadLibraryExW = reinterpret_cast<decltype(&LoadLibraryExW)>(
            GetProcAddress(kernelBaseModule, "LoadLibraryExW"));
        WindhawkUtils::SetFunctionHook(pKernelBaseLoadLibraryExW,
                                       LoadLibraryExW_Hook,
                                       &LoadLibraryExW_Original);
    }

    // Announce the state right away so TrafficMonitor knows whether to keep
    // the taskbar alignment on "Center". The block width is filled in by the
    // first layout pass.
    PublishState(IsSplitEnabled(), 0);

    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L">");

    g_unloading = true;

    PublishState(false, 0);

    if (g_regKey) {
        RegCloseKey(g_regKey);
        g_regKey = nullptr;
    }
}
