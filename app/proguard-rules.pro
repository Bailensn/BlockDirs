# BlockDirs R8 规则

# 模块入口保持。YukiHook 的 KSP 会生成
# top.yixiangren.blockdirs.HookEntry_YukiHookXposedInit 写进 META-INF/xposed/java_init.list，
# 框架按类名反射实例化，所以本包下所有类名都不能被混淆。
-keep class top.yixiangren.blockdirs.** { *; }

# 保持 libxposed 入口基类约定：任何继承 XposedModule 的入口都必须有公开无参构造
-keep,allowoptimization public class * extends io.github.libxposed.api.XposedModule {
    public <init>();
}

# 保持 Android 组件类（Activity / alias / receiver 等，manifest 声明不能被混淆）
-keep class * extends android.app.Activity { *; }
-keep class * extends android.app.Service { *; }
-keep class * extends android.content.BroadcastReceiver { *; }

# MainActivityAlias（activity-alias 隐藏桌面图标依赖，组件名不能混淆）
-keep class top.yixiangren.blockdirs.MainActivityAlias { *; }

# 保持 JNI / native 方法（native 库通过 System.loadLibrary 加载，方法名不能混淆）
-keepclasseswithmembernames class * {
    native <methods>;
}

# libxposed API 仅编译期存在（compileOnly），运行时由框架提供
-dontwarn io.github.libxposed.**

# YukiHook / KavaRef 的反射目标是 JDK/Android 类，其 AAR 已自带 keep 规则
-dontwarn com.highcapable.yukihookapi.**
-dontwarn com.highcapable.kavaref.**
-dontwarn kotlin.**

# appcompat / material 已在其 AAR 自带 keep 规则，无需额外处理
# 保留注解（部分反射依赖）
-keepattributes *Annotation*
