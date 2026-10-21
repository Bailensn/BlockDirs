package top.yixiangren.blockdirs.hook

import android.util.Log
import com.highcapable.kavaref.KavaRef.Companion.resolve
import com.highcapable.yukihookapi.hook.param.PackageParam
import top.yixiangren.blockdirs.nativebridge.BlockDirsNative
import java.io.File

private const val TAG = "BlockDirs"

/**
 * 在目标进程里安装拦截。
 *
 * 分两层，互为补充：
 * 1. **native 层（主）**：加载 `libblockdirs.so`，用 ShadowHook inline hook libc 的
 *    `mkdir` / `mkdirat`。MediaProvider 走的就是 FUSE → libc 这条路径，这是唯一可靠、
 *    零遗漏的拦截点；`system_server` 里也都靠这一层。
 * 2. **Java 层（兜底，仅 MediaProvider）**：用 KavaRef 解析 `java.io.File#mkdir` / `#mkdirs`，
 *    命中同一套「位置判定法」时直接返回成功但不真正建目录。system_server 里不装这一层，
 *    避免全局 Hook `File.mkdir(s)` 影响面过大。
 *
 * @param inSystemServer 当前是否处于 system_server（由 `loadSystem` 装载）
 */
fun PackageParam.installBlockDirsHooks(inSystemServer: Boolean = false) {
    installNativeHook(inSystemServer)
    if (!inSystemServer) installJavaGuard()
}

/** native 层：libc mkdir / mkdirat（幂等） */
private fun PackageParam.installNativeHook(inSystemServer: Boolean) {
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
