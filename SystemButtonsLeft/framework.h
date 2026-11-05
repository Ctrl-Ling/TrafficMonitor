#pragma once

// 插件是纯 Win32 DLL，不使用 MFC（避免插件里再拖一个 MFC 运行时）
#define WIN32_LEAN_AND_MEAN
#define NOGDICAPMASKS
#define NOMENUS
#define NOICONS
#define NOKEYSTATES
#define NOSYSCOMMANDS
#define NORASTEROPS
#define OEMRESOURCE
#define NOMCX
#define NOSERVICE

#include <windows.h>
#include <windowsx.h>
#include <vector>
#include <string>
