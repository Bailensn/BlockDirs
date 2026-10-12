package top.yixiangren.blockdirs.hook

import io.github.libxposed.api.XposedModule

/**
 * Hook 适配层（Hook Bridge）。
 *
 * 模块的拦截逻辑只依赖这个接口，不直接依赖任何具体的 Hook 框架。
 * 这样在 YukiHook 发布支持 libxposed 102 的版本后，只需新增/替换一个
 * [HookBridge] 实现并在 [HookEngine] 里切换，业务代码无需改动。
 *
 * 当前实现：[LibxposedHookBridge]（直接使用官方 libxposed API 102）。
 * 预留实现：[YukiHookBridge]（等待 YukiHook 支持 libxposed）。
 */
interface HookBridge {

    /** 桥接实现标识，仅用于日志 */
    val id: String

    /**
     * 在目标进程中安装拦截。
     *
     * 实现必须是幂等的：本方法可能在 [XposedModule.onModuleLoaded] 与
     * [XposedModule.onPackageReady] 中被调用多次。
     *
     * @param module 当前已加载的 libxposed 模块实例，用于注册 Hook 与写日志
     * @param target 目标进程名 / 包名
     * @param classLoader 目标包 ClassLoader；[XposedModule.onModuleLoaded] 阶段可能为 null
     */
    fun install(module: XposedModule, target: String, classLoader: ClassLoader? = null)
}
