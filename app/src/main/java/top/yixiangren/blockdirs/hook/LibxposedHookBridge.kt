package top.yixiangren.blockdirs.hook

import android.util.Log
import com.highcapable.kavaref.KavaRef.Companion.resolve
import io.github.libxposed.api.XposedInterface
import io.github.libxposed.api.XposedModule
import top.yixiangren.blockdirs.nativebridge.BlockDirsNative
import java.io.File
import java.lang.reflect.Method

/**
 * 基于官方 libxposed API 102 的 Hook 适配层实现。
 *
 * 分两层拦截，互为补充：
 *
 * 1. **native 层（主）**：加载 `libblockdirs.so`，用 ShadowHook inline hook 目标进程
 *    libc 的 `mkdir` / `mkdirat`。MediaProvider 走的就是 FUSE -> libc 这条路径，
 *    这是唯一可靠、零遗漏的拦截点。
 *
 * 2. **Java 层（兜底）**：用 KavaRef 反射解析 `java.io.File#mkdir` / `#mkdirs` 并用
 *    libxposed 挂 Hook，命中同一套「位置判定法」时直接返回成功但不真正建目录。
 *    部分 ROM 的 MediaProvider 也会走 Java 层建目录，这一层用来补齐。
 *
 * 所有 Hook 都只安装在目标进程（MediaProvider）内，不影响其它进程。
 */
object LibxposedHookBridge : HookBridge {

    private const val TAG = "BlockDirs"

    override val id: String = "libxposed-102"

    @Volatile
    private var nativeInstalled = false

    @Volatile
    private var javaGuardInstalled = false

    private val installLock = Any()

    override fun install(module: XposedModule, target: String, classLoader: ClassLoader?) {
        installNativeHook(module, target)
        installJavaGuard(module, target)
    }

    /** native 层：libc mkdir / mkdirat（幂等） */
    private fun installNativeHook(module: XposedModule, target: String) {
        if (nativeInstalled) return
        synchronized(installLock) {
            if (nativeInstalled) return
            try {
                BlockDirsNative.installNativeHook()
                nativeInstalled = true
                module.log(Log.INFO, TAG, "[$id] native libc mkdir/mkdirat hook installed in $target")
            } catch (t: Throwable) {
                // 安装失败时不置位，下次生命周期回调会重试
                module.log(Log.ERROR, TAG, "[$id] native hook install failed in $target: $t")
            }
        }
    }

    /** Java 层兜底：java.io.File#mkdir / #mkdirs（幂等） */
    private fun installJavaGuard(module: XposedModule, target: String) {
        if (javaGuardInstalled) return
        synchronized(installLock) {
            if (javaGuardInstalled) return
            try {
                val installed = hookFileMethod(module, "mkdir") + hookFileMethod(module, "mkdirs")
                if (installed > 0) javaGuardInstalled = true
                module.log(Log.INFO, TAG, "[$id] java layer guard hooks installed ($installed) in $target")
            } catch (t: Throwable) {
                // Java 层只是兜底，失败不影响 native 层拦截
                module.log(Log.ERROR, TAG, "[$id] java layer guard install failed in $target: $t")
            }
        }
    }

    /**
     * 用 KavaRef 反射解析 `java.io.File` 上指定名字的无参方法，并交给 libxposed 挂 Hook。
     *
     * @return 成功挂上的数量（0 或 1）
     */
    private fun hookFileMethod(module: XposedModule, methodName: String): Int {
        val method: Method = File::class.resolve()
            .firstMethodOrNull {
                name = methodName
                emptyParameters()
            }
            ?.self ?: return 0

        module.hook(method)
            // 保底：Hook 内部抛异常时不连累宿主进程
            .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
            .setId("blockdirs-file-$methodName")
            .intercept { chain ->
                val path = (chain.thisObject as? File)?.path
                if (DirGuardPolicy.isTopLevelOnExternal(path)) {
                    module.log(Log.WARN, TAG, "[$id] blocked File.$methodName -> $path")
                    // mkdir / mkdirs 返回 boolean：返回 true 假装成功，调用方收到成功即停止重试
                    true
                } else {
                    chain.proceed()
                }
            }
        return 1
    }
}
