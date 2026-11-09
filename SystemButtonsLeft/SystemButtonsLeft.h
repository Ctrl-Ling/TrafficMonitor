// Author: Ctrl-Ling
// Last Modified By: Ctrl-Ling
// Last Modified At: 2026-11-09 17:39:27 +08:00
#pragma once
#include "framework.h"
#include "PluginInterface.h"
#include <string>

// “win按钮居左，软件图标居中”插件。
// 这个插件本身不在任务栏上显示任何监控项目（GetItem 永远返回空），
// 它只做一件事：把助手 DLL 注入 explorer.exe，让 Windows 11 的开始/搜索/多任务视图等按钮固定在任务栏左侧，
// 同时让应用图标继续保持居中；TrafficMonitor 的任务栏窗口会紧跟在被钉住的按钮块后面。
class CSystemButtonsLeftPlugin : public ITMPlugin
{
private:
    CSystemButtonsLeftPlugin();

public:
    static CSystemButtonsLeftPlugin& Instance();

    // 通过 ITMPlugin 继承
    virtual IPluginItem* GetItem(int index) override;
    virtual void DataRequired() override;
    virtual const wchar_t* GetInfo(PluginInfoIndex index) override;
    virtual OptionReturn ShowOptionsDialog(void* hParent) override;
    virtual void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override;
    virtual void OnInitialize(ITrafficMonitor* pApp) override;
    //本体每次采集完数据都会在本体的界面线程上调用这里（不是定时器，是本体现成的节奏），
    //借这个点把“TM 任务栏窗口紧跟左侧按钮块”的子类化挂好/挪到新窗口上
    virtual void OnMonitorInfo(const MonitorInfo& monitor_info) override;

private:
    ITrafficMonitor* m_app{};
    std::wstring m_config_dir;
    bool m_config_loaded{ false };
};

#ifdef __cplusplus
extern "C" {
#endif
    __declspec(dllexport) ITMPlugin* TMPluginGetInstance();

#ifdef __cplusplus
}
#endif
