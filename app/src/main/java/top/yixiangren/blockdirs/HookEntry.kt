package top.yixiangren.blockdirs

import android.util.Log
import com.highcapable.yukihookapi.annotation.xposed.YukiHookLibXposedEntry
import com.highcapable.yukihookapi.annotation.xposed.YukiHookLibXposedEntry.HotReload
import com.highcapable.yukihookapi.hook.factory.configure
import com.highcapable.yukihookapi.hook.factory.encase
import com.highcapable.yukihookapi.hook.xposed.YukiHookXposedModule
import com.highcapable.yukihookapi.hook.xposed.bridge.event.registerFrameworkEvents
import com.highcapable.yukihookapi.hook.xposed.bridge.event.v102.onHotReloaded
import com.highcapable.yukihookapi.hook.xposed.bridge.event.v102.onHotReloading
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
 *
 * 热重载：`hotReload = HotReload.AUTO`，APK 更新后框架会直接热重载正在运行的目标进程，
 * 不用重启手机。native 层的 libc hook 会被有意保留（见 `onHotReloading` 的注释），
 * Java 层由 YukiHook 自己在 `onHotReloaded` 里恢复；如需显式控制可改成 `HotReload.MANUAL`。
 *
 * ⚠️ `system` **故意不写进推荐作用域**：在 system_server 里 hook libc 的 mkdir 属于开机关键路径，
 * 只要行为不对（比如返回成功但目录没建、调用方随后又去读写），就可能让某个系统服务异常，
 * 甚至开机失败 —— 那时 LSPosed 会把**所有模块**一起禁用、进它自己的安全模式。
 * 想拦 `Android/` 的用户需要在 LSPosed 里手动勾「系统框架」，这一步故意留给用户自己决定。
 */
@YukiHookLibXposedEntry(
    targetApiVersion = 102,
    scope = [
        Targets.MEDIA_PROVIDER_MODULE,
        Targets.MEDIA_PROVIDER_LEGACY
    ],
    hotReload = HotReload.AUTO
)
object HookEntry : YukiHookXposedModule {

    private const val TAG = "BlockDirs"

    override fun onInit() {
        // 调试日志默认关闭；排查时改成 true 可以看到 YukiHook 的详细装载日志
        configure { debug = false }

        // 框架事件只能在 onInit 里注册，且只能注册一次
        registerFrameworkEvents {
            onHotReloading {
                // 返回 true 表示允许这次热重载。
                // 这里**故意不卸载** native hook：libxposed 热重载不会 dlclose 模块的 .so、
                // 也不会调 JNI_OnUnload，所以旧一代装的 libc hook 仍然有效；而主动 unhook
                // 会和正在执行的 hook 线程抢 trampoline，反而更危险。
                // 新一代实例再次 installNativeHook() 时会被 C 侧的 stub 守卫挡住，不会重复 hook。
                Log.i(TAG, "hot reloading: native libc hook 保留，Java 层由 YukiHook 恢复")
                true
            }
            onHotReloaded {
                Log.i(TAG, "hot reloaded: 新一代实例已接管")
            }
        }
    }

    override fun onHook() = encase {
        loadApp(Targets.MEDIA_PROVIDER_MODULE) { installBlockDirsHooks(inSystemServer = false) }
        loadApp(Targets.MEDIA_PROVIDER_LEGACY) { installBlockDirsHooks(inSystemServer = false) }
        loadSystem { installBlockDirsHooks(inSystemServer = true) }
    }
}
