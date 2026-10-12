package top.yixiangren.blockdirs.hook

/**
 * Hook 引擎：决定当前使用哪个 [HookBridge] 实现。
 *
 * 切换 YukiHook 时只需改这一行（见 [YukiHookBridge] 顶部注释）：
 * `val bridge: HookBridge = YukiHookBridge`
 */
object HookEngine {

    /** 当前生效的 Hook 适配层 */
    val bridge: HookBridge = LibxposedHookBridge
}
