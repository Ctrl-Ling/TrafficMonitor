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
        ::swprintf_s(buf, L"助手：未找到 %s（要和插件放在同一个 plugins 目录）", kHelperDllName);
    else if (st.split_active)
        ::swprintf_s(buf, L"助手：已在 explorer.exe 中挂钩，左侧按钮块宽 %d DIP", st.block_width);
    else if (st.helper_injected)
        ::swprintf_s(buf, L"助手：已注入，但没能改写布局（保持系统默认）");
    else
        ::swprintf_s(buf, L"助手：已就位，尚未注入");
    SetStatusLine(hDlg, IDC_STATUS_LINE3, buf);

    ::swprintf_s(buf, L"Windows 任务栏图标对齐：%s（拆分生效时保持“居中”）",
        st.taskbar_center_align ? L"居中" : L"靠左");
    SetStatusLine(hDlg, IDC_STATUS_LINE4, buf);

    const wchar_t* result = CTaskbarSplitController::Instance().LastMessage().c_str();
    SetStatusLine(hDlg, IDC_STATUS_LINE5, result);
    SetStatusLine(hDlg, IDC_STATUS_LINE6, L"");
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
