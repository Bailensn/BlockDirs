cmake_minimum_required(VERSION 4.0.2)
project(blockdirs)

set(CMAKE_CXX_STANDARD 17)

# 不再依赖 shadowhook（字节跳动 android-inline-hook）。
#
# 原因（两次真实崩溃复盘）：
#   * SHADOWHOOK_CALL_PREV   → sh_hub_get_prev_func 里 stack 为 NULL → SIGSEGV
#   * SHADOWHOOK_STACK_SCOPE → sh_hub_pop_stack     里 stack 为 NULL → SIGSEGV
#   两者都依赖 shadowhook 的「每线程 frame 栈」（pthread TLS），而在
#   com.android.providers.media.module 的 android.bg 线程上这个栈根本建立不起来。
#   MediaProvider 是 FUSE 守护进程，崩了内部存储都挂不上。
#
# 现在 native 层完全自包含：自己 mmap 跳板页 + 改 libc 函数入口 4 字节 + 直接重做 syscall。
# 好处：不依赖任何第三方 hook 框架的 TLS / 状态机，构建也快（不用 FetchContent 拉整个仓库）。

add_library(blockdirs SHARED
    blockdirs.cpp
    hook_engine.cpp)

# hook_engine 是纯 POSIX 的（不依赖 Android），CI 会用 aarch64 交叉编译器 + qemu 对它
# 跑真执行单测（native/src/test/hook_engine_test.cpp），所以别往里加 Android 头文件。

target_compile_options(blockdirs PRIVATE
    # arm64 上让每个函数入口带 `bti c`：我们是用 `br x16` 间接跳进代理函数的，
    # 如果系统启用了 BTI，落在非 bti 指令上会直接 fault。
    -mbranch-protection=standard
    -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(blockdirs
    log)
