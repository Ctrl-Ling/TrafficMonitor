#include "framework.h"
#include "detour.h"
#include <cstring>

// x64 内联跳转：在目标函数开头写入 jmp [rip+0] + 8 字节绝对地址，共 14 字节。
// 被覆盖的必须是整数条指令，所以实际覆盖长度是 >=14 的某个指令边界。
// 只有当这些指令里不含任何相对地址（call / jcc / rip相对寻址 / SIB+disp32 / moffs）时才敢搬；
// 认不出来的一律返回失败，由调用方退回系统自带的“靠左”，绝不带着半懂的字节去改 explorer。
static constexpr size_t kPatchSize = 14;

enum : unsigned char
{
    F_NONE = 0,
    F_MODRM = 1 << 0,     //有 ModRM（可能带 SIB / 位移）
    F_IMM8 = 1 << 1,      //1 字节立即数
    F_IMMV = 1 << 2,      //无 0x66 前缀时 4 字节，有则 2 字节
    F_REL = 1 << 3,       //含相对地址：放弃安装
    F_FIXED2 = 1 << 4,    //双字节 opcode 且定长 2（syscall / cpuid 等）
    F_GROUP = 1 << 5      //有没有立即数取决于 ModRM 的 reg 字段（F6 / F7）
};

//单字节 opcode 表，只收录编译器会给函数开头生成的那几种形式
static unsigned char OpcodeFlags1(unsigned char op)
{
    switch (op)
    {
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
    case 0x90: case 0x98: case 0x99: case 0xC3: case 0xC9: case 0xCC:
        return F_NONE;
    case 0xE8: case 0xE9: case 0xEB:
        return F_REL;
    case 0x6A: case 0xA8:
        return F_IMM8;
    case 0x68: case 0xA9:
        return F_IMMV;
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C:
        return F_IMM8;
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D:
        return F_IMMV;
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x38: case 0x39: case 0x3A: case 0x3B:
    case 0x63:
    case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F:
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
    case 0xFE: case 0xFF:
        return F_MODRM;
    case 0x69:
        return F_MODRM | F_IMMV;
    case 0x6B:
        return F_MODRM | F_IMM8;
    case 0x80: case 0x82: case 0x83:
        return F_MODRM | F_IMM8;
    case 0x81:
        return F_MODRM | F_IMMV;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        return F_IMM8;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        return F_IMMV;          //有 REX.W 时是 8 字节立即数，解码里补
    case 0xC6:
        return F_MODRM | F_IMM8;
    case 0xC7:
        return F_MODRM | F_IMMV;
    case 0xF6:
        return F_MODRM | F_GROUP | F_IMM8;
    case 0xF7:
        return F_MODRM | F_GROUP | F_IMMV;
    }
    return 0xFF;                //不认识
}

//双字节 opcode（0F xx）表，同样只收常见的那几种
static unsigned char OpcodeFlags2(unsigned char op)
{
    switch (op)
    {
    case 0x05: case 0xA2:
        return F_FIXED2;        //syscall / cpuid
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        return F_REL;           //jcc rel32
    case 0x1E: case 0x1F:       //endbr64 / 多字节 nop
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
    case 0xAF:                   //imul
    case 0xB0: case 0xB1:        //cmpxchg / xadd
    case 0xB6: case 0xB7:        //movzx
    case 0xBE: case 0xBF:        //movsx
        return F_MODRM;
    case 0xA4: case 0xAC: case 0xBA:    //shld / shrd / bt 的立即数形式
        return F_MODRM | F_IMM8;
    }
    return 0xFF;
}

//一条指令的长度；返回 0 表示看不懂；relative 置真表示含相对地址，不能搬
static size_t DecodeOne(const unsigned char* code, size_t available, bool& relative)
{
    relative = false;
    size_t i = 0;
    bool rex_w = false;
    bool imm16 = false;

    for (;;)
    {
        if (i >= available)
            return 0;
        unsigned char b = code[i];
        if (b == 0x66)
        {
            imm16 = true;
            i++;
            continue;
        }
        if (b == 0xF2 || b == 0xF3 || b == 0xF0 || b == 0x2E || b == 0x36 ||
            b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65)
        {
            i++;
            continue;
        }
        if (b == 0x67)
            return 0;       //地址大小前缀会改变寻址宽度，不处理
        if (b >= 0x40 && b <= 0x4F)
        {
            rex_w = (b & 0x08) != 0;
            i++;
            break;          //REX 必须是最后一个前缀
        }
        break;
    }

    if (i >= available)
        return 0;
    unsigned char op = code[i];
    i++;
    bool two_byte = false;
    if (op == 0x0F)
    {
        two_byte = true;
        if (i >= available)
            return 0;
        op = code[i];
        i++;
    }

    unsigned char flags = two_byte ? OpcodeFlags2(op) : OpcodeFlags1(op);
    if (flags == 0xFF)
        return 0;

    if (flags & F_REL)
    {
        //EB 是 rel8；E8/E9 和 0F 80-8F 都是 rel32（此时 i 已经算上了 opcode 本身）
        size_t disp = (!two_byte && op == 0xEB) ? 1 : 4;
        relative = true;
        return (i + disp <= available) ? i + disp : 0;
    }
    if (flags & F_FIXED2)
        return i;

    unsigned char reg_field = 0;
    if (flags & F_MODRM)
    {
        if (i >= available)
            return 0;
        unsigned char modrm = code[i];
        reg_field = (modrm >> 3) & 7;
        i++;
        unsigned char mod = modrm >> 6;
        unsigned char rm = modrm & 7;
        if (mod != 3)
        {
            if (rm == 4)
            {
                if (i >= available)
                    return 0;
                unsigned char sib = code[i];
                i++;
                unsigned char base = sib & 7;
                if (mod == 0 && base == 5)
                {
                    relative = true;      //SIB + disp32（绝对地址），不敢搬
                    i += 4;
                }
                else if (mod == 1)
                    i += 1;
                else if (mod == 2)
                    i += 4;
            }
            else if (mod == 0 && rm == 5)
            {
                relative = true;          //RIP 相对寻址，搬走就错位
                i += 4;
            }
            else if (mod == 1)
                i += 1;
            else if (mod == 2)
                i += 4;
        }
    }

    bool has_imm = true;
    if (flags & F_GROUP)
        has_imm = (reg_field == 0 || reg_field == 1);    //只有 test 才有立即数

    if (has_imm)
    {
        if (flags & F_IMM8)
            i += 1;
        else if (flags & F_IMMV)
        {
            size_t sz = imm16 ? 2 : 4;
            if (!two_byte && rex_w && op >= 0xB8 && op <= 0xBF)
                sz = 8;       //movabs r64, imm64
            i += sz;
        }
    }

    return (i <= available && i <= 15) ? i : 0;
}

//从头走完整指令直到覆盖够 patch_size；遇到相对地址或不认识的指令就返回 0
static size_t MeasurePrologue(const unsigned char* code, size_t patch_size)
{
    constexpr size_t window = 32;
    size_t offset = 0;
    while (offset < patch_size)
    {
        bool relative = false;
        size_t len = DecodeOne(code + offset, window - offset, relative);
        if (len == 0 || relative)
            return 0;
        offset += len;
    }
    return offset;
}

//在目标 ±2GB 内找一块可执行内存，跳回原函数时才能用 5 字节的 jmp rel32
static void* AllocNearbyTrampoline(void* target, size_t size)
{
    const uintptr_t target_addr = reinterpret_cast<uintptr_t>(target);
    const uintptr_t gran = 0x10000;
    const uintptr_t first = (target_addr / gran) * gran;
    for (int ring = 0; ring <= 24; ring++)
    {
        for (int sign = -1; sign <= 1; sign += 2)
        {
            uintptr_t candidate = first + static_cast<uintptr_t>(ring * gran) * static_cast<uintptr_t>(sign);
            if (candidate < 0x10000)
                continue;
            intptr_t delta = static_cast<intptr_t>(candidate) - static_cast<intptr_t>(target_addr);
            if (delta <= -0x70000000LL || delta >= 0x70000000LL)
                continue;
            void* p = ::VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p != nullptr)
                return p;
        }
    }
    return nullptr;
}

bool InstallInlineDetour(InlineDetour& detour, void* target, void* hook)
{
    detour = InlineDetour{};
    if (target == nullptr || hook == nullptr)
        return false;

    const unsigned char* code = static_cast<const unsigned char*>(target);
    size_t overwritten = MeasurePrologue(code, kPatchSize);
    if (overwritten == 0 || overwritten > sizeof(detour.saved_bytes))
        return false;

    const size_t tramp_size = overwritten + 16;
    void* tramp = AllocNearbyTrampoline(target, tramp_size);
    if (tramp == nullptr)
        return false;

    unsigned char* cursor = static_cast<unsigned char*>(tramp);
    memcpy(cursor, code, overwritten);
    cursor += overwritten;

    //跳回目标函数没有被覆盖的部分
    intptr_t back = reinterpret_cast<intptr_t>(static_cast<unsigned char*>(target) + overwritten) -
        (static_cast<intptr_t>(reinterpret_cast<uintptr_t>(cursor)) + 5);
    if (back <= -0x70000000LL || back >= 0x70000000LL)
    {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    *cursor++ = 0xE9;
    int32_t back32 = static_cast<int32_t>(back);
    memcpy(cursor, &back32, sizeof(back32));
    cursor += sizeof(back32);

    DWORD old_protect{};
    if (!::VirtualProtect(target, overwritten, PAGE_EXECUTE_READWRITE, &old_protect))
    {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }

    memcpy(detour.saved_bytes, code, overwritten);
    detour.overwritten_length = overwritten;

    unsigned char* patch = static_cast<unsigned char*>(target);
    patch[0] = 0xFF;
    patch[1] = 0x25;
    patch[2] = 0;
    patch[3] = 0;
    patch[4] = 0;
    patch[5] = 0;
    uint64_t hook_addr = reinterpret_cast<uint64_t>(hook);
    memcpy(patch + 6, &hook_addr, sizeof(hook_addr));

    ::VirtualProtect(target, overwritten, old_protect, &old_protect);
    ::FlushInstructionCache(::GetCurrentProcess(), target, overwritten);

    detour.target = target;
    detour.trampoline = tramp;
    detour.trampoline_length = static_cast<size_t>(cursor - static_cast<unsigned char*>(tramp));
    detour.installed = true;
    return true;
}

void RemoveInlineDetour(InlineDetour& detour)
{
    if (detour.target != nullptr && detour.overwritten_length > 0)
    {
        DWORD old_protect{};
        if (::VirtualProtect(detour.target, detour.overwritten_length, PAGE_EXECUTE_READWRITE, &old_protect))
        {
            memcpy(detour.target, detour.saved_bytes, detour.overwritten_length);
            ::VirtualProtect(detour.target, detour.overwritten_length, old_protect, &old_protect);
            ::FlushInstructionCache(::GetCurrentProcess(), detour.target, detour.overwritten_length);
        }
    }
    detour.installed = false;
    //trampoline 故意不释放：可能还有线程正停在里面执行。泄漏 4KB 换稳定，进程退出自然回收。
}
