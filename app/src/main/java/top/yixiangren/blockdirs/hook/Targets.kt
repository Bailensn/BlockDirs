package top.yixiangren.blockdirs.hook

/**
 * 模块会安装 Hook 的目标进程。
 *
 * 目录创建可能来自两个地方，两个都要覆盖：
 * - **MediaProvider**（`com.android.providers.media[.module]`）：媒体目录走 FUSE → libc，
 *   是 native 层的主战场，同时也装 Java 层兜底；
 * - **system_server**（`android`）：卷挂载/准备用户存储时创建 `Android/`（`Android/data`、`Android/obb` 的副产物）
 *   就在这里，只装 native 层（避免在 system_server 里全局 Hook `File.mkdir(s)`）。
 *
 * 注：如果是 `vold` / `installd` 这类 **native 守护进程**建的目录，Xposed/libxposed 注入不进去，
 * 任何模块都拦不到（详见 README「已知限制」）。
 */
object Targets {

    /** libxposed / LSPosed 里「系统框架」对应的包名 */
    const val SYSTEM_SERVER = "android"

    /** 各 ROM / Android 版本下 MediaProvider 的两种包名 */
    private val MEDIA_PROVIDERS = setOf(
        "com.android.providers.media.module",
        "com.android.providers.media"
    )

    fun isMediaProvider(name: String): Boolean = name in MEDIA_PROVIDERS
}
