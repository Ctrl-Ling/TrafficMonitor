#include "framework.h"
#include "SystemButtonsLeft.h"
#include "TaskbarSplitController.h"
#include "OptionsDialog.h"

extern HMODULE g_plugin_module;

CSystemButtonsLeftPlugin::CSystemButtonsLeftPlugin()
{
}

CSystemButtonsLeftPlugin& CSystemButtonsLeftPlugin::Instance()
{
    static CSystemButtonsLeftPlugin instance;
    return instance;
}

//本插件不提供任何显示项目
IPluginItem* CSystemButtonsLeftPlugin::GetItem(int index)
{
    return nullptr;
}

void CSystemButtonsLeftPlugin::DataRequired()
{
}

const wchar_t* CSystemButtonsLeftPlugin::GetInfo(PluginInfoIndex index)
{
    static const wchar_t* display_name = L"win按钮居左，软件图标居中";
    static const wchar_t* description = L"把开始、搜索、多任务视图等系统按钮固定在任务栏左侧，应用图标保持居中（仅支持 Windows 11 64 位）。";
    static const wchar_t* author = L"Ctrl-Ling";
    static const wchar_t* copyright = L"MIT";
    static const wchar_t* version = L"1.0";
    static const wchar_t* url = L"https://github.com/Ctrl-Ling/TrafficMonitor";

    switch (index)
    {
    case TMI_NAME:
        return display_name;
    case TMI_DESCRIPTION:
        return description;
    case TMI_AUTHOR:
        return author;
    case TMI_COPYRIGHT:
        return copyright;
    case TMI_VERSION:
        return version;
    case TMI_URL:
        return url;
    }
    return L"";
}

ITMPlugin::OptionReturn CSystemButtonsLeftPlugin::ShowOptionsDialog(void* hParent)
{
    return CTaskbarOptionsDialog::Show(hParent, g_plugin_module);
}

void CSystemButtonsLeftPlugin::OnInitialize(ITrafficMonitor* pApp)
{
    m_app = pApp;
    CTaskbarSplitController::Instance().SetApp(pApp);
}

void CSystemButtonsLeftPlugin::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data)
{
    if (index == EI_CONFIG_DIR)
    {
        m_config_dir = data;
        //配置文件：<配置目录>\plugins\SystemButtonsLeft.ini
        std::wstring ini_file = m_config_dir + L"SystemButtonsLeft.ini";
        CTaskbarSplitController::Instance().SetConfigFile(ini_file);
        if (!m_config_loaded)
        {
            m_config_loaded = true;
            //把上次保存的开关状态应用到系统上
            CTaskbarSplitController::Instance().LoadConfigAndApply();
        }
    }
}

__declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    return &CSystemButtonsLeftPlugin::Instance();
}
