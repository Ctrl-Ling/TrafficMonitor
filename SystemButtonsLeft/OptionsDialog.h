#pragma once
#include <windows.h>
#include "PluginInterface.h"

// “win按钮居左，软件图标居中”的选项对话框（纯 Win32，资源在插件 DLL 里）
class CTaskbarOptionsDialog
{
public:
    //hParent：调用者（TrafficMonitor）的窗口句柄；hInstance：插件模块实例
    static ITMPlugin::OptionReturn Show(void* hParent, HINSTANCE hinstance);
};
