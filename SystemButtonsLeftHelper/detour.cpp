#include "framework.h"
#include "detour.h"
#include <cstring>

// x64 内联跳转：在目标函数开头写入 jmp [rip+0] + 8 字节绝对地址，共 14 字节。
// 被覆盖的必须是整数条指令，所以实际覆盖长度是 >=14 的某个指令边界。
// 搬走的指令里如果带相对地址（E8/E9/EB、0F 8x、rip 相对寻址），搬到 trampoline 后
// “下一条指令的位置”变了，必须把位移重新算一遍；算完放不下就整个放弃。
// 认不出来的一律返回失败，由调用方退回系统自带的“靠左”，绝不带着半懂的字节去改 explorer。
static constexpr size_t kPatchSize = 14;

enum : unsigned char
{
    F_NONE = 0,
    F_MODRM = 1 << 0,     //有 ModRM（可能带 SIB / 位移）
    F_IMM8 = 1 << 1,      //1 字节立即数
    F_IMMV = 1 << 2,      //无 0x66 前缀时 4 字节，有则 2 字节
    F_REL = 1 << 3,       //含相对跳转（E8/E9/EB/0F 8x），搬走时要重算位移
    F_FIXED2 = 1 << 4,    //双字节 opcode 且定长 2（syscall / cpuid 等）
    F_GROUP = 1 << 5,     //有没有立即数取决于 ModRM 的 reg 字段（F6 / F7）
    F_IMM16 = 1 << 6      //固定 2 字节立即数（ret imm16）
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
    case 0x6A: case 0xA8: case 0xCD:
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
    case 0x63: case 0x6C: case 0x6D:
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
    case 0xC2:
        return F_IMM16;         //ret imm16，没有 modrm
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
    case 0xA3: case 0xAB: case 0xB3:       //bt / bts / btr
    case 0xAF:                   //imul
    case 0xB0: case 0xB1:        //cmpxchg / xadd
    case 0xB6: case 0xB7:        //movzx
    case 0xBE: case 0xBF:        //movsx
    case 0x10: case 0x11:        //movups
    case 0x28: case 0x29:        //movaps
    case 0x6E: case 0x7E:        //movq
    case 0xD6: case 0xD7:        //movq / movntq
        return F_MODRM;
    case 0xA4: case 0xAC: case 0xBA:       //shld / shrd / bt 的立即数形式
        return F_MODRM | F_IMM8;
    case 0x70: case 0xC6:        //pshufd / movntps 这类带 1 字节立即数的
        return F_MODRM | F_IMM8;
    }
    return 0xFF;
}

//一条指令的解码结果。disp_size>0 表示这条指令里有一段“相对地址”，
//disp_offset 是它在指令内的偏移，is_branch 区分相对跳转和 rip 相对寻址。
struct InstrInfo
{
    size_t length{};
    size_t disp_offset{};
    size_t disp_size{};     //0=没有；1=rel8；4=rel32
    bool is_branch{};
};

//解码一条指令；返回 false 表示看不懂
static bool DecodeOne(const unsigned char* code, size_t available, InstrInfo& info)
{
    info = InstrInfo{};
    size_t i = 0;
    bool rex_w = false;
    bool imm16 = false;

    for (;;)
    {
        if (i >= available)
            return false;
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
            return false;     //地址大小前缀会改变寻址宽度，不处理
        if (b >= 0x40 && b <= 0x4F)
        {
            rex_w = (b & 0x08) != 0;
            i++;
            break;            //REX 必须是最后一个前缀
        }
        break;
    }

    if (i >= available)
        return false;
    unsigned char op = code[i];
    i++;
    bool two_byte = false;
    if (op == 0x0F)
    {
        two_byte = true;
        if (i >= available)
            return false;
        op = code[i];
        i++;
        if (op == 0x38 || op == 0x3A)
            return false;     //三段式 opcode，本解码器不认识
    }

    unsigned char flags = two_byte ? OpcodeFlags2(op) : OpcodeFlags1(op);
    if (flags == 0xFF)
        return false;

    if (flags & F_REL)
    {
        //EB 是 rel8；E8/E9 和 0F 80-8F 都是 rel32（此时 i 已经算上了 opcode 本身）
        info.disp_size = (!two_byte && op == 0xEB) ? 1 : 4;
        info.disp_offset = i;
        info.is_branch = true;
        if (i + info.disp_size > available)
            return false;
        info.length = i + info.disp_size;
        return true;
    }
    if (flags & F_FIXED2)
    {
        info.length = i;
        return true;
    }

    unsigned char reg_field = 0;
    if (flags & F_MODRM)
    {
        if (i >= available)
            return false;
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
                    return false;
                unsigned char sib = code[i];
                i++;
                unsigned char base = sib & 7;
                unsigned char index = (sib >> 3) & 7;
                const bool no_index = (index == 4);    //SIB.index=100 表示没有变址寄存器
                if (mod == 0 && base == 5)
                {
                    //只有“既没基址、也没变址”时这个 disp32 才是 rip 相对的；
                    //有变址寄存器时它是普通绝对 disp32，原样搬走就行
                    if (no_index)
                    {
                        info.disp_offset = i;
                        info.disp_size = 4;
                    }
                    i += 4;
                }
                else if (mod == 1)
                    i += 1;
                else if (mod == 2)
                    i += 4;
            }
            else if (mod == 0 && rm == 5)
            {
                //rip 相对寻址：位移相对“下一条指令”，搬走必须重算
                info.disp_offset = i;
                info.disp_size = 4;
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
        else if (flags & F_IMM16)
            i += 2;
        else if (flags & F_IMMV)
        {
            size_t sz = imm16 ? 2 : 4;
            if (!two_byte && rex_w && op >= 0xB8 && op <= 0xBF)
                sz = 8;       //movabs r64, imm64
            i += sz;
        }
    }

    if (i > available || i > 15)
        return false;
    info.length = i;
    return true;
}

static constexpr size_t kMaxPrologueInstrs = 12;
static constexpr size_t kCodeWindow = 40;

struct ProloguePlan
{
    InstrInfo instrs[kMaxPrologueInstrs]{};
    size_t count{};
    size_t total_length{};
    bool has_relative{};      //开头这段里有没有相对地址（有的话 trampoline 必须落在目标 ±2GB 内）
};

//从头走完整指令直到覆盖够 kPatchSize；只有在不认识指令、或指令条数超上限时才失败
static bool BuildPrologue(const unsigned char* code, ProloguePlan& plan)
{
    plan = ProloguePlan{};
    size_t offset = 0;
    while (offset < kPatchSize)
    {
        if (plan.count >= kMaxPrologueInstrs || offset >= kCodeWindow)
            return false;
        InstrInfo info;
        if (!DecodeOne(code + offset, kCodeWindow - offset, info))
            return false;
        plan.instrs[plan.count++] = info;
        if (info.disp_size != 0)
            plan.has_relative = true;
        offset += info.length;
    }
    plan.total_length = offset;
    return true;
}

//把开头这段指令复制到 trampoline，并按新位置重算所有相对地址
static bool RelocatePrologue(const ProloguePlan& plan, const unsigned char* src_code, unsigned char* dst,
                             uintptr_t src_base, uintptr_t dst_base)
{
    size_t off = 0;
    for (size_t n = 0; n < plan.count; n++)
    {
        const InstrInfo& info = plan.instrs[n];
        memcpy(dst + off, src_code + off, info.length);
        if (info.disp_size != 0)
        {
            //相对地址算的是“距离下一条指令”：指令长度没变，但基准地址变了
            const LONG64 orig_next = static_cast<LONG64>(src_base + off + info.length);
            const LONG64 new_next = static_cast<LONG64>(dst_base + off + info.length);
            if (info.disp_size == 4)
            {
                int32_t old_disp{};
                memcpy(&old_disp, src_code + off + info.disp_offset, sizeof(old_disp));
                const LONG64 delta = (orig_next + old_disp) - new_next;
                if (delta < -2147483648LL || delta > 2147483647LL)
                    return false;
                const int32_t new_disp = static_cast<int32_t>(delta);
                memcpy(dst + off + info.disp_offset, &new_disp, sizeof(new_disp));
            }
            else
            {
                const signed char old_disp = static_cast<signed char>(src_code[off + info.disp_offset]);
                const LONG64 delta = (orig_next + old_disp) - new_next;
                if (delta < -128 || delta > 127)
                    return false;     //rel8 放不下，又不能改长度（后面指令的偏移会全乱），只能放弃
                dst[off + info.disp_offset] = static_cast<unsigned char>(static_cast<signed char>(delta));
            }
        }
        off += info.length;
    }
    return true;
}

size_t ReadTargetBytes(void* address, unsigned char* buffer, size_t count)
{
    if (address == nullptr || buffer == nullptr || count == 0)
        return 0;
    MEMORY_BASIC_INFORMATION info{};
    if (::VirtualQuery(address, &info, sizeof(info)) == 0)
        return 0;
    if (info.State != MEM_COMMIT || info.Protect == PAGE_NOACCESS || (info.Protect & PAGE_GUARD) != 0)
        return 0;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (region_end - addr < count)
        return 0;         //跨页就整段不读，省得读到未提交的下一页
    memcpy(buffer, address, count);
    return count;
}

bool IsReadableCodeAddress(void* address)
{
    if (address == nullptr)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (::VirtualQuery(address, &info, sizeof(info)) == 0)
        return false;
    if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0)
        return false;
    return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

//函数开头如果只是一串 jmp（增量链接留下的跳转表、加载器打补丁留下的跳板），
//顺着跳走到真正的实现，补丁打在真函数上更稳（真函数开头一般是 push/sub/mov，不带相对地址）。
//最多走 6 跳，任何一跳不靠谱就停在原地。
static void* ResolveFunctionThunk(void* address)
{
    uintptr_t cur = reinterpret_cast<uintptr_t>(address);
    for (int hop = 0; hop < 6; hop++)
    {
        unsigned char buf[16]{};
        if (ReadTargetBytes(reinterpret_cast<void*>(cur), buf, sizeof(buf)) < sizeof(buf))
            break;
        size_t i = 0;
        while (i < 4 && buf[i] >= 0x40 && buf[i] <= 0x4F)
            i++;          //只允许 REX 前缀
        uintptr_t next = 0;
        if (buf[i] == 0xE9)
        {
            int32_t disp{};
            memcpy(&disp, buf + i + 1, sizeof(disp));
            next = static_cast<uintptr_t>(static_cast<LONG64>(cur + i + 5) + disp);
        }
        else if (buf[i] == 0xEB)
        {
            const signed char disp = static_cast<signed char>(buf[i + 2]);
            next = static_cast<uintptr_t>(static_cast<LONG64>(cur + i + 3) + disp);
        }
        else if (buf[i] == 0xFF && buf[i + 1] == 0x25)
        {
            //jmp qword ptr [rip+disp32] —— 操作数是个指针，取出来再跳
            int32_t disp{};
            memcpy(&disp, buf + i + 2, sizeof(disp));
            const uintptr_t ptr_addr = static_cast<uintptr_t>(static_cast<LONG64>(cur + i + 6) + disp);
            unsigned char pointer_bytes[8]{};
            if (ReadTargetBytes(reinterpret_cast<void*>(ptr_addr), pointer_bytes, sizeof(pointer_bytes)) < sizeof(pointer_bytes))
                break;
            uint64_t target_value{};
            memcpy(&target_value, pointer_bytes, sizeof(target_value));
            if (target_value == 0 || target_value > 0x0000FFFFFFFFFFFFULL)
                break;
            next = static_cast<uintptr_t>(target_value);
        }
        else
        {
            break;          //不是纯跳转，这里就是函数真正开始的地方
        }
        if (!IsReadableCodeAddress(reinterpret_cast<void*>(next)))
            break;
        cur = next;
    }
    return reinterpret_cast<void*>(cur);
}

//在目标 ±2GB 内找一块可执行内存：带相对地址的指令搬过去后位移才放得下
static void* AllocNearbyTrampoline(void* target, size_t size)
{
    const uintptr_t target_addr = reinterpret_cast<uintptr_t>(target);
    const uintptr_t gran = 0x10000;
    const uintptr_t first = (target_addr / gran) * gran;
    for (int ring = 1; ring <= 512; ring++)
    {
        for (int sign = -1; sign <= 1; sign += 2)
        {
            const LONG64 candidate = static_cast<LONG64>(first) + static_cast<LONG64>(ring) * static_cast<LONG64>(gran) * sign;
            if (candidate < 0x10000 || candidate > 0x7FFFFFFFE000LL)
                continue;
            const LONG64 delta = candidate - static_cast<LONG64>(target_addr);
            if (delta <= -0x70000000LL || delta >= 0x70000000LL)
                continue;
            void* p = ::VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(candidate)), size,
                MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p != nullptr)
                return p;
        }
    }
    return nullptr;
}

//在 trampoline 末尾写一条跳回 target+overwritten 的 jmp：能用 5 字节就用，不行就用 14 字节绝对跳
static size_t EmitJumpBack(unsigned char* dst, uintptr_t dst_base, size_t dst_offset, uintptr_t to)
{
    const LONG64 delta = static_cast<LONG64>(to) - (static_cast<LONG64>(dst_base) + static_cast<LONG64>(dst_offset + 5));
    if (delta >= -2147483648LL && delta <= 2147483647LL)
    {
        dst[dst_offset] = 0xE9;
        const int32_t d = static_cast<int32_t>(delta);
        memcpy(dst + dst_offset + 1, &d, sizeof(d));
        return 5;
    }
    dst[dst_offset] = 0xFF;
    dst[dst_offset + 1] = 0x25;
    dst[dst_offset + 2] = 0;
    dst[dst_offset + 3] = 0;
    dst[dst_offset + 4] = 0;
    dst[dst_offset + 5] = 0;
    const uint64_t abs_target = to;
    memcpy(dst + dst_offset + 6, &abs_target, sizeof(abs_target));
    return 14;
}

bool InstallInlineDetour(InlineDetour& detour, void* target, void* hook, DetourFail* fail_reason)
{
    detour = InlineDetour{};
    detour.slot_address = target;
    DetourFail fail = kDetourFailNone;

    if (target == nullptr || hook == nullptr)
    {
        fail = kDetourFailBadArgs;
    }
    else if (!IsReadableCodeAddress(target))
    {
        fail = kDetourFailUnreadable;
    }
    else
    {
        void* real_target = ResolveFunctionThunk(target);
        detour.target = real_target;
        detour.thunk_followed = (real_target != target);

        unsigned char code_bytes[kCodeWindow]{};
        ProloguePlan plan;
        if (ReadTargetBytes(real_target, code_bytes, sizeof(code_bytes)) < kPatchSize)
        {
            fail = kDetourFailUnreadable;
        }
        else if (!BuildPrologue(code_bytes, plan))
        {
            fail = kDetourFailBadInstruction;
        }
        else if (plan.total_length > sizeof(detour.saved_bytes))
        {
            fail = kDetourFailTooLong;
        }
        else
        {
            const size_t overwritten = plan.total_length;
            detour.overwritten_length = overwritten;
            const uintptr_t src_base = reinterpret_cast<uintptr_t>(real_target);

            const size_t tramp_size = overwritten + 16;
            void* tramp = nullptr;
            //指令里有相对地址时，trampoline 必须落在目标 ±2GB 内，否则重算后的位移放不下
            if (plan.has_relative)
                tramp = AllocNearbyTrampoline(real_target, tramp_size);
            if (tramp == nullptr)
                tramp = ::VirtualAlloc(nullptr, tramp_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

            if (tramp == nullptr)
            {
                detour.overwritten_length = 0;
                fail = kDetourFailNoMemory;
            }
            else
            {
                const uintptr_t dst_base = reinterpret_cast<uintptr_t>(tramp);
                unsigned char* dst = static_cast<unsigned char*>(tramp);
                if (!RelocatePrologue(plan, code_bytes, dst, src_base, dst_base))
                {
                    ::VirtualFree(tramp, 0, MEM_RELEASE);
                    detour.overwritten_length = 0;
                    fail = kDetourFailFarTarget;
                }
                else
                {
                    detour.trampoline = tramp;
                    const size_t tail = EmitJumpBack(dst, dst_base, overwritten, src_base + overwritten);
                    detour.trampoline_length = overwritten + tail;
                    ::FlushInstructionCache(::GetCurrentProcess(), tramp, detour.trampoline_length);

                    DWORD old_protect{};
                    if (!::VirtualProtect(real_target, overwritten, PAGE_EXECUTE_READWRITE, &old_protect))
                    {
                        ::VirtualFree(tramp, 0, MEM_RELEASE);
                        detour.trampoline = nullptr;
                        detour.overwritten_length = 0;
                        fail = kDetourFailProtect;
                    }
                    else
                    {
                        memcpy(detour.saved_bytes, code_bytes, overwritten);
                        //jmp [rip+0] + 8 字节绝对地址：不依赖相对距离，钩子函数在哪都能跳
                        dst = static_cast<unsigned char*>(real_target);
                        dst[0] = 0xFF;
                        dst[1] = 0x25;
                        dst[2] = 0;
                        dst[3] = 0;
                        dst[4] = 0;
                        dst[5] = 0;
                        const uint64_t hook_addr = reinterpret_cast<uint64_t>(hook);
                        memcpy(dst + 6, &hook_addr, sizeof(hook_addr));
                        ::VirtualProtect(real_target, overwritten, old_protect, &old_protect);
                        ::FlushInstructionCache(::GetCurrentProcess(), real_target, overwritten);
                        detour.installed = true;
                        fail = kDetourFailNone;
                    }
                }
            }
        }
    }

    if (fail_reason != nullptr)
        *fail_reason = fail;
    return detour.installed;
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
