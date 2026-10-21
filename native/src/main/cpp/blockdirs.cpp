#include <jni.h>
#include <string.h>
#include <stdio.h>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/stat.h>

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

    // 更深层不拦
    if (strchr(name, '/') != nullptr) {
        return false;
    }

    return true;
}

// mkdirat 允许相对路径（dirfd + name）。把 dirfd 还原成绝对路径后再判断，
// 否则 libfuse 之类用相对路径建目录的调用方会被漏掉。
static bool is_top_level_on_external_at(int dirfd, const char *path) {
    if (path == nullptr || *path == '\0') {
        return false;
    }
    if (path[0] == '/') {
        return is_top_level_on_external(path);
    }

    // 相对 CWD 的调用（AT_FDCWD）与我们的目标无关，直接放过
    if (dirfd < 0) {
        return false;
    }

    char link[32];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", dirfd);

    char resolved[BLOCKDIRS_MAX_PATH];
    ssize_t got = readlink(link, resolved, sizeof(resolved) - 1);
    if (got <= 0) {
        return false;
    }
    resolved[got] = '\0';

    // 拼接成 "<resolved>/<path>"
    size_t base_len = (size_t) got;
    size_t name_len = strlen(path);
    if (base_len == 0 || base_len + 1 + name_len >= sizeof(resolved)) {
        return false;
    }
    if (resolved[base_len - 1] != '/') {
        resolved[base_len++] = '/';
    }
    memcpy(resolved + base_len, path, name_len + 1);

    return is_top_level_on_external(resolved);
}

// Hook 后的 mkdir
//
// 两条 ShadowHook 的硬性要求（同步自上游 0dd79e6a，他们后来 revert 时误删了）：
//  1) 必须调用 SHADOWHOOK_STACK_SCOPE()（C++ RAII，函数退出时自动 pop_stack）。
//     否则 shadowhook 的 hub 会认为上一次代理调用「还在递归中」，
//     从第二次调用起就不再回调本代理函数 —— 表现为「hook 只生效一次」。
//  2) 调用原函数必须用 SHADOWHOOK_CALL_PREV 宏，不能直接调裸函数指针。
static int my_mkdir(const char *path, mode_t mode) {
    SHADOWHOOK_STACK_SCOPE();

    if (is_top_level_on_external(path)) {
        LOGW("BLOCKED mkdir (top-level on external volume): %s", path);
        return 0;
    }
    return SHADOWHOOK_CALL_PREV(my_mkdir, path, mode);
}

// Hook 后的 mkdirat
static int my_mkdirat(int dirfd, const char *path, mode_t mode) {
    SHADOWHOOK_STACK_SCOPE();

    if (is_top_level_on_external_at(dirfd, path)) {
        LOGW("BLOCKED mkdirat (top-level on external volume): dirfd=%d path=%s", dirfd, path);
        return 0;
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
Java_top_yixiangren_blockdirs_nativebridge_BlockDirsNative_installNativeHook(JNIEnv *env, jclass clazz) {
    LOGI("installNativeHook: block ALL top-level dirs on external volumes");

    if (!ensure_shadowhook()) return;

    hook_libc_symbol("mkdir", (void *) my_mkdir, (void **) &orig_mkdir, &mkdir_stub);
    hook_libc_symbol("mkdirat", (void *) my_mkdirat, (void **) &orig_mkdirat, &mkdirat_stub);
}
