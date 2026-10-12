package top.yixiangren.blockdirs

import android.util.Log
import io.github.libxposed.api.XposedModule
import io.github.libxposed.api.XposedModuleInterface.ModuleLoadedParam
import io.github.libxposed.api.XposedModuleInterface.PackageReadyParam
import top.yixiangren.blockdirs.hook.HookEngine

/**
 * BlockDirs 的 libxposed API 102 入口。
 *
 * 现代 Xposed API 不再向 zygote 注入，也不会把模块加载进模块自身进程：
 * 框架只会在【作用域内的目标进程】里实例化本类，并回调下面的生命周期方法。
 *
 * 目标进程：`com.android.providers.media.module`（MediaProvider，UID 10176）。
 * 该进程通过 native 层 FUSE 直接调用 libc 的 mkdir / mkdirat 创建外置卷根目录下的
 * 标准媒体目录，Java 层 Hook 拦不到，因此真正的拦截在 native 层完成。
 *
 * 入口声明：`META-INF/xposed/java_init.list`
 * 模块元数据：`META-INF/xposed/module.prop`、`META-INF/xposed/scope.list`
 *
 * @see top.yixiangren.blockdirs.hook.LibxposedHookBridge 当前 Hook 实现
 * @see top.yixiangren.blockdirs.hook.YukiHookBridge 预留的 YukiHook 适配层
 */
class BlockDirsModule : XposedModule() {

    /**
     * 模块代（generation）被加载进目标进程时回调，是最早的时机。
     * 这里立刻按进程名判断并安装 native hook，避免在 MediaProvider 初始化
     * 卷目录之前错过任何一次 mkdir。
     */
    override fun onModuleLoaded(param: ModuleLoadedParam) {
        val processName = param.processName
        log(Log.INFO, TAG, "libxposed API ${getApiVersion()} loaded in $processName (systemServer=${param.isSystemServer})")
        if (!isMediaProvider(processName)) return
        HookEngine.bridge.install(this, processName, null)
    }

    /**
     * 包 ClassLoader 就绪、即将创建 Application 时回调。
     * 这里按包名再确认一次（幂等），并在需要时补装 Java 层兜底 Hook。
     */
    override fun onPackageReady(param: PackageReadyParam) {
        val packageName = param.packageName
        if (!isMediaProvider(packageName)) return
        HookEngine.bridge.install(this, packageName, param.classLoader)
    }

    companion object {

        const val TAG = "BlockDirs"

        /** 各 ROM/Android 版本下 MediaProvider 的两种包名 */
        private val MEDIA_PROVIDER_PACKAGES = setOf(
            "com.android.providers.media.module",
            "com.android.providers.media"
        )

        fun isMediaProvider(name: String): Boolean = name in MEDIA_PROVIDER_PACKAGES
    }
}
