package top.yixiangren.blockdirs

import com.highcapable.yukihookapi.annotation.xposed.YukiHookLibXposedEntry
import com.highcapable.yukihookapi.hook.factory.configure
import com.highcapable.yukihookapi.hook.factory.encase
import com.highcapable.yukihookapi.hook.xposed.YukiHookXposedModule
import top.yixiangren.blockdirs.hook.Targets
import top.yixiangren.blockdirs.hook.installBlockDirsHooks

/**
 * BlockDirs 的 YukiHook 模块入口（libxposed API 102 底座）。
 *
 * 入口类、`META-INF/xposed/java_init.list`、`module.prop`、`scope.list` 全部由
 * `yukihook-compiler`（KSP）在编译期生成，仓库里不再手写这些文件。
 *
 * 作用域说明（来自 libxposed 官方文档）：
 * - `com.android.providers.media[.module]`：MediaProvider，媒体目录由它创建；
 * - `system`：libxposed 用来代表 `system_server` 的**虚拟包名**，
 *   `Android/`（`Android/data`、`Android/obb` 的副产物）在卷挂载阶段由它创建。
 *   注意不是真实包名 `android`（那是「Android 系统」framework-res，组件跑在 `:ui` 进程）。
 */
@YukiHookLibXposedEntry(
    targetApiVersion = 102,
    scope = [
        Targets.MEDIA_PROVIDER_MODULE,
        Targets.MEDIA_PROVIDER_LEGACY,
        Targets.SYSTEM_SERVER
    ]
)
object HookEntry : YukiHookXposedModule {

    override fun onInit() = configure {
        // 调试日志默认关闭；排查时改成 true 可以看到 YukiHook 的详细装载日志
        debug = false
    }

    override fun onHook() = encase {
        loadApp(Targets.MEDIA_PROVIDER_MODULE) { installBlockDirsHooks(inSystemServer = false) }
        loadApp(Targets.MEDIA_PROVIDER_LEGACY) { installBlockDirsHooks(inSystemServer = false) }
        loadSystem { installBlockDirsHooks(inSystemServer = true) }
    }
}
