// Author: Ctrl-Ling
// Last Modified By: Ctrl-Ling
// Last Modified At: 2026-11-10 16:53:30 +08:00
#include "framework.h"
#include <cstdio>
#include <cwchar>
#include "resource.h"
#include "OptionsDialog.h"
#include "TaskbarSplitController.h"
#include "SplitContract.h"

static void SetStatusLine(HWND hDlg, int id, const wchar_t* text)
{
    ::SetDlgItemTextW(hDlg, id, text);
}

//助手写在注册表里的进度码 -> 一句话诊断。
//挂钩是跨进程的（插件在 TrafficMonitor，助手在 explorer.exe），只看“没生效”没法定位，
//所以助手每过一步就写一个码，这里翻译成人话。
static const wchar_t* HelperStepText(int step)
{
    switch (step)
    {
    case kStepNone:               return L"当前 explorer 里没有助手在跑（没注入，或 explorer 刚重启过）";
    case kStepHookRan:            return L"助手已进 explorer 的任务栏线程，还没读到开关";
    case kStepRequestSeen:        return L"助手读到开关了，正在装挂钩";
    case kStepProbeObjectFailed:  return L"建 XAML 探针对象失败（这个线程拿不到 XAML 运行时）";
    case kStepSlotNotXaml:        return L"槽位地址不在 XAML 模块里（这个系统版本上槽位 92 不对）";
    case kStepDetourFailed:       return L"内联跳转装不上（原因见细分码 231~238）";
    case kStepDetourBadArgs:      return L"内联跳转装不上：挂钩地址是空的";
    case kStepDetourUnreadable:   return L"内联跳转装不上：函数所在内存读不到（开头字节已写进注册表 TaskbarSplitPrologue）";
    case kStepDetourBadInstruction: return L"内联跳转装不上：函数开头有看不懂的指令，不敢搬（开头字节已写进注册表 TaskbarSplitPrologue）";
    case kStepDetourTooLong:      return L"内联跳转装不上：函数开头指令太多太长，搬不动";
    case kStepDetourFarTarget:    return L"内联跳转装不上：搬走后相对地址超出可编码范围（跳板离函数太远）";
    case kStepDetourNoMemory:     return L"内联跳转装不上：函数附近找不到能用的可执行内存";
    case kStepDetourProtect:      return L"内联跳转装不上：改不动函数代码页的写权限";
    case kStepDetourNotWritten:   return L"内联跳转装不上：补丁字节写不进去（写完读回来不是我们写的）";
    case kStepSelfCallMissed:     return L"自检失败：调用该槽位没进我们的函数（挂钩点不对）";
    case kStepSelfCallCrashed:    return L"（旧版）自检：调用该槽位当场崩了，还没进我们的函数（异常码见注册表 TaskbarSplitSelfTest 的 se=）";
    case kStepSelfCallNotHooked:  return L"自检失败：踩补丁地址能正常返回，但一次都没进我们的函数 —— 那几个字节没被执行（现场见 TaskbarSplitTarget / SelfTest）";
    case kStepSelfCallThrew:      return L"自检失败：建探针对象或取虚表时抛异常（这个线程的 XAML 运行时状态不对）";
    case kStepSelfCallHookFaulted: return L"自检失败：原函数跑得好好的，一踩我们改过的地址就崩 —— 搬走的指令有问题，已自动拆掉补丁（四个地址见 TaskbarSplitSelfTest）";
    case kStepSelfCallProbeUnsafe: return L"补丁已写入并核对过，但自检用的临时控件本身排不了版（走原函数也崩），改没改到位要等任务栏真的排一次版才知道；这期间不动 Windows 的对齐设置";
    case kStepTrampolineFailed:   return L"自检失败：搬走的指令在 trampoline 上跑不动";
    case kStepHookInstalled:      return L"挂钩成功，等任务栏排一次版就真正钉住";
    case kStepPinning:            return L"正在钉住：系统按钮已经过挂钩";
    default:                      return L"未知进度码";
    }
}

//把控制器的当前状态填到对话框的“当前状态”分组里
static void RefreshStatusLines(HWND hDlg)
{
    const SplitStatus& st = CTaskbarSplitController::Instance().Status();
    wchar_t buf[256];

    if (st.windows11)
        ::swprintf_s(buf, L"系统：Windows 11，支持拆分");
    else
        ::swprintf_s(buf, L"系统：不是 Windows 11，只能用系统自带的“靠左”");
    SetStatusLine(hDlg, IDC_STATUS_LINE1, buf);

    ::swprintf_s(buf, L"TrafficMonitor：%d 位", static_cast<int>(sizeof(void*) * 8));
    SetStatusLine(hDlg, IDC_STATUS_LINE2, buf);

    if (!st.helper_exists)
        ::swprintf_s(buf, L"助手：未找到 %s（要和 TrafficMonitor.exe 放同一个目录）", kHelperDllName);
    else if (st.split_active)
    {
        wchar_t build[64]{};
        SplitRegReadString(kRegBuild, build, sizeof(build) / sizeof(build[0]));
        if (build[0] != L'\0')
            ::swprintf_s(buf, L"助手：已在 explorer.exe 中挂钩，左侧按钮块宽 %d DIP｜%s", st.block_width, build);
        else
            ::swprintf_s(buf, L"助手：已在 explorer.exe 中挂钩，左侧按钮块宽 %d DIP", st.block_width);
    }
    else if (st.helper_injected)
        ::swprintf_s(buf, L"助手：已注入，但没能改写布局（未改动 Windows 的对齐设置）");
    else
        ::swprintf_s(buf, L"助手：已就位，尚未注入");
    SetStatusLine(hDlg, IDC_STATUS_LINE3, buf);

    ::swprintf_s(buf, L"Windows 任务栏图标对齐：%s（拆分生效时保持“居中”）",
        st.taskbar_center_align ? L"居中" : L"靠左");
    SetStatusLine(hDlg, IDC_STATUS_LINE4, buf);

    const wchar_t* result = CTaskbarSplitController::Instance().LastMessage().c_str();
    SetStatusLine(hDlg, IDC_STATUS_LINE5, result);

    //诊断分三行，一行一件事（每行只有约 55 个字符的宽度，拼一起就被截断了，实测就是这样丢了后半句）：
    //  第 6 行：explorer 里的助手卡在哪一步；
    //  第 7 行：插件这边有没有真的把 TM 的任务栏窗口搬到按钮块后面；
    //  第 8 行：助手看到的四个按钮各自的右边界（块宽取其中最大的一个）。
    ::swprintf_s(buf, L"诊断（%d）：%s", st.helper_step, HelperStepText(st.helper_step));
    SetStatusLine(hDlg, IDC_STATUS_LINE6, buf);

    wchar_t anchor[256]{};
    if (SplitRegReadString(kRegWnd, anchor, sizeof(anchor) / sizeof(anchor[0])) && anchor[0] != L'\0')
        ::swprintf_s(buf, L"窗口跟随：%s", anchor);
    else
        ::swprintf_s(buf, L"窗口跟随：还没挂上（任务栏窗口打开后本体下一次刷新就挂）");
    SetStatusLine(hDlg, IDC_STATUS_LINE7, buf);

    wchar_t edges[128]{};
    if (SplitRegReadString(kRegEdges, edges, sizeof(edges) / sizeof(edges[0])) && edges[0] != L'\0')
        ::swprintf_s(buf, L"按钮右边界：%s", edges);
    else
        ::swprintf_s(buf, L"按钮右边界：助手还没报上来");
    SetStatusLine(hDlg, IDC_STATUS_LINE8, buf);
}

static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT message, WPARAM w_param, LPARAM l_param)
{
    switch (message)
    {
    case WM_INITDIALOG:
    {
        CTaskbarSplitController::Instance().RefreshStatus();
        ::CheckDlgButton(hDlg, IDC_SPLIT_ENABLE_CHECK,
            CTaskbarSplitController::Instance().IsEnabled() ? BST_CHECKED : BST_UNCHECKED);
        RefreshStatusLines(hDlg);
        return TRUE;
    }

    case WM_COMMAND:
    {
        switch (LOWORD(w_param))
        {
        case IDOK:
        {
            bool old_enabled = CTaskbarSplitController::Instance().IsEnabled();
            bool new_enabled = (::IsDlgButtonChecked(hDlg, IDC_SPLIT_ENABLE_CHECK) != BST_UNCHECKED);
            //勾选状态变了，或者勾上了但上一次没成功，都重新应用一次
            if (new_enabled != old_enabled || (new_enabled && !CTaskbarSplitController::Instance().Status().split_active))
            {
                std::wstring msg;
                bool ok = CTaskbarSplitController::Instance().Apply(new_enabled, msg);
                RefreshStatusLines(hDlg);
                if (!ok && !msg.empty())
                    ::MessageBoxW(hDlg, msg.c_str(), L"win按钮居左", MB_OK | MB_ICONWARNING);
                ::EndDialog(hDlg, IDOK);
                return TRUE;
            }
            ::EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }

        case IDCANCEL:
            ::EndDialog(hDlg, IDCANCEL);
            return TRUE;

        case IDC_SPLIT_ENABLE_CHECK:
        {
            //勾选/取消时立刻把状态文字刷新一次，让用户看到当前是否满足条件
            RefreshStatusLines(hDlg);
            return TRUE;
        }
        }
        break;
    }

    case WM_CLOSE:
        ::EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

ITMPlugin::OptionReturn CTaskbarOptionsDialog::Show(void* hParent, HINSTANCE hinstance)
{
    bool enabled_before = CTaskbarSplitController::Instance().IsEnabled();
    INT_PTR ret = ::DialogBoxParamW(hinstance, MAKEINTRESOURCEW(IDD_OPTIONS_DIALOG),
        static_cast<HWND>(hParent), DlgProc, 0);
    CTaskbarSplitController::Instance().RefreshStatus();
    bool enabled_after = CTaskbarSplitController::Instance().IsEnabled();
    //只要点过“确定”并且状态发生了变化（或者本来就有变化）就告诉主程序设置改了
    if (ret == IDOK && enabled_before != enabled_after)
        return ITMPlugin::OR_OPTION_CHANGED;
    return ITMPlugin::OR_OPTION_UNCHANGED;
}
