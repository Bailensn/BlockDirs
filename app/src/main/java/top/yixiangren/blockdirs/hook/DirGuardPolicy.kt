package top.yixiangren.blockdirs.hook

/**
 * 「位置判定法」的共享实现。
 *
 * 不硬编码任何目录名（Android / Music / Pictures / DCIM / Movies …），
 * 而是按路径结构判断：只要是「外置卷根目录下的第一层目录」，一律拦截。
 * 这样不存在「漏掉某个标准目录名」的风险，也不会影响内部存储和更深层子目录。
 *
 * 生效范围（同一个卷在不同挂载命名空间下的多种写法都覆盖）：
 * - `/storage/<UUID>/<name>`
 * - `/mnt/media_rw/<UUID>/<name>`
 * - `/mnt/secure/<...>/<name>`
 * - `/mnt/user/<userId>/<UUID>/<name>`
 * - `/mnt/runtime/<access>/<UUID>/<name>`
 *
 * 排除：
 * - 内部共享存储：`/storage/emulated/...`、`/mnt/user/0/emulated/...`、`/mnt/runtime/*/emulated/...`
 * - 更深层路径：`<UUID>/<name>/...`
 * - 卷根本身：`/storage/<UUID>`
 *
 * 注意：native 层 `blockdirs.cpp` 中的 `is_top_level_on_external` 是本实现的镜像
 * （并且额外处理 `mkdirat` 的相对路径 + dirfd），两者必须保持完全一致。
 * 修改任意一侧时请同步另一侧。
 */
object DirGuardPolicy {

    /** 外置可移动存储可能出现的挂载前缀 */
    private const val STORAGE_PREFIX = "/storage/"
    private const val MEDIA_RW_PREFIX = "/mnt/media_rw/"
    private const val SECURE_PREFIX = "/mnt/secure/"
    private const val USER_PREFIX = "/mnt/user/"
    private const val RUNTIME_PREFIX = "/mnt/runtime/"

    /** 内部共享存储，必须放过 */
    private const val EMULATED = "emulated"

    /**
     * 判断 [path] 是否为「外置卷根目录下的第一层目录」。
     *
     * @param path 待判定路径（应为绝对路径），可为 null
     * @return true 表示应当拦截
     */
    fun isTopLevelOnExternal(path: String?): Boolean {
        if (path.isNullOrEmpty()) return false

        // 去掉尾部 '/'：mkdir("/a/b/") 与 mkdir("/a/b") 语义相同
        val normalized = if (path.length > 1) path.trimEnd('/').ifEmpty { "/" } else path

        val base = when {
            normalized.startsWith(STORAGE_PREFIX) -> normalized.substring(STORAGE_PREFIX.length)
            normalized.startsWith(MEDIA_RW_PREFIX) -> normalized.substring(MEDIA_RW_PREFIX.length)
            normalized.startsWith(SECURE_PREFIX) -> normalized.substring(SECURE_PREFIX.length)
            // /mnt/user/<userId>/<UUID>/<name> —— 跳过 userId 这一层
            normalized.startsWith(USER_PREFIX) ->
                normalized.substring(USER_PREFIX.length).substringAfter('/', "")
            // /mnt/runtime/<access>/<UUID>/<name> —— 跳过 access 这一层
            normalized.startsWith(RUNTIME_PREFIX) ->
                normalized.substring(RUNTIME_PREFIX.length).substringAfter('/', "")
            else -> return false
        }

        if (base.isEmpty()) return false

        // 内部存储 /storage/emulated/... 不受影响
        if (base.startsWith(EMULATED)) return false

        // 卷 UUID 之后的第一个 '/'，其后面就是第一层目录名
        val slash = base.indexOf('/')
        if (slash < 0) return false

        val name = base.substring(slash + 1)
        if (name.isEmpty()) return false

        // 更深层不拦
        if (name.indexOf('/') >= 0) return false

        return true
    }
}
