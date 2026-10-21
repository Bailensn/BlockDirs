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

    init {
        System.loadLibrary("blockdirs")
    }

    @Volatile
    private var installed = false

    /**
     * 幂等安装：同一个进程内重复调用（多个生命周期回调）时只真正装一次。
     *
     * @throws Throwable native 库加载或 ShadowHook 初始化失败时抛出，由调用方记录
     */
    @Synchronized
    fun installOnce() {
        if (installed) return
        installNativeHook()
        installed = true
    }

    /**
     * 在目标进程内安装 libc `mkdir` / `mkdirat` 的 inline hook。
     * 由 ShadowHook 完成。
     */
    private external fun installNativeHook()
}
