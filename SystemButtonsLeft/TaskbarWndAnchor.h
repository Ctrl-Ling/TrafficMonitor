// Author: Ctrl-Ling
// Last Modified By: Ctrl-Ling
// Last Modified At: 2026-11-09 17:39:27 +08:00
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

class ITrafficMonitor;

//// 让 TrafficMonitor 的任务栏窗口紧跟在被钉到左侧的系统按钮块后面——不改动主程序一行代码。
//
// 物理画面：TM 的任务栏窗口不是飘在桌面上，它是被 SetParent 挂进 Shell_TrayWnd 的一个子窗口。
// 主程序每摆一次位置，最后都要走一次 MoveWindow；而任何一次移动在系统层面都必然先经过
// 这个窗口的 WM_WINDOWPOSCHANGING（就像进门总得摸一次门把手）。插件和它同一个进程，
// 于是可以给这个窗口套一层窗口过程（subclass），在坐标落地前一刻把 X 改成“按钮块右边界 + 间距”。
// 主程序里原本那段“拆分生效时摆在按钮块后面”的分支因此不再需要存在。
namespace CTaskbarWndAnchor
{
    //传入主程序接口指针（插件 OnInitialize 时给一次）。之后取任务栏窗口句柄、取任务栏 DPI 都靠它。
    void SetApp(ITrafficMonitor* app);

    //告诉锚定模块当前拆分状态：助手是否钉住了左块、左块右边界（1/100 DIP）。
    //只改内存里的数，不碰窗口、不碰注册表，任何线程都能调。
    void UpdateState(bool split_active, int block_right_edge_100);

    //按当前状态把子类过程挂上去/摘掉/挪到新窗口上，并把诊断信息写进注册表。
    //只在 TM 的界面线程调用（本体的每帧回调 OnMonitorInfo 就是这个点）：
    //挂摘窗口过程必须在窗口所属线程之外也别碰加载器锁，界面线程最稳。
    void Sync();
}
