# BlockDirs

阻止手机在插入 OTG U 盘 / SD 卡等**外置可移动存储**时，自动在其根目录创建 `Android`、`Music`、`Pictures`、`DCIM`、`Movies` 等标准媒体目录的模块。

- Xposed API：**libxposed API 102**（现代 Xposed API，`io.github.libxposed:api:102.0.0`）
- Hook 框架：**YukiHook 1.5.0**（`com.highcapable.yukihookapi:*`，libxposed 底座）+ **KavaRef** 反射
- 入口与 `META-INF/xposed/*` 元数据：由 `yukihook-compiler`（KSP）在编译期生成，仓库里不再手写

> 已在 ColorOS 16（Android 16，实测 `PJZ110` / ColorOS V16.1.0）上端到端验证通过；理论上基于 AOSP 的系统也可使用。

---

## 背景

Android 的 MediaProvider（`com.android.providers.media.module`，UID `10176`）在检测到外置存储卷首次挂载时，会在卷的根目录初始化一批标准媒体目录（`Music`、`Pictures`、`DCIM`、`Movies` 等）。这些目录的创建并非走 Java 层的 `File.mkdir`，而是由 MediaProvider 通过 **native 层 FUSE** 直接调用 libc 的 `mkdir` / `mkdirat` 完成，因此常规的 Java 层 Hook 无法拦截。

## 原理

入口 `top.yixiangren.blockdirs.HookEntry` 是一个 `YukiHookXposedModule`，用 `@YukiHookLibXposedEntry` 声明作用域，KSP 据此生成
`HookEntry_YukiHookXposedInit` 入口类与 `META-INF/xposed/{java_init.list,module.prop,scope.list}`。
它在**两个**作用域目标进程里分别安装拦截：

| 目标进程 | 干什么 | 模式 |
|---|---|---|
| `com.android.providers.media[.module]`（MediaProvider） | 拦 `Music` / `Pictures` / `DCIM` / `Movies` / `Downloads` … 的创建 | **真拦截**（native 主 + Java 兜底） |
| `system`（system_server 的虚拟包名，**默认不勾**） | 排查用：记录谁在卷根建目录 | **只记日志，绝不改变返回值** |

两层拦截：

1. **native 层（主拦截，ShadowHook inline hook）**
   Hook 目标进程内 libc 的 `mkdir` / `mkdirat`。
   代理函数必须带 `SHADOWHOOK_STACK_SCOPE()`（负责出代理时 pop frame，少这行 hook 会失效）；
   但**调原函数必须用 hook 回填的 `orig` 指针，绝不能用 `SHADOWHOOK_CALL_PREV`** ——
   后者走 `shadowhook_get_prev_func()`，直接解引用每线程 TLS 的 frame 栈：
   `if (0 == stack->frames_cnt)`，`stack` 为 NULL 就是 SIGSEGV。
   我们实测过：MediaProvider 的 `android.bg` 线程拿不到这个栈 → 段错误 → FUSE 守护进程跟着死 →
   内部存储都挂不上。这条注释就是防止以后有人「好心」把它改回 CALL_PREV。
2. **Java 层（兜底，YukiHook Hook + KavaRef 反射，仅 MediaProvider）**
   用 KavaRef 解析 `java.io.File#mkdir` / `#mkdirs`，命中同一判定规则时直接返回成功但不真正建目录。
   在 system_server 里不装这一层（全局 Hook `File.mkdir(s)` 影响面太大）。

**安全设计（重要）**：拦截只发生在 MediaProvider —— 那里判断错了最多是某个媒体目录没出现，
最坏情况也只是 MediaProvider 自己重启，不会牵连开机。system_server 那条路（勾了「系统框架」才会走）
固定以 `MODE_LOG_ONLY` 安装：命中只往 logcat 打一行，返回值永远交给原函数，
**从设计上不可能让开机失败**（也就不会再触发 LSPosed 把全部模块禁用的安全模式）。
另外 native 侧还有熔断：5 秒内拦截超过 512 次就判定「有代码在死循环重试」，立刻放行（fail open），
宁可漏拦也不把进程拖死在 hook 里。

### 热重载（Hot Reload）

入口声明了 `hotReload = HotReload.AUTO`：APK 更新后框架会直接把新代码热重载进正在运行的目标进程
（system_server / MediaProvider），**不用重启手机**。

- **Java 层**：由 YukiHook 在 `onHotReloaded` 里自动恢复（旧 hook handle 卸载，新实例重新 hook）。
- **native 层**：**故意保留不卸载**。libxposed 热重载不会 `dlclose` 模块的 `.so`、也不会调
  `JNI_OnUnload`，所以旧一代装的 libc hook 依然有效；主动 unhook 反而会和正在执行的 hook 线程
  抢 trampoline。新一代实例再次 `installNativeHook()` 会被 C 层的 stub 守卫挡住，不会重复 hook。
- 想改成「只在模块里手动触发」：把注解里的 `HotReload.AUTO` 换成 `HotReload.MANUAL`。

两层都采用**位置判定法**而非「黑名单目录名」：

- 命中的路径形态（同一个卷在不同挂载命名空间下的多种写法）：
  - `/storage/<UUID>/<name>`
  - `/mnt/media_rw/<UUID>/<name>`
  - `/mnt/secure/<...>/<name>`
  - `/mnt/user/<userId>/<UUID>/<name>`
  - `/mnt/runtime/<access>/<UUID>/<name>`
- 排除：内部存储 `emulated`；卷根本身；`<UUID>/<name>/...` 这类更深层路径；
- `mkdirat` 的相对路径（`dirfd` + 名字）会先经 `/proc/self/fd/<dirfd>` 还原成绝对路径再判定；
- 命中后**返回成功（native 返回 0 / Java 返回 true）但不真正创建目录**。

### 设计要点

- **不硬编码目录名，零遗漏**：任何第一层目录都会被拦，不存在「漏掉某个标准目录名」的风险。
- **仅拦截外置可移动存储**：手机内部存储 `/storage/emulated` 完全不受影响。
- **更深层子目录不受影响**：只拦卷根下的第一层，`XXX/Music/...` 这类子路径不拦。
- **进程级隔离**：Hook 只装在 MediaProvider 与 system_server 里，你用自己的文件管理器手动建目录走的是另一个进程，不会被拦。
- **不会死循环**：返回「成功」是终止信号，调用方收到成功不会重试，因此不会出现「反复创建」的现象。

### 已知限制（重要）

- **`Android/` 大概率不是 system_server 建的。** 翻过 AOSP 源码：`StorageManagerService`（system_server 的存储服务）
  在挂载流程里**没有**建外置卷 `Android/` 的代码，只有 `mkdirs(callingPkg, appPath)` 这种「应用请求自己的外置目录」
  才走的分支；`vold` 的 `PublicVolume` 也只 `mkdir("/mnt/secure/asec")`。所以它更可能是 **`installd`
  （native 守护进程）或应用自己**建的 —— 这类进程 Xposed 注入不进去，得从 FUSE / 内核 / magisk 层解决。
- 想确认到底是谁在建：勾上「系统框架」（此时是 log-only，纯记录、不改行为），插卡后看
  ```bash
  adb logcat -c && adb logcat | grep -iE 'BlockDirsNative|BlockDirs'
  ```
  - `BLOCKED mkdir...`：MediaProvider 里被我们拦了；
  - `HOOKED(log-only) mkdir...`：system_server 里有人建（这次只记录，下次可针对性处理）；
  - 一条都没有：说明既不是 MediaProvider 也不是 system_server 建的。

### 副作用（请知悉）

- 外置卷根目录下任何**第一层目录**都会被拦截，包括你手动在卷根下新建的目录（但手动目录由文件管理器进程创建，实际不受影响，此条仅为 hook 语义说明）。

---

## 关于 YukiHook 1.5

本项目基于 YukiHook **1.5.0**（2026-10 发布）。1.5.0 起项目更名为 YukiHook，并拆分为多个模块：

| 模块 | 作用 |
|---|---|
| `yukihook-bom` | 统一版本管理 |
| `yukihook-core` | 核心 Hook API、模块入口、模块状态 |
| `yukihook-runtime-libxposed` | **libxposed 底座**（本模块使用的；另一个底座是 `yukihook-runtime-xposed82`） |
| `yukihook-extension-kavaref` | 与 KavaRef 的衔接（`MemberResolver.hook` 等） |
| `yukihook-compiler` | KSP 代码生成：入口类 + `META-INF/xposed/*` |

关键点：

- 入口用 `@YukiHookLibXposedEntry(targetApiVersion = 102, scope = [...])` 声明，
  KSP 生成 `HookEntry_YukiHookXposedInit` 与 `java_init.list` / `module.prop` / `scope.list`，
  **不要再手写**这些文件（重复会被 KSP 拒绝）。
- 作用域里的 `system` 是 libxposed 代表 system_server 的**虚拟包名**，不是 `android`。
  它**没有**写进 `scope.list`：system_server 属于开机关键路径，勾不勾由用户自己决定（默认只做 MediaProvider）。
- 「检查更新」卡片里的激活状态改用 `YukiHook.Status.observeFrameworkService` + `isModuleActive`，
  不再自己维护 libxposed Service 连接。

---

## 目录结构

```
top.yixiangren.blockdirs/
├── app/                                  # Android 应用（libxposed 入口 + 关于页 UI）
│   └── src/main/
│       ├── java/top/yixiangren/blockdirs/
│       │   ├── HookEntry.kt               # YukiHook 入口（@YukiHookLibXposedEntry，作用域声明）
│       │   ├── MainActivity.kt            # 关于页 UI（激活状态走 YukiHook.Status）
│       │   ├── UpdateChecker.kt           # 检查更新
│       │   ├── AppInfo.kt                 # 仓库地址单一来源
│       │   ├── hook/
│       │   │   ├── BlockDirsHooks.kt      # 各目标进程里安装拦截（native + KavaRef 兜底）
│       │   │   ├── Targets.kt             # 目标进程 / 作用域包名
│       │   │   └── DirGuardPolicy.kt      # 「位置判定法」共享实现
│       │   ├── nativebridge/
│       │   │   └── BlockDirsNative.kt     # native 库加载与 JNI 声明
│       │   └── com/bytedance/shadowhook/
│       │       └── ShadowHook.kt          # shadowhook JNI_OnLoad 防御性空壳类
│       └── res/layout/activity_main.xml
│       （META-INF/xposed/* 由 KSP 生成到 build/，不在源码树里）
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
KSP 2.3.12（YukiHook 代码生成）、compileSdk 37 / targetSdk 36、NDK 30.0.16248370、CMake 4.1.2。
构建脚本全部为 **Kotlin DSL**（`.gradle.kts`），模块代码全部为 **Kotlin**。

> 构建时会由 KSP 生成 `HookEntry_YukiHookXposedInit` 与 `META-INF/xposed/{java_init.list,module.prop,scope.list}`，
> 可在 `app/build/generated/ksp/release/resources/` 下核对。

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
2. 打开 LSPosed（需支持 libxposed 现代 API），启用模块。模块已内置作用域推荐（`scope.list`），**默认只预选下面两项**：
   - **com.android.providers.media.module**（MediaProvider，媒体目录）
   - **com.android.providers.media**（旧版包名，部分机型）
3. **（可选，仅排查用）** 手动勾上 **系统框架**（包名是 **`system`**）：只会把「谁在卷根建目录」打进 logcat，**不会**改变系统行为。
4. 重启手机（或重启 MediaProvider / 软重启使注入生效）。
5. 插入 OTG U 盘 / SD 卡，观察其根目录不再自动生成媒体目录。

> 「系统框架」默认没勾：它只用于排查（log-only，见上文「安全设计」），**不影响拦截效果** ——
> 媒体目录的拦截全部发生在 MediaProvider 那一层。
> 另外若真发生过开机失败，LSPosed 会把所有模块一起禁用、进它自己的安全模式（不是手机的安全模式）；
> 数据与配置都在，重启一次 + 重新启用模块即可恢复，不用刷机。

> **别勾错**：LSPosed 里「系统框架」的包名是 `system`（libxposed 的虚拟包名，代表 system_server）；
> 「Android 系统」的包名是 `android`（framework-res，uid 1000），它的组件跑在单独的 `:ui` 进程，
> **勾 `android` 不会把模块注入 system_server**，`onSystemServerStarting` 也就不会被调用。
>
> libxposed 102 不再向 zygote 注入，作用域按进程精确投递。
> 「系统框架」只装 native 层拦截，不会在 system_server 里全局 Hook `File.mkdir(s)`。
>
> 若之前安装过旧模块 `io.github.rootuser.no_otg_media_folders`，建议先卸载以避免混淆验证。

---

## 作者

[yixiangdebufengren](https://github.com/yixiangdebufengren)
