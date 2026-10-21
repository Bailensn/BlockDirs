// 「位置判定法」的 native 实现（纯 POSIX，不依赖 Android）。
//
// 单独抽出来的原因：这是模块**唯一**决定"拦不拦"的逻辑，它必须和 Kotlin 侧
// DirGuardPolicy 完全一致，而且必须在 CI 上用真路径跑一遍（native/src/test）。
//
// 判定范围（同一个卷在不同挂载命名空间下的多种写法都覆盖）：
//   /storage/<UUID>/<name>
//   /mnt/media_rw/<UUID>/<name>
//   /mnt/secure/<...>/<name>
//   /mnt/user/<userId>/<UUID>/<name>
//   /mnt/runtime/<access>/<UUID>/<name>
// 排除：内部存储 emulated、卷根本身。
#pragma once

#include <cstddef>

namespace blockdirs {

/**
 * 判断路径是否是「外置卷根目录下的第一层目录」。
 *
 * 额外规则：第一层目录还不存在时，它下面的子路径也返回 true。
 * 原因：父目录被我们拦掉（返回成功但没建）之后，mkdir("<卷根>/Android/data") 真实结果
 * 只会是 ENOENT，ENOENT 会以异常形式往上传，可能把调用方搞挂（system_server 里就是
 * 开机失败 → LSPosed 进安全模式）。所以这里让它也「成功」。
 */
bool is_top_level_on_external(const char *path);

/**
 * 把 mkdirat 的 (dirfd, path) 还原成绝对路径。
 *
 * @return 成功返回 out，失败返回 nullptr（调用方直接放行）
 */
const char *resolve_mkdirat_path(int dirfd, const char *path, char *out, size_t out_size);

}  // namespace blockdirs
