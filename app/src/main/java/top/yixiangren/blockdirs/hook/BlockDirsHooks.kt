package top.yixiangren.blockdirs.hook

import android.util.Log
import com.highcapable.kavaref.KavaRef.Companion.resolve
import com.highcapable.yukihookapi.hook.param.PackageParam
import top.yixiangren.blockdirs.nativebridge.BlockDirsNative
import java.io.File

private const val TAG = "BlockDirs"

/**
 * native 层总开关（编译期硬开关）。
 *
 * 为什么现在是 false：native 层（改 libc 函数入口）在真机上连着炸了三次 ——
 * shadowhook 的 TLS 两次、自研跳板一次 —— 每次都把 MediaProvider（FUSE 守护进程）打崩，
 * 连带内部存储挂不上、系统几乎软砖。
 *
 * 在能**安全验证**之前，native 层一律不安装：模块只剩 Java 层兜底，
 * 从设计上不可能再影响内部存储或开机。要重新启用必须改这个常量并重新构建。
 */
private const val NATIVE_HOOK_ENABLED = false

/**
 * 在目标进程里安装拦截。
 *
 * 分两层：
 * 1. **native 层（当前编译期禁用）**：改 libc `mkdir` / `mkdirat` 入口跳到代理函数。
 *    它是唯一能拦 FUSE → libc 那条路径的手段，但真机上炸了三次（见 [NATIVE_HOOK_ENABLED]），
 *    在能安全验证之前不启用。
 * 2. **Java 层（当前唯一生效的一层，仅 MediaProvider）**：用 KavaRef 解析
 *    `java.io.File#mkdir` / `#mkdirs`，命中同一套「位置判定法」时直接返回成功但不真正建目录。
 *    system_server 里不装这一层，避免全局 Hook `File.mkdir(s)` 影响面过大。
 *
 * @param inSystemServer 当前是否处于 system_server（由 `loadSystem` 装载）
 */
fun PackageParam.installBlockDirsHooks(inSystemServer: Boolean = false) {
    installNativeHook(inSystemServer)
    if (!inSystemServer) installJavaGuard()
}

/** native 层：libc mkdir / mkdirat（幂等） */
private fun PackageParam.installNativeHook(inSystemServer: Boolean) {
    if (!NATIVE_HOOK_ENABLED) {
        // 默认路径：一行 native 代码都不加载，绝不碰 libc
        Log.i(TAG, "native 层已禁用（编译期开关），本进程只装 Java 层兜底")
        return
    }
    // system_server 里只做诊断（log-only），绝不改变返回值 —— 开机关键路径不能碰
    val mode = if (inSystemServer) BlockDirsNative.MODE_LOG_ONLY else BlockDirsNative.MODE_BLOCK
    try {
        BlockDirsNative.installOnce(mode)
        val where = if (inSystemServer) "system_server(log-only)" else packageName
        Log.i(TAG, "native libc mkdir/mkdirat hook installed in $where")
    } catch (t: Throwable) {
        Log.e(TAG, "native hook install failed in $packageName", t)
    }
}

/** Java 层兜底：java.io.File#mkdir / #mkdirs */
private fun PackageParam.installJavaGuard() {
    for (methodName in listOf("mkdir", "mkdirs")) {
        val resolver = File::class.resolve()
            .firstMethodOrNull {
                name = methodName
                emptyParameters()
            } ?: continue

        resolver.self.hook {
            intercept {
                val path = instanceOrNull<File>()?.absolutePath
                if (DirGuardPolicy.isTopLevelOnExternal(path)) {
                    Log.w(TAG, "blocked File.$methodName -> $path")
                    // mkdir / mkdirs 返回 boolean：返回 true 假装成功，调用方收到成功即停止重试
                    true
                } else {
                    callOriginal()
                }
            }
        }
    }
}
