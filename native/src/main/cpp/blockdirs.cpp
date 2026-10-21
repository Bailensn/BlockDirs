// BlockDirs native 层
//
// ============================================================================
// 为什么不再用 shadowhook（两次把用户手机搞崩后的结论，别再改回去）
// ============================================================================
//
// 第一版用 shadowhook 的 SHADOWHOOK_CALL_PREV，崩在 sh_hub_get_prev_func：
//     sh_hub_stack_t *stack = sh_safe_pthread_getspecific(sh_hub_stack_tls_key);
//     if (0 == stack->frames_cnt) ...      // ← stack 是 NULL，解引用 0x0
// 第二版退回直接调 orig 指针、但留着 SHADOWHOOK_STACK_SCOPE()，崩在 sh_hub_pop_stack，
// 是同一处 TLS 拿不到栈、同一个空指针。
//
// 也就是说：shadowhook 的「每线程 frame 栈」在这台机器（OnePlus / Android 16，
// 进程 com.android.providers.media.module 的 android.bg 线程）上根本建立不起来，
// 任何依赖它的 API（CALL_PREV / STACK_SCOPE）都会段错误。
// 崩的是 MediaProvider —— 它就是 FUSE 守护进程，崩了内部存储都挂不上。
//
// 现在的做法：完全不用 shadowhook。
//   * 直接改 libc 函数入口的 4 字节，跳到我们自己 mmap 出来的一小段跳板
//     （`ldr x16, #8; br x16; .quad proxy`），跳板再做一次间接跳转进代理函数；
//   * 代理函数内部**不调用原函数**，而是直接重做一次 syscall（mkdirat/mkdir 本来
//     就是 syscall 包装），所以没有任何「原函数指针」需要保存，也不需要搬迁指令；
//   * 不依赖任何 TLS / 框架内部状态，因此不会再有上面那类空指针；
//   * 重复安装安全：入口已经是 b 跳转时，再按同样规则算偏移照样跳得对。
//
// ============================================================================
// 安全设计
// ============================================================================
//  * 模式 0 = 真拦截（只在 MediaProvider）：命中返回 0 假装成功，但不真正建目录；
//    模式 1 = 只记日志（system_server 用）：只打一行 logcat，行为零改变，
//    所以就算勾了「系统框架」也不可能让开机失败。
//  * 熔断：5 秒内拦截超过 512 次 → 认为有代码在死循环重试，直接放行（fail open），
//    宁可漏拦也不能把进程拖死。
#include <jni.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>
#include <atomic>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <android/log.h>

#include "hook_engine.h"
#include "guard_policy.h"

#define TAG "BlockDirsNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// 路径缓冲上限。刻意不依赖 PATH_MAX（不同 libc 放的位置不一致）。
#define BLOCKDIRS_MAX_PATH 4096

// 工作模式（由 Java 侧传入）
#define BLOCKDIRS_MODE_BLOCK 0
#define BLOCKDIRS_MODE_LOG_ONLY 1
static std::atomic<int> hook_mode{BLOCKDIRS_MODE_BLOCK};

// 熔断参数
#define BLOCKDIRS_BURST_LIMIT 512
#define BLOCKDIRS_BURST_WINDOW_SEC 5
static std::atomic<long long> burst_window_start{0};
static std::atomic<long long> burst_blocks{0};
static std::atomic<bool> burst_tripped{false};

// 已安装标记（进程级；热重载只换 Java 类加载器，.so 不会被 dlclose）
static std::atomic<bool> hooks_installed{false};

// ============================================================================
// 熔断
// ============================================================================

/** 记一次拦截；返回 true 表示已熔断、调用方应当放行 */
static bool burst_guard_should_pass() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = (long long) ts.tv_sec;

    long long start = burst_window_start.load(std::memory_order_relaxed);
    if (start == 0 || now - start >= BLOCKDIRS_BURST_WINDOW_SEC) {
        burst_window_start.store(now, std::memory_order_relaxed);
        burst_blocks.store(0, std::memory_order_relaxed);
        burst_tripped.store(false, std::memory_order_relaxed);
        return false;
    }
    if (burst_tripped.load(std::memory_order_relaxed)) {
        return true;
    }
    long long count = burst_blocks.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > BLOCKDIRS_BURST_LIMIT) {
        burst_tripped.store(true, std::memory_order_relaxed);
        LOGE("拦截次数异常（%lld 次 / %d 秒），疑似死循环重试，熔断放行以免拖死进程",
             count, BLOCKDIRS_BURST_WINDOW_SEC);
        return true;
    }
    return false;
}

/**
 * 统一处理一次 mkdir 类调用。
 *
 * @return true 表示调用方应当直接返回 result_out（假装成功）；false 表示原样执行 syscall
 */
static bool handle_top_level_mkdir(const char *syscall_name, const char *path, int *result_out) {
    if (!blockdirs::is_top_level_on_external(path)) {
        return false;
    }
    if (hook_mode.load(std::memory_order_relaxed) == BLOCKDIRS_MODE_LOG_ONLY) {
        LOGW("HOOKED(log-only) %s (top-level on external volume): %s", syscall_name, path);
        return false;   // 只记录，绝不改变行为
    }
    if (burst_guard_should_pass()) {
        LOGW("MELTED %s (too many blocks), pass through: %s", syscall_name, path);
        return false;
    }
    LOGW("BLOCKED %s (top-level on external volume): %s", syscall_name, path);
    *result_out = 0;   // 假装成功
    return true;
}

// ============================================================================
// 代理函数（跳板跳进来的目标）
//
// 注意：这里**不调用原函数**，而是直接重做 syscall —— mkdirat/mkdir 本来就是
// syscall 包装，bionic 的 syscall() 自己会处理 errno。这样完全不需要保存原始
// 函数指针，也就不会碰 shadowhook 任何内部状态。
// ============================================================================

/** 直接做 mkdirat syscall；返回值语义与 libc 一致（失败返回 -1 并设 errno） */
static int do_mkdirat(int dirfd, const char *path, mode_t mode) {
    return (int) syscall(SYS_mkdirat, dirfd, path, mode);
}

extern "C" __attribute__((used, noinline, visibility("default")))
int blockdirs_mkdirat(int dirfd, const char *path, mode_t mode) {
    char absolute[BLOCKDIRS_MAX_PATH];
    const char *resolved = blockdirs::resolve_mkdirat_path(dirfd, path, absolute, sizeof(absolute));
    if (resolved != nullptr) {
        int blocked_value = 0;
        if (handle_top_level_mkdir("mkdirat", resolved, &blocked_value)) {
            return blocked_value;
        }
    }
    return do_mkdirat(dirfd, path, mode);
}

extern "C" __attribute__((used, noinline, visibility("default")))
int blockdirs_mkdir(const char *path, mode_t mode) {
    int blocked_value = 0;
    if (handle_top_level_mkdir("mkdir", path, &blocked_value)) {
        return blocked_value;
    }
    return do_mkdirat(AT_FDCWD, path, mode);
}

// ============================================================================
// 安装：dlsym 找符号 -> 交给 hook_engine 改入口
//
// hook_engine 是纯 POSIX 的（不依赖 Android），所以同一份代码能在 CI 上用
// aarch64 交叉编译器 + qemu-aarch64 真跑一遍单测（native/src/test）。
// ============================================================================

static void install_one(const char *symbol, void *proxy) {
    void *target = dlsym(RTLD_DEFAULT, symbol);
    if (target == nullptr) {
        LOGE("%s: dlsym 失败", symbol);
        return;
    }
    blockdirs::HookResult result = blockdirs::hook_address(target, proxy);
    if (!result.ok) {
        LOGE("%s: hook 失败（%s）", symbol, result.reason);
        return;
    }
    LOGI("%s hooked: target=%p stub=%p proxy=%p", symbol, result.target, result.stub, proxy);
}

// ============================================================================
// 设备自检（给 App 用，安全）
//
// 只给**本 .so 里的一个假函数**装 hook，绝不碰 libc。用来在真机上验证：
//   mmap 跳板 -> 改入口 -> 跳进代理 -> 返回值正确
// 这条链路在目标 ROM 上到底通不通（PAC / BTI / mprotect 权限都会影响它）。
// 自检通过 ≠ MediaProvider 里一定成功，但自检失败就一定不能开 native 层。
// ============================================================================

static std::atomic<int> g_self_test_calls{0};

extern "C" __attribute__((noinline, used)) int blockdirs_self_test_target(int a) {
    return a * 2;
}

extern "C" __attribute__((noinline, used)) int blockdirs_self_test_proxy(int a) {
    g_self_test_calls.fetch_add(1, std::memory_order_relaxed);
    return a * 2 + 1;
}

extern "C" JNIEXPORT jstring JNICALL
Java_top_yixiangren_blockdirs_nativebridge_BlockDirsNative_selfTest(
        JNIEnv *env, jobject thiz) {
    (void) thiz;
    char buf[256];

#if defined(__aarch64__)
    g_self_test_calls.store(0, std::memory_order_relaxed);
    blockdirs::HookResult result =
            blockdirs::hook_address((void *) blockdirs_self_test_target,
                                    (void *) blockdirs_self_test_proxy);
    if (!result.ok) {
        snprintf(buf, sizeof(buf), "FAIL(install): %s", result.reason);
        return env->NewStringUTF(buf);
    }

    // volatile 函数指针：阻止编译器内联/常量折叠，保证真的走一次调用
    int (*volatile fn)(int) = blockdirs_self_test_target;   // volatile 指针：强制每次真调用
    int value = fn(21);
    int calls = g_self_test_calls.load(std::memory_order_relaxed);
    if (value == 43 && calls == 1) {
        snprintf(buf, sizeof(buf), "OK: target=%p stub=%p delta=%lld", result.target,
                 result.stub,
                 (long long) ((intptr_t) result.stub - (intptr_t) result.target));
    } else {
        snprintf(buf, sizeof(buf), "FAIL(call): 返回 %d（期望 43）, proxy 调用 %d 次", value, calls);
    }
#else
    snprintf(buf, sizeof(buf), "SKIP: 当前 ABI 不是 arm64");
#endif

    return env->NewStringUTF(buf);
}

// ============================================================================
// JNI 入口
// ============================================================================

extern "C" JNIEXPORT void JNICALL
Java_top_yixiangren_blockdirs_nativebridge_BlockDirsNative_installNativeHook(
        JNIEnv *env, jobject thiz, jint mode) {
    (void) env;
    (void) thiz;

    hook_mode.store(mode == BLOCKDIRS_MODE_LOG_ONLY ? BLOCKDIRS_MODE_LOG_ONLY
                                                   : BLOCKDIRS_MODE_BLOCK,
                    std::memory_order_relaxed);
    LOGI("installNativeHook: mode=%s",
         mode == BLOCKDIRS_MODE_LOG_ONLY ? "log-only(diagnostics)" : "block");

    if (hooks_installed.exchange(true, std::memory_order_acq_rel)) {
        return;   // 幂等（热重载后新一代实例再调进来时只更新模式）
    }

    install_one("mkdirat", (void *) blockdirs_mkdirat);
    install_one("mkdir", (void *) blockdirs_mkdir);
}
