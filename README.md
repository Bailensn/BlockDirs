# BlockDirs

阻止手机在插入 OTG U 盘 / SD 卡等**外置可移动存储**时，自动在其根目录创建 `Android`、`Music`、`Pictures`、`DCIM`、`Movies` 等标准媒体目录的模块。

- Xposed API：**libxposed API 102**（现代 Xposed API，`io.github.libxposed:api:102.0.0`）
- 反射库：**KavaRef**（`com.highcapable.kavaref:*`，YukiHookAPI 1.3.0+ 官方反射引擎）
- Hook 适配层：内置 `HookBridge` 抽象，已为 **YukiHook** 预留接入位（见下文）

> 已在 ColorOS 16（Android 16，实测 `PJZ110` / ColorOS V16.1.0）上端到端验证通过；理论上基于 AOSP 的系统也可使用。

---

## 背景

Android 的 MediaProvider（`com.android.providers.media.module`，UID `10176`）在检测到外置存储卷首次挂载时，会在卷的根目录初始化一批标准媒体目录（`Android`、`Music`、`Pictures`、`DCIM`、`Movies` 等）。这些目录的创建并非走 Java 层的 `File.mkdir`，而是由 MediaProvider 通过 **native 层 FUSE** 直接调用 libc 的 `mkdir` / `mkdirat` 完成，因此常规的 Java 层 Hook 无法拦截。

## 原理

模块入口 `top.yixiangren.blockdirs.BlockDirsModule` 继承 `io.github.libxposed.api.XposedModule`，只在作用域目标进程（MediaProvider）中被框架实例化，然后安装两层拦截：

1. **native 层（主拦截，ShadowHook inline hook）**
   Hook 目标进程内 libc 的 `mkdir` / `mkdirat` 符号。
2. **Java 层（兜底，libxposed Hook + KavaRef 反射）**
   用 KavaRef 解析 `java.io.File#mkdir` / `#mkdirs`，命中同一判定规则时直接返回成功但不真正建目录。

两层都采用**位置判定法**而非「黑名单目录名」：

- 仅当目录路径满足以下条件时才拦截：
  - 路径前缀为 `/storage/`、`/mnt/media_rw/` 或 `/mnt/secure/`；
  - 且排除内部存储 `emulated`；
  - 且是「卷 UUID 之后仅一层」的目录名；
- 命中后**返回成功（native 返回 0 / Java 返回 true）但不真正创建目录**。

### 设计要点

- **不硬编码目录名，零遗漏**：任何第一层目录都会被拦，不存在「漏掉某个标准目录名」的风险。
- **仅拦截外置可移动存储**：手机内部存储 `/storage/emulated` 完全不受影响。
- **更深层子目录不受影响**：只拦卷根下的第一层，`XXX/Music/...` 这类子路径不拦。
- **进程级隔离**：Hook 只安装在 MediaProvider 进程内，你用自己的文件管理器手动建目录走的是另一个进程，不会被拦。
- **不会死循环**：返回「成功」是终止信号，调用方收到成功不会重试，因此不会出现「反复创建」的现象。

### 副作用（请知悉）

- 外置卷根目录下任何**第一层目录**都会被拦截，包括你手动在卷根下新建的目录（但手动目录由文件管理器进程创建，实际不受影响，此条仅为 hook 语义说明）。

---

## 关于 YukiHookAPI

本项目的 Hook 逻辑通过 `top.yixiangren.blockdirs.hook.HookBridge` 抽象，当前实现为 `LibxposedHookBridge`（官方 libxposed API 102），并为 YukiHook 预留了 `YukiHookBridge`。

**目前无法直接使用 YukiHookAPI 的 libxposed 能力**，原因是：

- YukiHookAPI 稳定版 **1.3.2** 基于旧版 rovo89 Xposed API（`de.robv.android.xposed:api:82`），属于传统模块，不是 libxposed 102；
- 原生支持 libxposed 的 **2.x 分支**在仓库里只有占位空壳（`object YukiHook` 无实现），且**未发布到 Maven Central / JitPack / HighCapable 公共仓库**；
- 官方 README 提到的 **1.5.0 公测版**（支持 libxposed）目前仅通过 Telegram 群发放，同样不在任何公开 Maven 仓库。

因此当前先用官方 libxposed API 102 + KavaRef 落地；等 YukiHook 发布可用的 libxposed 依赖后，只需：
1. 引入其 libxposed 支持模块（`kavaref-core` / `kavaref-android` 继续保留）；
2. 在 `YukiHookBridge` 中按官方 DSL 复刻拦截逻辑；
3. 把 `HookEngine.bridge` 从 `LibxposedHookBridge` 改为 `YukiHookBridge`。

详细步骤见 [`YukiHookBridge.kt`](app/src/main/java/top/yixiangren/blockdirs/hook/YukiHookBridge.kt) 顶部注释。

---

## 目录结构

```
top.yixiangren.blockdirs/
├── app/                                  # Android 应用（libxposed 入口 + 关于页 UI）
│   └── src/main/
│       ├── java/top/yixiangren/blockdirs/
│       │   ├── BlockDirsModule.kt         # libxposed API 102 入口（XposedModule）
│       │   ├── ActivationStatus.kt        # 激活状态检测（libxposed Service API）
│       │   ├── MainActivity.java          # 关于页 UI
│       │   ├── UpdateChecker.java         # 检查更新
│       │   ├── hook/
│       │   │   ├── HookBridge.kt          # Hook 适配层抽象（为 YukiHook 预留）
│       │   │   ├── LibxposedHookBridge.kt # libxposed 102 实现（native + KavaRef 兜底）
│       │   │   ├── YukiHookBridge.kt      # YukiHook 适配层占位
│       │   │   ├── HookEngine.kt          # 桥接实现选择
│       │   │   └── DirGuardPolicy.kt      # 「位置判定法」共享实现
│       │   ├── nativebridge/
│       │   │   └── BlockDirsNative.kt     # native 库加载与 JNI 声明
│       │   └── com/bytedance/shadowhook/
│       │       └── ShadowHook.kt          # shadowhook JNI_OnLoad 防御性空壳类
│       ├── resources/META-INF/xposed/     # 现代 Xposed 模块元数据
│       │   ├── module.prop                # minApiVersion/targetApiVersion=102
│       │   ├── java_init.list             # 入口类声明
│       │   └── scope.list                 # 推荐作用域
│       └── res/layout/activity_main.xml
├── native/                                # native hook 实现（ShadowHook + libc mkdir/mkdirat）
│   ├── build.gradle.kts
│   └── src/main/cpp/blockdirs.cpp
├── app/build.gradle.kts                   # Kotlin DSL 构建脚本
├── build.gradle.kts
├── settings.gradle.kts
└── ...
```

---

## 构建

### GitHub Actions

仓库已配置 `build.yml`，push 后会自动构建 `arm64-v8a` 的 Release APK（release 复用 debug 签名，便于覆盖安装）。

构建产物会自动发布为 GitHub Release，tag 形如 `92-git+03d914ec`（= `git rev-list --count` + 短 hash），
与 App 内「检查更新」的版本号约定一致。

工作流分三个任务，**Release 只在构建产物校验通过之后才会创建**：

1. `build`：构建 APK → 校验（存在 / 非空 / 合法 zip）→ 归档为 workflow artifact，本身不碰 Release；
2. `release`：`needs: build` 且 `apk_ok == true` 才执行，下载 artifact 再校验一次，之后才清理旧的 tag/release 并发布；
3. `publish-mirror`（可选）：`needs: [build, release]`，从刚发布的 Release 下载 APK 同步到 LSPosed 模块仓库。

因此构建失败、产物缺失或产出坏文件时，既不会创建新 Release，也不会先把已有 Release 删掉。

> **Fork 用户注意**：GitHub 会默认关闭 fork 仓库的 Actions。首次需要到 `Actions` 页面点一次
> 「I understand my workflows, go ahead and enable them」，否则 push 后不会产生任何构建（运行记录恒为 0）。

可选：把构建产物同步到 LSPosed 官方模块仓库（`Xposed-Modules-Repo/<包名>`）。默认**跳过**，
配置好下面两项后才会启用（fork / 个人构建不会因此变红）：
- 仓库**变量** `MIRROR_REPO`（Settings → Secrets and variables → Actions → Variables），例如 `Xposed-Modules-Repo/top.yixiangren.blockdirs`
- 仓库**密钥** `MIRROR_TOKEN`（同页 Secrets），需为对该镜像仓库有写权限的 PAT

工作流中的 Action 均为最新大版本，全部运行在 **node24** 运行时：
`actions/checkout@v7`、`actions/setup-java@v6`、`android-actions/setup-android@v4`、
`gradle/actions/setup-gradle@v6`、`softprops/action-gh-release@v3`。
（若使用自托管 Runner，需 Runner 版本 ≥ 2.327.1。）

### 本地构建

```bash
./gradlew assembleRelease
# 产物：app/build/outputs/apk/release/app-release.apk
```

> 若提示 `Permission denied`，先执行 `chmod +x gradlew`（部分文件系统 / 解压拷贝会丢失可执行位）。
> 也可直接使用系统 Gradle：`gradle assembleRelease`。

环境（全部为当前最新稳定版）：AGP 9.4.1、Gradle 9.8.1、JDK 21、Kotlin 2.4.21（AGP 9 内置 Kotlin，不再单独 apply `kotlin-android`）、
compileSdk/targetSdk 36、NDK 30.0.16248370、CMake 4.1.2。构建脚本全部为 **Kotlin DSL**（`.gradle.kts`），模块代码全部为 **Kotlin**。

> AGP 9 起 `android.newDsl` / `android.builtInKotlin` / `android.uniquePackageNames` 默认开启：
> 无需再应用 `org.jetbrains.kotlin.android`，且每个 library 必须有独立 `namespace`（本项目的 `:native` 使用 `top.yixiangren.blockdirs.nativelib`）。

构建产物 APK 根目录应包含：
```
META-INF/xposed/module.prop
META-INF/xposed/java_init.list
META-INF/xposed/scope.list
```

---

## 安装与启用

1. 安装构建出的 APK。
2. 打开 LSPosed（需支持 libxposed 现代 API），启用模块。模块已内置作用域推荐（`scope.list`，自动预选 MediaProvider），作用域选择：
   - **com.android.providers.media.module**（MediaProvider）
   - **com.android.providers.media**（旧版包名，部分机型）
3. 重启手机（或重启 MediaProvider / 软重启使注入生效）。
4. 插入 OTG U 盘 / SD 卡，观察其根目录不再自动生成媒体目录。

> libxposed 102 不再向 zygote 注入，作用域按进程精确投递；无需再勾选「系统框架」。
>
> 若之前安装过旧模块 `io.github.rootuser.no_otg_media_folders`，建议先卸载以避免混淆验证。

---

## 作者

[yixiangdebufengren](https://github.com/yixiangdebufengren)
