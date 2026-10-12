package top.yixiangren.blockdirs

import android.util.Log
import io.github.libxposed.api.XposedModule
import io.github.libxposed.api.XposedModuleInterface.ModuleLoadedParam
import io.github.libxposed.api.XposedModuleInterface.PackageReadyParam
import io.github.libxposed.api.XposedModuleInterface.SystemServerStartingParam
import top.yixiangren.blockdirs.hook.HookEngine
import top.yixiangren.blockdirs.hook.Targets

/**
 * BlockDirs 的 libxposed API 102 入口。
 *
 * 现代 Xposed API 不再向 zygote 注入，也不会把模块加载进模块自身进程：
 * 框架只会在【作用域内的目标进程】里实例化本类，并回调下面的生命周期方法。
 *
 * 目标进程有两个，因为「卷根第一层目录」不是同一个进程建的：
 *
 * 1. `com.android.providers.media.module`（MediaProvider，UID 10176）
 *    媒体目录（Music / Pictures / DCIM / Movies / Downloads …）由它经 native 层 FUSE
 *    直接调用 libc 的 mkdir / mkdirat 创建 → [onPackageReady] 里装 native hook（+ Java 兜底）。
 * 2. `android`（system_server）
 *    卷挂载 / 准备用户存储的流程会创建 `Android/`（`Android/data`、`Android/obb` 的副产物）
 *    → [onSystemServerStarting] 里同样装 native hook。
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
     * 这里立刻按进程名判断并安装 native hook，避免错过卷目录初始化的第一次 mkdir。
     */
    override fun onModuleLoaded(param: ModuleLoadedParam) {
        val processName = param.processName
        log(Log.INFO, TAG, "libxposed API ${getApiVersion()} loaded in $processName (systemServer=${param.isSystemServer})")
        if (!Targets.isMediaProvider(processName)) return
        HookEngine.bridge.install(this, processName, null)
    }

    /**
     * 包 ClassLoader 就绪、即将创建 Application 时回调。
     * 这里按包名再确认一次（幂等），并在需要时补装 Java 层兜底 Hook。
     */
    override fun onPackageReady(param: PackageReadyParam) {
        val packageName = param.packageName
        if (!Targets.isMediaProvider(packageName)) return
        HookEngine.bridge.install(this, packageName, param.classLoader)
    }

    /**
     * system_server 就绪回调。
     * 只装 native 层：`Android/` 目录通常在卷挂载阶段就被建出来了。
     */
    override fun onSystemServerStarting(param: SystemServerStartingParam) {
        log(Log.INFO, TAG, "libxposed API ${getApiVersion()} loaded in system_server")
        HookEngine.bridge.install(this, Targets.SYSTEM_SERVER, param.classLoader)
    }

    companion object {
        const val TAG = "BlockDirs"
    }
}
