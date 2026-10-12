#include <jni.h>
#include <limits.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/stat.h>

#include "shadowhook.h"

#define TAG "BlockDirsNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// 原始函数指针（由 shadowhook 回填）
static int (*orig_mkdir)(const char *, mode_t) = nullptr;
static int (*orig_mkdirat)(int, const char *, mode_t) = nullptr;

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
    static thread_local char norm[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(norm)) {
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
        const char *slash = strchr(norm + 10, '/');
        if (slash == nullptr) {
            return false;
        }
        base = slash + 1;
    } else if (strncmp(norm, "/mnt/runtime/", 13) == 0) {
        // /mnt/runtime/<access>/<UUID>/<name> —— 跳过 access 这一层
        const char *slash = strchr(norm + 13, '/');
        if (slash == nullptr) {
            return false;
        }
        base = slash + 1;
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

    static thread_local char resolved[PATH_MAX];

    if (dirfd == AT_FDCWD) {
        if (getcwd(resolved, sizeof(resolved)) == nullptr) {
            return false;
        }
    } else {
        char link[64];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", dirfd);
        ssize_t n = readlink(link, resolved, sizeof(resolved) - 1);
        if (n <= 0) {
            return false;
        }
        resolved[n] = '\0';
    }

    // 拼接成 "<resolved>/<path>"
    size_t base_len = strlen(resolved);
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
static int my_mkdir(const char *path, mode_t mode) {
    if (is_top_level_on_external(path)) {
        LOGW("BLOCKED mkdir (top-level on external volume): %s", path);
        return 0;
    }
    return orig_mkdir(path, mode);
}

// Hook 后的 mkdirat
static int my_mkdirat(int dirfd, const char *path, mode_t mode) {
    if (is_top_level_on_external_at(dirfd, path)) {
        LOGW("BLOCKED mkdirat (top-level on external volume): dirfd=%d path=%s", dirfd, path);
        return 0;
    }
    return orig_mkdirat(dirfd, path, mode);
}

extern "C" JNIEXPORT void JNICALL
Java_top_yixiangren_blockdirs_nativebridge_BlockDirsNative_installNativeHook(JNIEnv *env, jclass clazz) {
    LOGI("installNativeHook: block ALL top-level dirs on external volumes");

    // 初始化 shadowhook（默认 inline 模式）
    if (shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false) != 0) {
        LOGE("shadowhook_init failed");
        return;
    }

    // Hook libc 的 mkdir
    void *mkdir_stub = shadowhook_hook_sym_name(
            "libc.so", "mkdir", (void *)my_mkdir, (void **)&orig_mkdir);
    if (mkdir_stub != nullptr) {
        LOGI("mkdir hooked, orig=%p", (void *)orig_mkdir);
    } else {
        LOGE("mkdir hook failed");
    }

    // Hook libc 的 mkdirat
    void *mkdirat_stub = shadowhook_hook_sym_name(
            "libc.so", "mkdirat", (void *)my_mkdirat, (void **)&orig_mkdirat);
    if (mkdirat_stub != nullptr) {
        LOGI("mkdirat hooked, orig=%p", (void *)orig_mkdirat);
    } else {
        LOGE("mkdirat hook failed");
    }
}
