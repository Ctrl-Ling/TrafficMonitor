#include "framework.h"

//插件模块实例句柄（对话框资源在插件 DLL 里，需要它）
HMODULE g_plugin_module{};

BOOL APIENTRY DllMain(HMODULE hmodule, DWORD ul_reason_for_call, LPVOID lp_reserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        g_plugin_module = hmodule;
        ::DisableThreadLibraryCalls(hmodule);
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;

    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
