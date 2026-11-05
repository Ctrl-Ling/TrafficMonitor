#pragma once
#include <windows.h>

// 一个极小的 x64 内联跳转（detour）实现，只用在本机可控的场景：
// 目标是 explorer 里 XAML 框架的虚表函数，我们要在它进门的地方插一个绝对跳转。
//
// 安全策略（宁可不做，也不能把 explorer 弄崩）：
//   1）目标函数开头的指令必须能被本文件里的小解码器完整识别；
//   2）被覆盖的指令里如果有相对地址（call/jmp/rip 相对寻址），不再放弃 ——
//      搬到 trampoline 时按 trampoline 的位置把位移重算一遍，等价改写；
//      只有重算后放不下（超出 rel8/rel32 范围）才放弃；
//   3）函数开头如果只是一串 jmp（编译器/CET 的跳转表、加载器打补丁留下的），
//      先顺着 jmp 走到真正的实现，在真实现上下补丁；
//   4）地址大小前缀 0x67、三段式 opcode（0F 38/3A）、moffs 之类一律认不出来就失败；
//   5）安装后由调用方自己跑一次“验证调用”，失败就还原；
//   6）还原只是一次字节写回，模块卸载前（DllMain）一定会执行。

//失败原因。内联跳转这条路上有好几个岔口，跨进程挂钩出错时只能靠这个数说话，
//助手会把它换算成 23x 进度码写进注册表，插件选项框直接翻译成中文显示。
enum DetourFail
{
    kDetourFailNone = 0,
    kDetourFailBadArgs = 1,       //目标或钩子地址是空的
    kDetourFailUnreadable = 2,    //目标内存读不到（不是已提交的代码页）
    kDetourFailBadInstruction = 3,//开头有本解码器不认识的指令
    kDetourFailTooLong = 4,       //开头指令太长，搬不动
    kDetourFailFarTarget = 5,     //重算后的相对地址放不下
    kDetourFailNoMemory = 6,      //找不到能用的可执行内存
    kDetourFailProtect = 7,       //VirtualProtect 改不动写权限
    kDetourFailNotWritten = 8     //写完跳回读，字节没变（代码页写不进去 / 被保护）
};

struct InlineDetour
{
    void* target{};                       //真正被改写的地址（可能已经顺着开头的 jmp 往后挪了）
    void* slot_address{};                 //调用方最初给我们的地址（虚表槽里那个）
    bool  thunk_followed{ false };        //是否穿过了开头的 jmp
    unsigned char saved_bytes[32]{};      //原来的字节
    size_t overwritten_length{};          //被覆盖了多少字节（>=14）
    void* trampoline{};                   //可执行内存：搬过去的原指令（位移已修正）+ 跳回目标
    size_t trampoline_length{};
    bool installed{ false };
};

//安全读取一段内存（只做 memcpy，不碰未提交的页）。返回真正读到的字节数，0=读不了。
size_t ReadTargetBytes(void* address, unsigned char* buffer, size_t count);

//这块地址是不是已提交、可读、可执行的代码
bool IsReadableCodeAddress(void* address);

//把 target 开头若干指令搬到 trampoline 上（相对地址按新位置重算），
//并在 target 处写入 jmp [rip+0]; addr。fail_reason 为空时只回 true/false。
//follow_thunks=false（默认）：补丁就打在 target 这个地址上 —— 虚表槽指哪就打哪，
//  这样“通过槽位调用”一定走进我们的钩子（实测打过一次“顺着 jmp 打到真身”，
//  结果自检时调用槽位根本没进钩子，说明那个 jmp 的目标并不是调用真正去的地方）。
//follow_thunks=true：先穿过开头的 jmp，把补丁打在它指向的实现上（开头搬不动时的退路）。
bool InstallInlineDetour(InlineDetour& detour, void* target, void* hook, DetourFail* fail_reason = nullptr,
                         bool follow_thunks = false);

//还原 target 的原始字节（trampoline 故意不释放：可能还有线程停在里面）
void RemoveInlineDetour(InlineDetour& detour);

//已经装上了吗
inline bool DetourInstalled(const InlineDetour& detour) { return detour.installed; }
