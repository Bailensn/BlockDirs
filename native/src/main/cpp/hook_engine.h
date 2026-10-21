// BlockDirs 的最小 arm64 inline hook 引擎（纯 POSIX，不依赖 Android / 任何第三方 hook 框架）
//
// 单独抽出来的原因：它可以在 x86 CI 上用 aarch64 交叉编译器编译、再用 qemu-aarch64
// 真执行一遍（arm64 指令真的被解码、跳转真的发生），从而在**不碰真机**的前提下验证：
//   * 跳板页是否落在 b 指令 ±128MB 范围内
//   * 跳板编码（ldr x16, #8; br x16; .quad proxy）是否正确
//   * 入口改写后的 b 偏移解码回来是否正好等于跳板地址
//   * 改完之后调用目标函数是否真的进到代理函数、返回值是否正确
//
// 真机上翻过的车（跳到未映射地址）就属于这层的锅，必须先过测试。
#pragma once

#include <cstdint>

namespace blockdirs {

/** hook 结果；失败时 reason 是给人看的静态字符串 */
struct HookResult {
    bool ok;
    const char *reason;
    void *target;   // 被改写的函数入口
    void *stub;     // 跳板页（成功时有效）
};

/**
 * 把 target 函数入口的 4 字节改成 `b 跳板`，跳板间接跳到 proxy。
 *
 * 注意：proxy 必须自己重实现被替换函数的行为 —— 本引擎不保存、也不调用原函数，
 * 因为这正是历史上出问题的地方（依赖框架保存的「原函数指针」）。
 *
 * @param target 目标函数入口地址
 * @param proxy 代理函数地址（arm64 上要求入口带 `bti c`，编译时加
 *              -mbranch-protection=standard 即可）
 */
HookResult hook_address(void *target, void *proxy);

/** 判断两个地址之间是否在 arm64 `b` 指令的 ±128MB 可达范围内 */
bool within_branch_range(uintptr_t stub, uintptr_t target);

}  // namespace blockdirs
