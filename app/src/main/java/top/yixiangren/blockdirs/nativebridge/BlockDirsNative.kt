package top.yixiangren.blockdirs.nativebridge

/**
 * native 层唯一入口（对应 native/src/main/cpp/blockdirs.cpp）。
 *
 * JNI 符号由本类的包名/类名/方法名推导，改名时必须同步修改 C++ 侧的
 * `Java_top_yixiangren_blockdirs_nativebridge_BlockDirsNative_installNativeHook`。
 *
 * 注意：本类只会在目标进程（MediaProvider）里被
 * [top.yixiangren.blockdirs.hook.LibxposedHookBridge] 引用并加载，
 * 模块 App 自身进程不会加载该 native 库。
 */
object BlockDirsNative {

    init {
        System.loadLibrary("blockdirs")
    }

    /**
     * 在目标进程内安装 libc `mkdir` / `mkdirat` 的 inline hook。
     * 由 ShadowHook 完成，内部自带幂等判断。
     */
    external fun installNativeHook()
}
