package top.yixiangren.blockdirs.hook

/**
 * 「位置判定法」的共享实现。
 *
 * 不硬编码任何目录名（Android / Music / Pictures / DCIM / Movies …），
 * 而是按路径结构判断：只要是「外置卷根目录下的第一层目录」，一律拦截。
 * 这样不存在「漏掉某个标准目录名」的风险，也不会影响内部存储和更深层子目录。
 *
 * 生效范围：
 * - 命中：`/storage/<UUID>/<name>`、`/mnt/media_rw/<UUID>/<name>`、`/mnt/secure/<...>/<name>`
 * - 排除：内部存储 `/storage/emulated/...`
 * - 排除：`<UUID>/<name>/...` 这类更深层路径
 *
 * 注意：native 层 `blockdirs.cpp` 中的 `is_top_level_on_external` 是本实现的镜像，
 * 两者必须保持完全一致。修改任意一侧时请同步另一侧。
 */
object DirGuardPolicy {

    /** 外置可移动存储可能出现的挂载前缀 */
    private const val STORAGE_PREFIX = "/storage/"
    private const val MEDIA_RW_PREFIX = "/mnt/media_rw/"
    private const val SECURE_PREFIX = "/mnt/secure/"

    /** 内部共享存储，必须放过 */
    private const val EMULATED = "emulated"

    /**
     * 判断 [path] 是否为「外置卷根目录下的第一层目录」。
     *
     * @param path 待判定路径，可为 null
     * @return true 表示应当拦截
     */
    fun isTopLevelOnExternal(path: String?): Boolean {
        if (path.isNullOrEmpty()) return false

        val base = when {
            path.startsWith(STORAGE_PREFIX) -> path.substring(STORAGE_PREFIX.length)
            path.startsWith(MEDIA_RW_PREFIX) -> path.substring(MEDIA_RW_PREFIX.length)
            path.startsWith(SECURE_PREFIX) -> path.substring(SECURE_PREFIX.length)
            else -> return false
        }

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
