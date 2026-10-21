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
// 策略：判断路径是否是「外置卷根目录下的第一层目录」
//
// 与 Kotlin 侧 DirGuardPolicy.isTopLevelOnExternal 保持一致，改一处必须同步另一处。
//
// 支持的路径形态（同一个卷在不同挂载命名空间下的多种写法）：
//   /storage/<UUID>/<name>
//   /mnt/media_rw/<UUID>/<name>
//   /mnt/secure/<...>/<name>
//   /mnt/user/<userId>/<UUID>/<name>
//   /mnt/runtime/<access>/<UUID>/<name>
// 排除：内部存储 emulated、卷根本身。
// ============================================================================

/**
 * 判断「卷根 + 第一层目录名」这一层是否还不存在。
 *
 * @param norm 已经去掉尾部 '/' 的绝对路径
 * @param name 第一层目录名开始的位置（可能还带着更深的 "/xxx" 尾巴）
 */
static bool is_first_level_missing(const char *norm, const char *name) {
    const char *deeper = strchr(name, '/');       // 调用方保证非空
    size_t prefix_len = (size_t) (name - norm);   // 含卷根末尾的 '/'
    size_t name_len = (size_t) (deeper - name);

    char first_level[BLOCKDIRS_MAX_PATH];
    if (prefix_len + name_len >= sizeof(first_level)) {
        return false;
    }
    memcpy(first_level, norm, prefix_len + name_len);
    first_level[prefix_len + name_len] = '\0';

    struct stat st{};
    return stat(first_level, &st) != 0;   // 父层不在 -> 子路径也拦
}

static bool is_top_level_on_external(const char *path) {
    if (path == nullptr || *path == '\0') {
        return false;
    }

    // 去掉尾部 '/'（mkdir("/a/b/") 与 mkdir("/a/b") 语义相同）
    char norm[BLOCKDIRS_MAX_PATH];
    size_t len = strlen(path);
    if (len >= sizeof(norm)) {
        return false;
    }
    memcpy(norm, path, len + 1);
    while (len > 1 && norm[len - 1] == '/') {
        norm[--len] = '\0';
    }

    const char *base = nullptr;

    if (strncmp(norm, "/storage/", 9) == 0) {
        base = norm + 9;
    } else if (strncmp(norm, "/mnt/media_rw/", 14) == 0) {
        base = norm + 14;
    } else if (strncmp(norm, "/mnt/secure/", 12) == 0) {
        base = norm + 12;
    } else if (strncmp(norm, "/mnt/user/", 10) == 0) {
        // /mnt/user/<userId>/<UUID>/<name> —— 跳过 userId 这一层
        const char *sep = strchr(norm + 10, '/');
        if (sep == nullptr) {
            return false;
        }
        base = sep + 1;
    } else if (strncmp(norm, "/mnt/runtime/", 13) == 0) {
        // /mnt/runtime/<access>/<UUID>/<name> —— 跳过 access 这一层
        const char *sep = strchr(norm + 13, '/');
        if (sep == nullptr) {
            return false;
        }
        base = sep + 1;
    } else {
        return false;
    }

    // 排除内部共享存储 /storage/emulated/... 等
    if (strncmp(base, "emulated", 8) == 0) {
        return false;
    }

    // 找到卷 UUID 后的第一个 '/'，后面是第一层目录名
    const char *slash = strchr(base, '/');
    if (slash == nullptr) {
        return false;
    }

    const char *name = slash + 1;
    if (*name == '\0') {
        return false;
    }

    // 第一层目录不存在时，它下面的子路径也一并拦。
    //
    // 原因：父目录被我们拦掉（返回成功但没建），紧接着的 mkdir("<卷根>/Android/data")
    // 真实结果只会是 ENOENT；ENOENT 会以异常形式往上传，可能把调用方搞挂
    // （system_server 里就是开机失败 → LSPosed 进安全模式）。这里让它也「成功」。
    if (strchr(name, '/') != nullptr) {
        return is_first_level_missing(norm, name);
    }

    return true;
}

/**
 * 把 mkdirat 的 (dirfd, path) 还原成绝对路径。
 *
 * @param out 输出缓冲
 * @param out_size 缓冲大小
 * @return 成功时返回 out，失败返回 nullptr（此时调用方直接放行）
 */
static const char *resolve_mkdirat_path(int dirfd, const char *path, char *out, size_t out_size) {
    if (path == nullptr || *path == '\0') {
        return nullptr;
    }
    if (path[0] == '/') {
        size_t len = strlen(path);
        if (len >= out_size) {
            return nullptr;
        }
        memcpy(out, path, len + 1);
        return out;
    }

    // 相对 CWD 的调用（AT_FDCWD）与我们的目标无关，直接放过
    if (dirfd < 0) {
        return nullptr;
    }

    char link[32];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", dirfd);

    ssize_t got = readlink(link, out, out_size - 1);
    if (got <= 0) {
        return nullptr;
    }
    out[got] = '\0';

    // 拼接成 "<dirfd 目标>/<path>"
    size_t base_len = (size_t) got;
    size_t name_len = strlen(path);
    if (base_len == 0 || base_len + 1 + name_len >= out_size) {
        return nullptr;
    }
    if (out[base_len - 1] != '/') {
        out[base_len++] = '/';
    }
    memcpy(out + base_len, path, name_len + 1);

    return out;
}

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
    if (!is_top_level_on_external(path)) {
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
    const char *resolved = resolve_mkdirat_path(dirfd, path, absolute, sizeof(absolute));
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
