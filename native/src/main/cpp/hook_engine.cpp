#include "hook_engine.h"

#include <cstring>
#include <sys/mman.h>

namespace blockdirs {

namespace {

constexpr size_t kPageSize = 4096;

#if defined(__aarch64__)

// 跳板编码：
//   ldr x16, #8      ; 从 pc+8 读 8 字节到 x16（字面量紧跟在 br 后面）
//   br  x16          ; 间接跳到 x16
//   .quad proxy
constexpr uint32_t kLdrX16Literal8 = 0x58000050u;
constexpr uint32_t kBrX16 = 0xD61F0200u;

constexpr uint32_t kBranchOpcode = 0x14000000u;
constexpr uint32_t kBranchImmMask = 0x03FFFFFFu;

/** 在目标函数附近 mmap 一页（先 RW，写完由 write_stub 改成 RX） */
void *map_stub_page(uintptr_t target) {
    uintptr_t base = target & ~(uintptr_t) (kPageSize - 1);

#ifdef MAP_FIXED_NOREPLACE
    for (int i = 1; i <= 0x80; i++) {
        for (int dir = -1; dir <= 1; dir += 2) {
            uintptr_t addr = base + (uintptr_t) dir * (uintptr_t) i * 0x10000;
            if (addr < 0x10000) {
                continue;
            }
            void *p = mmap(reinterpret_cast<void *>(addr), kPageSize, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) {
                if (within_branch_range(reinterpret_cast<uintptr_t>(p), target)) {
                    return p;
                }
                munmap(p, kPageSize);
            }
        }
    }
#endif

    // 退路：只给 hint，落点自己检查是否还在可达范围内
    void *p = mmap(reinterpret_cast<void *>(base), kPageSize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        if (within_branch_range(reinterpret_cast<uintptr_t>(p), target)) {
            return p;
        }
        munmap(p, kPageSize);
    }
    return nullptr;
}

/** 写跳板并改成只读可执行 */
bool write_stub(void *page, void *proxy) {
    auto *code = static_cast<uint32_t *>(page);
    code[0] = kLdrX16Literal8;
    code[1] = kBrX16;
    uintptr_t addr = reinterpret_cast<uintptr_t>(proxy);
    memcpy(&code[2], &addr, sizeof(addr));

    if (mprotect(page, kPageSize, PROT_READ | PROT_EXEC) != 0) {
        return false;
    }
    __builtin___clear_cache(static_cast<char *>(page), static_cast<char *>(page) + 16);
    return true;
}

/** 把入口 4 字节改成 `b stub` */
bool patch_entry(void *target, void *stub) {
    auto *site = static_cast<uint32_t *>(target);
    // 基准必须是 site 本身，不能是 site+1（见 within_branch_range 的注释）
    int64_t delta = static_cast<int64_t>(reinterpret_cast<uintptr_t>(stub)) -
                    static_cast<int64_t>(reinterpret_cast<uintptr_t>(site));
    if (delta < -(1LL << 27) || delta >= (1LL << 27)) {
        return false;
    }

    uintptr_t page_start = reinterpret_cast<uintptr_t>(site) & ~(uintptr_t) (kPageSize - 1);
    if (mprotect(reinterpret_cast<void *>(page_start), kPageSize,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }

    *site = kBranchOpcode | (static_cast<uint32_t>(delta >> 2) & kBranchImmMask);
    __builtin___clear_cache(reinterpret_cast<char *>(site),
                            reinterpret_cast<char *>(site) + 4);

    mprotect(reinterpret_cast<void *>(page_start), kPageSize, PROT_READ | PROT_EXEC);
    return true;
}

#endif  // __aarch64__

}  // namespace

bool within_branch_range(uintptr_t stub, uintptr_t target) {
    // 注意：AArch64 的 B 偏移是相对**指令自身地址**的（ARMv7 才是 PC+8/+4）。
    // 用 target+4 当基准会整体偏 4 字节 —— 真机上就是这么跳到「跳板 - 4」的未映射页崩掉的。
    int64_t delta = static_cast<int64_t>(stub) - static_cast<int64_t>(target);
    return delta >= -(1LL << 27) && delta < (1LL << 27);
}

HookResult hook_address(void *target, void *proxy) {
    if (target == nullptr || proxy == nullptr) {
        return {false, "target/proxy 为空", target, nullptr};
    }
#if defined(__aarch64__)
    void *page = map_stub_page(reinterpret_cast<uintptr_t>(target));
    if (page == nullptr) {
        return {false, "找不到可用的跳板页（±128MB 内没有空页）", target, nullptr};
    }
    if (!write_stub(page, proxy)) {
        munmap(page, kPageSize);
        return {false, "跳板页 mprotect(RX) 失败", target, nullptr};
    }
    if (!patch_entry(target, page)) {
        munmap(page, kPageSize);
        return {false, "入口改写失败（mprotect 或分支越界）", target, nullptr};
    }
    return {true, "ok", target, page};
#else
    return {false, "当前架构未实现（只支持 arm64）", target, nullptr};
#endif
}

}  // namespace blockdirs
