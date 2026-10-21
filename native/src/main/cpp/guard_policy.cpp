#include "guard_policy.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace blockdirs {

namespace {

// 路径缓冲上限。刻意不依赖 PATH_MAX（不同 libc 放的位置不一致）。
constexpr size_t kMaxPath = 4096;

constexpr const char *kStoragePrefix = "/storage/";
constexpr const char *kMediaRwPrefix = "/mnt/media_rw/";
constexpr const char *kSecurePrefix = "/mnt/secure/";
constexpr const char *kUserPrefix = "/mnt/user/";
constexpr const char *kRuntimePrefix = "/mnt/runtime/";

/** 「卷根 + 第一层目录名」这一层是否还不存在（deeper 必须是 name 里的 '/'） */
bool is_first_level_missing(const char *norm, const char *name, const char *deeper) {
    size_t prefix_len = static_cast<size_t>(name - norm);   // 含卷根末尾的 '/'
    size_t name_len = static_cast<size_t>(deeper - name);

    char first_level[kMaxPath];
    if (prefix_len + name_len >= sizeof(first_level)) {
        return false;
    }
    memcpy(first_level, norm, prefix_len + name_len);
    first_level[prefix_len + name_len] = '\0';

    struct stat st{};
    return stat(first_level, &st) != 0;   // 父层不在 -> 子路径也拦
}

}  // namespace

bool is_top_level_on_external(const char *path) {
    if (path == nullptr || *path == '\0') {
        return false;
    }

    // 去掉尾部 '/'（mkdir("/a/b/") 与 mkdir("/a/b") 语义相同）
    char norm[kMaxPath];
    size_t len = strlen(path);
    if (len >= sizeof(norm)) {
        return false;
    }
    memcpy(norm, path, len + 1);
    while (len > 1 && norm[len - 1] == '/') {
        norm[--len] = '\0';
    }

    const char *base = nullptr;

    if (strncmp(norm, kStoragePrefix, strlen(kStoragePrefix)) == 0) {
        base = norm + strlen(kStoragePrefix);
    } else if (strncmp(norm, kMediaRwPrefix, strlen(kMediaRwPrefix)) == 0) {
        base = norm + strlen(kMediaRwPrefix);
    } else if (strncmp(norm, kSecurePrefix, strlen(kSecurePrefix)) == 0) {
        base = norm + strlen(kSecurePrefix);
    } else if (strncmp(norm, kUserPrefix, strlen(kUserPrefix)) == 0) {
        // /mnt/user/<userId>/<UUID>/<name> —— 跳过 userId 这一层
        const char *sep = strchr(norm + strlen(kUserPrefix), '/');
        if (sep == nullptr) {
            return false;
        }
        base = sep + 1;
    } else if (strncmp(norm, kRuntimePrefix, strlen(kRuntimePrefix)) == 0) {
        // /mnt/runtime/<access>/<UUID>/<name> —— 跳过 access 这一层
        const char *sep = strchr(norm + strlen(kRuntimePrefix), '/');
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

    // 更深层：只有「第一层目录还不存在」时才拦（见头文件里的原因说明）
    const char *deeper = strchr(name, '/');
    if (deeper != nullptr) {
        return is_first_level_missing(norm, name, deeper);
    }

    return true;
}

const char *resolve_mkdirat_path(int dirfd, const char *path, char *out, size_t out_size) {
    if (path == nullptr || *path == '\0' || out == nullptr || out_size == 0) {
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
    size_t base_len = static_cast<size_t>(got);
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

}  // namespace blockdirs
