package top.yixiangren.blockdirs

import io.github.libxposed.service.XposedService
import io.github.libxposed.service.XposedServiceHelper

/**
 * 模块激活状态检测（libxposed Service 版）。
 *
 * 旧版 rovo89 方案依赖「框架把模块无条件下发到模块自身进程」，再用 Hook 自己的
 * `ModuleActive.isActive()` 来标记激活。libxposed 102 不再向模块自身进程注入
 * 模块代码，因此改为使用官方 Service API：
 *
 * 1. `io.github.libxposed:service` 的 AAR 自带 `XposedProvider`
 *    （authority = `${applicationId}.XposedService`），manifest 合并后由框架回调；
 * 2. 框架通过 `XposedProvider.call("SEND_BINDER", …)` 把服务 Binder 回传给 App；
 * 3. 这里注册监听拿到 [XposedService]，读取其作用域与已注入进程，判断模块是否生效。
 *
 * 该状态完全由「框架是否已连接 + 模块作用域/注入情况」实时决定，不落任何持久化文件。
 */
object ActivationStatus {

    /** 激活状态回调（Kotlin `fun interface`，Kotlin/Java 均可直接用 lambda） */
    fun interface Listener {
        fun onActivationChanged(active: Boolean)
    }

    /** 需要被拦截的目标包名 */
    private val TARGET_PACKAGES = setOf(
        "com.android.providers.media.module",
        "com.android.providers.media"
    )

    private val lock = Any()

    @Volatile
    private var active: Boolean = false

    private var listener: Listener? = null
    private var registered: Boolean = false

    /** 当前缓存的激活状态 */
    val isActive: Boolean get() = active

    /**
     * 注册激活状态监听，注册后会立刻用当前缓存值回调一次。
     *
     * `XposedServiceHelper.registerListener` 只允许调用一次，这里已做去重，
     * 多次调用只会覆盖回调并补发当前状态。
     */
    fun observe(onChanged: Listener) {
        synchronized(lock) {
            listener = onChanged
            if (!registered) {
                registered = true
                XposedServiceHelper.registerListener(object : XposedServiceHelper.OnServiceListener {
                    override fun onServiceBind(service: XposedService) {
                        val scoped = runCatching {
                            service.scope.any { it in TARGET_PACKAGES }
                        }.getOrDefault(false)
                        val running = runCatching {
                            service.runningTargets.isNotEmpty()
                        }.getOrDefault(false)
                        update(scoped || running)
                    }

                    override fun onServiceDied(service: XposedService) {
                        update(false)
                    }
                })
            }
        }
        onChanged.onActivationChanged(active)
    }

    private fun update(value: Boolean) {
        val callback: Listener?
        synchronized(lock) {
            if (active == value) return
            active = value
            callback = listener
        }
        callback?.onActivationChanged(value)
    }
}
