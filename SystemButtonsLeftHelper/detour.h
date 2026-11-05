#pragma once
#include <windows.h>

// 一个极小的 x64 内联跳转（detour）实现，只用在本机可控的场景：
// 目标是 explorer 里 XAML 框架的虚表函数，我们要在它进门的地方插一个绝对跳转。
//
// 安全策略（宁可不做，也不能把 explorer 弄崩）：
//   1）目标函数开头的指令必须能被本文件里的小解码器完整识别；
//   2）被覆盖的那几条指令里如果含有相对地址（call/jmp/rip相对寻址），直接放弃；
//   3）安装后由调用方自己跑一次“验证调用”，失败就还原；
//   4）还原只是一次指针/字节写回，模块卸载前（DllMain）一定会执行。

struct InlineDetour
{
    void* target{};                       //被改写的函数地址
    unsigned char saved_bytes[16]{};      //原来的字节
    size_t overwritten_length{};          //被覆盖了多少字节（>=14）
    void* trampoline{};                   //可执行内存：搬过去的原指令 + 跳回目标
    size_t trampoline_length{};
    bool installed{ false };
};

//把 target 开头若干指令搬到 trampoline 上，并在 target 处写入 jmp [rip+0]; addr
bool InstallInlineDetour(InlineDetour& detour, void* target, void* hook);

//还原 target 的原始字节（trampoline 故意不释放：可能还有线程停在里面）
void RemoveInlineDetour(InlineDetour& detour);

//已经装上了吗
inline bool DetourInstalled(const InlineDetour& detour) { return detour.installed; }
