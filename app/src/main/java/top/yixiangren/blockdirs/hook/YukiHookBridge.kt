package top.yixiangren.blockdirs.hook

import io.github.libxposed.api.XposedModule

/**
 * 预留的 **YukiHook** 适配层（目前未启用）。
 *
 * ## 为什么现在不能用 YukiHook
 *
 * 截至本文件编写时（libxposed API 102 已发布），[YukiHookAPI](https://github.com/HighCapable/YukiHookAPI)
 * 的公开版本情况如下：
 *
 * - **稳定版 1.3.2**：基于旧版 rovo89 Xposed API（`de.robv.android.xposed:api:82`），
 *   入口是 `IYukiHookXposedInit`，属于**传统模块**，不是 libxposed 102。
 * - **2.x 开发分支**：原生支持 libxposed，但仓库里只有占位空壳
 *   （`object YukiHook` 无任何实现），且**未发布到 Maven Central / JitPack /
 *   HighCapable 公共仓库**，无法作为依赖解析。
 * - 官方 README 提到的 **1.5.0 公测版**（支持 libxposed）目前只通过 Telegram 群发放，
 *   同样不在任何公开 Maven 仓库里。
 *
 * 因此当前版本先用官方 libxposed API 102 落地（见 [LibxposedHookBridge]），
 * 同时把拦截逻辑抽到 [HookBridge] 抽象层，等 YukiHook 具备可用依赖后可无缝切换。
 *
 * ## 将来如何接入
 *
 * 1. 在 `app/build.gradle` 加入 YukiHook 的 libxposed 支持模块，例如（名称以其正式发布为准）：
 *    ```groovy
 *    implementation 'com.highcapable.yukihook:yukihook-api-libxposed:<version>'
 *    // 并保留 kavaref-core / kavaref-android
 *    ```
 * 2. 让模块入口 `BlockDirsModule` 继承 YukiHook 提供的 libxposed 入口基类
 *    （或按官方文档用其 DSL 重写入口）。
 * 3. 在本类里用 YukiHook 的 Hook DSL 复刻 [LibxposedHookBridge] 的两层拦截
 *    （native 层调用不变，仍走 `BlockDirsNative.installNativeHook()`；
 *    Java 层用 `File::class.resolve()` + YukiHook 的 `method { }.hook { }`）。
 * 4. 把 [HookEngine.bridge] 从 [LibxposedHookBridge] 改为 [YukiHookBridge]。
 *
 * 在此之前，本类保持「不参与构建」的占位状态，不会被调用。
 */
object YukiHookBridge : HookBridge {

    override val id: String = "yukihook-libxposed"

    override fun install(module: XposedModule, target: String, classLoader: ClassLoader?) {
        throw UnsupportedOperationException(
            "YukiHook 尚未发布可用的 libxposed 102 版本；请继续使用 LibxposedHookBridge，" +
                "或先按本文件顶部注释接入 YukiHook 的公测依赖。"
        )
    }
}
