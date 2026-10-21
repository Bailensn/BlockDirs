package top.yixiangren.blockdirs.nativebridge

/**
 * native 层唯一入口（对应 native/src/main/cpp/blockdirs.cpp）。
 *
 * JNI 符号由本类的包名/类名/方法名推导，改名时必须同步修改 C++ 侧的
 * `Java_top_yixiangren_blockdirs_nativebridge_BlockDirsNative_installNativeHook`。
 *
 * 注意：本类只会在目标进程（MediaProvider / system_server）里被
 * [top.yixiangren.blockdirs.hook.installBlockDirsHooks] 引用并加载，
 * 模块 App 自身进程不会加载该 native 库。
 */
object BlockDirsNative {

    /**
     * 真拦截：命中规则就返回成功、但不真正创建目录。
     * 只用在 MediaProvider 里 —— 那里判断错了最多是媒体目录没出现，不会影响开机。
     */
    const val MODE_BLOCK = 0

    /**
     * 只记录、绝不改变行为。
     * 用在 system_server 里做**诊断**：谁在卷根建目录会被打进 logcat，
     * 但返回值永远交给原函数，所以这条路径不可能让开机流程失败。
     */
    const val MODE_LOG_ONLY = 1

    init {
        System.loadLibrary("blockdirs")
    }

    @Volatile
    private var installed = false

    /**
     * 幂等安装：同一个进程内重复调用（多个生命周期回调、热重载后的新一代实例）时只装一次。
     *
     * @param mode [MODE_BLOCK] 或 [MODE_LOG_ONLY]
     * @throws Throwable native 库加载或 ShadowHook 初始化失败时抛出，由调用方记录
     */
    @Synchronized
    fun installOnce(mode: Int) {
        if (installed) return
        installNativeHook(mode)
        installed = true
    }

    /**
     * 在目标进程内安装 libc `mkdir` / `mkdirat` 的 inline hook。
     * 由 ShadowHook 完成。
     */
    private external fun installNativeHook(mode: Int)
}
