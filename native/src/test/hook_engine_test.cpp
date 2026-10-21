// hook_engine 的 arm64 真执行测试。
//
// 跑法（CI 里也是这套）：
//   aarch64-linux-gnu-g++ -std=c++17 -O1 -static -mbranch-protection=standard \
//       -Inative/src/main/cpp \
//       native/src/main/cpp/hook_engine.cpp native/src/test/hook_engine_test.cpp \
//       -o hook_engine_test
//   qemu-aarch64-static ./hook_engine_test
//
// 它在 arm64 上**真的**执行：真的 mmap 跳板、真的改写指令、真的跳进代理函数。
// 真机上翻过的「跳到未映射地址」这类问题，在这里就能被抓到。
#include "hook_engine.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_proxy_calls = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                 \
    do {                                 \
        if (!(cond)) {                   \
            printf("FAIL: " __VA_ARGS__); \
            printf("\n");                \
            g_failures++;                \
        }                                \
    } while (0)

// 被替换的目标函数：故意和 proxy 返回值不同，用来判断是否真的进了 proxy
extern "C" __attribute__((noinline, used)) int test_target(int a) {
    return a * 2;
}

// 代理函数：不调用原函数，自己重实现（和 blockdirs.cpp 里的做法一致）
extern "C" __attribute__((noinline, used)) int test_proxy(int a) {
    g_proxy_calls++;
    return a * 2 + 1;
}

int main() {
    // 用 volatile 函数指针，阻止编译器内联/常量折叠，保证真的走一次调用
    volatile int (*fp)(int) = test_target;

    // 0) 基线
    CHECK(fp(21) == 42, "基线返回值错误: %d", fp(21));

    // 1) 装 hook
    blockdirs::HookResult r = blockdirs::hook_address((void *) test_target, (void *) test_proxy);
    CHECK(r.ok, "hook_address 失败: %s", r.reason);
    if (!r.ok) {
        return 1;
    }
    printf("target=%p stub=%p delta=%lld\n", r.target, r.stub,
           (long long) ((intptr_t) r.stub - (intptr_t) r.target));

    // 2) 跳板必须落在 b 指令 ±128MB 内
    CHECK(blockdirs::within_branch_range((uintptr_t) r.stub, (uintptr_t) r.target),
          "跳板不在 ±128MB 内: target=%p stub=%p", r.target, r.stub);

    // 3) 入口那 4 字节确实是 b，且解码回来的目标正好是跳板
    uint32_t insn = 0;
    memcpy(&insn, (const void *) test_target, 4);
    CHECK((insn & 0xFC000000u) == 0x14000000u, "入口不是 b 指令: 0x%08x", insn);
    int64_t imm = (int64_t) (insn & 0x03FFFFFFu);
    if (imm & 0x02000000LL) {
        imm |= ~0x03FFFFFFLL;   // 符号扩展（负偏移）
    }
    uintptr_t decoded = (uintptr_t) test_target + (uintptr_t) (imm << 2);
    CHECK(decoded == (uintptr_t) r.stub, "b 的目标不对: 解码=%p 期望=%p",
          (void *) decoded, r.stub);

    // 4) 真的跳进去了，返回值来自 proxy
    CHECK(fp(21) == 43, "proxy 没被调用（返回 %d）", fp(21));
    CHECK(g_proxy_calls == 1, "proxy 调用次数=%d", g_proxy_calls);

    // 5) 连续调用都还在 proxy 里（验证不会「生效一次就失效」）
    for (int i = 0; i < 100; i++) {
        (void) fp(i);
    }
    CHECK(g_proxy_calls == 101, "批量调用后 proxy 次数=%d", g_proxy_calls);

    // 6) 重复安装（模拟模块热重载后的新一代实例）不应该崩
    blockdirs::HookResult r2 = blockdirs::hook_address((void *) test_target, (void *) test_proxy);
    CHECK(r2.ok, "重复 hook 失败: %s", r2.reason);
    CHECK(fp(21) == 43, "重复 hook 后调用异常: %d", fp(21));
    CHECK(g_proxy_calls == 102, "重复 hook 后 proxy 次数=%d", g_proxy_calls);

    // 7) 非法参数不能崩
    blockdirs::HookResult bad = blockdirs::hook_address(nullptr, (void *) test_proxy);
    CHECK(!bad.ok, "空 target 竟然成功了");

    if (g_failures == 0) {
        printf("OK: 全部断言通过（%d 次调用都进了 proxy）\n", g_proxy_calls);
        return 0;
    }
    printf("FAILED: %d 项断言失败\n", g_failures);
    return 1;
}
