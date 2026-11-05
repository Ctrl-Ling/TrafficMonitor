#pragma once

//助手 DLL：注入到 explorer.exe 里，把任务栏的系统按钮钉在左侧。
//这里用 C++/WinRT 的 SDK 头（winrt/*.h），不需要 /ZW，只需要 C++17。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

//Windows 头和 C++/WinRT 撞名的宏（winrt 的头里会报 C4002）
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

//C++/WinRT 的运行时入口（RoGetActivationFactory 等）由这几个库提供。
//写在这里而不是各个 .cpp 里，保证只要包含 framework.h 就自动带上。
#pragma comment(lib, "windowsapp")
#pragma comment(lib, "runtimeobject")
#pragma comment(lib, "ole32")
#pragma comment(lib, "advapi32")

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
