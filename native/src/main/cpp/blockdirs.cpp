#include <jni.h>
#include <string.h>
#include <stdio.h>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/stat.h>
#include <time.h>
#include <atomic>

#include "shadowhook.h"

#define TAG "BlockDirsNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// 路径缓冲上限。
// 刻意不依赖 PATH_MAX：不同 libc 把它放在 <limits.h> / <sys/param.h> / <linux/limits.h>
// 的位置不一致，自己定一个常量最省事。
#define BLOCKDIRS_MAX_PATH 4096

// 原始函数指针（由 shadowhook 回填）
static int (*orig_mkdir)(const char *, mode_t) = nullptr;
static int (*orig_mkdirat)(int, const char *, mode_t) = nullptr;

// shadowhook 返回的 stub（进程级共享）。
// 热重载只会替换 Java 类加载器，libxposed 不会 dlclose 模块的 .so、也不会调 JNI_OnUnload，
// 所以这里的静态状态在同一个进程里跨「模块世代」依然有效：新一代实例再次调用
// installNativeHook 会被下面的守卫挡住，不会重复 hook。
static void *mkdir_stub = nullptr;
static void *mkdirat_stub = nullptr;
static bool shadowhook_ready = false;

// Hook 工作模式（由 Java 侧传入；热重载时会重新设置）：
//   0 = 真拦截，只在 MediaProvider 里用
//   1 = 只记日志、绝不改变行为，system_server 里用 —— 用来排查到底是谁在建目录，
//       且不可能影响开机（LSPosed 安全模式那条路彻底堵死）
#define BLOCKDIRS_MODE_BLOCK 0
#define BLOCKDIRS_MODE_LOG_ONLY 1
static int hook_mode = BLOCKDIRS_MODE_BLOCK;

// 熔断：短时间内拦截次数爆表说明有代码在死循环重试（FUSE / 媒体扫描都可能），
// 这时放行（fail open）—— 宁可漏拦也不能把进程拖死在 hook 里。
#define BLOCKDIRS_BURST_LIMIT 512
#define BLOCKDIRS_BURST_WINDOW_SEC 5
static std::atomic<long long> burst_window_start{0};
static std::atomic<long long> burst_blocks{0};
static std::atomic<bool> burst_tripped{false};

// 判断路径是否是「外置卷根目录下的第一层目录」。
//
// 与 Kotlin 侧 DirGuardPolicy.isTopLevelOnExternal 保持一致，改一处必须同步另一处。
//
// 支持的路径形态（同一个卷在不同挂载命名空间下的多种写法）：
//   /storage/<UUID>/<name>
//   /mnt/media_rw/<UUID>/<name>
//   /mnt/secure/<...>/<name>
//   /mnt/user/<userId>/<UUID>/<name>
//   /mnt/runtime/<access>/<UUID>/<name>
// 排除：内部存储 emulated、更深层子目录、卷根本身。
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
    // 真实结果只会是 ENOENT；而 ENOENT 会以异常形式往上传 —— 在 system_server 的开机
    // 流程里这足以把进程搞挂，然后 LSPosed 判定开机失败、进它自己的安全模式（全模块禁用）。
    // 所以这里改成「父目录不在就也返回成功」，让调用方以为建好了，避免异常冒泡。
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

// Hook 后的 mkdir
//
// 两条 ShadowHook 的硬性要求（同步自上游 0dd79e6a，他们后来 revert 时误删了）：
//  1) 必须调用 SHADOWHOOK_STACK_SCOPE()（C++ RAII，函数退出时自动 pop_stack）。
//     否则 shadowhook 的 hub 会认为上一次代理调用「还在递归中」，
//     从第二次调用起就不再回调本代理函数 —— 表现为「hook 只生效一次」。
//  2) 调用原函数必须用 SHADOWHOOK_CALL_PREV 宏，不能直接调裸函数指针。
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
 * 统一处理一次 `mkdir` 类调用。
 *
 * @return 需要由调用方返回给系统时给出返回值；返回 false 表示应由代理原样调用原函数
 */
static bool handle_top_level_mkdir(const char *syscall_name, const char *path, int *err_out) {
    if (!is_top_level_on_external(path)) {
        return false;
    }
    if (hook_mode == BLOCKDIRS_MODE_LOG_ONLY) {
        LOGW("HOOKED(log-only) %s (top-level on external volume): %s", syscall_name, path);
        return false;   // 只记录，绝不改变行为
    }
    if (burst_guard_should_pass()) {
        LOGW("MELTED %s (too many blocks in a row), pass through: %s", syscall_name, path);
        return false;
    }
    LOGW("BLOCKED %s (top-level on external volume): %s", syscall_name, path);
    *err_out = 0;   // 假装成功
    return true;
}

static int my_mkdir(const char *path, mode_t mode) {
    SHADOWHOOK_STACK_SCOPE();

    int result = 0;
    if (handle_top_level_mkdir("mkdir", path, &result)) {
        return result;
    }
    return SHADOWHOOK_CALL_PREV(my_mkdir, path, mode);
}

// Hook 后的 mkdirat
static int my_mkdirat(int dirfd, const char *path, mode_t mode) {
    SHADOWHOOK_STACK_SCOPE();

    // 相对路径要先经 /proc/self/fd/<dirfd> 还原成绝对路径
    char absolute[BLOCKDIRS_MAX_PATH];
    const char *resolved = resolve_mkdirat_path(dirfd, path, absolute, sizeof(absolute));

    if (resolved != nullptr) {
        int result = 0;
        if (handle_top_level_mkdir("mkdirat", resolved, &result)) {
            return result;
        }
    }
    return SHADOWHOOK_CALL_PREV(my_mkdirat, dirfd, path, mode);
}

/** 初始化 shadowhook（只做一次；热重载时不重复初始化） */
static bool ensure_shadowhook() {
    if (shadowhook_ready) return true;
    if (shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false) != 0) {
        LOGE("shadowhook_init failed, errno=%d", shadowhook_get_errno());
        return false;
    }
    shadowhook_ready = true;
    return true;
}

/** 同一个符号已经被 hook 过（上一代实例留下的，或别的模块先 hook 了） */
static bool is_duplicate_hook_error() {
    int err = shadowhook_get_errno();
    return err == SHADOWHOOK_ERRNO_HOOK_UNIQUE_DUP || err == SHADOWHOOK_ERRNO_HOOK_DUP;
}

/** hook 一个 libc 符号；已 hook / 重复时跳过而不报错（幂等，供热重载复用） */
static void hook_libc_symbol(const char *sym, void *new_addr, void **orig_addr, void **stub_out) {
    if (*stub_out != nullptr) return;

    void *stub = shadowhook_hook_sym_name("libc.so", sym, new_addr, orig_addr);
    if (stub != nullptr) {
        *stub_out = stub;
        LOGI("%s hooked, orig=%p", sym, *orig_addr);
        return;
    }
    if (is_duplicate_hook_error()) {
        LOGW("%s 已被 hook 过（上一代实例或其它模块），跳过", sym);
        return;
    }
    LOGE("%s hook failed, errno=%d", sym, shadowhook_get_errno());
}

extern "C" JNIEXPORT void JNICALL
Java_top_yixiangren_blockdirs_nativebridge_BlockDirsNative_installNativeHook(JNIEnv *env, jclass clazz, jint mode) {
    hook_mode = (mode == BLOCKDIRS_MODE_LOG_ONLY) ? BLOCKDIRS_MODE_LOG_ONLY : BLOCKDIRS_MODE_BLOCK;
    LOGI("installNativeHook: mode=%s", hook_mode == BLOCKDIRS_MODE_BLOCK ? "block" : "log-only(diagnostics)");

    if (!ensure_shadowhook()) return;

    hook_libc_symbol("mkdir", (void *) my_mkdir, (void **) &orig_mkdir, &mkdir_stub);
    hook_libc_symbol("mkdirat", (void *) my_mkdirat, (void **) &orig_mkdirat, &mkdirat_stub);
}
