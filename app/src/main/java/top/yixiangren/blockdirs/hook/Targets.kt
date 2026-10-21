package top.yixiangren.blockdirs.hook

/**
 * 模块会安装 Hook 的目标进程（也是 YukiHook 入口声明的推荐作用域）。
 *
 * 「卷根第一层目录」不是同一个进程建的，所以两边都要覆盖：
 * - **MediaProvider**（`com.android.providers.media[.module]`）：媒体目录走 FUSE → libc；
 * - **system_server**：卷挂载 / 准备用户存储时创建 `Android/`（`Android/data`、`Android/obb`
 *   的副产物）。
 *
 * 注：如果是 `vold` / `installd` 这类 **native 守护进程**建的目录，Xposed/libxposed 注入不进去，
 * 任何模块都拦不到（详见 README「已知限制」）。
 */
object Targets {

    const val MEDIA_PROVIDER_MODULE = "com.android.providers.media.module"

    /** 旧版 / 部分机型的 MediaProvider 包名 */
    const val MEDIA_PROVIDER_LEGACY = "com.android.providers.media"

    /**
     * libxposed 用**虚拟包名** `system` 代表 `system_server`，必须显式写进作用域
     * 才会被投递进去（见 `XposedModuleInterface` 的 package-info）。
     *
     * 注意别和真实包名 `android` 搞混：LSPosed 里 `android` 显示为「Android 系统」，
     * 那是 framework-res 包，它的组件跑在单独的 `:ui` 进程，勾它**不会**进入 system_server。
     */
    const val SYSTEM_SERVER = "system"
}
