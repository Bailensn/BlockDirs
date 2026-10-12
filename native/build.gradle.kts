plugins {
    id("com.android.library")
}

android {
    // AGP 9 起 android.uniquePackageNames 默认开启：每个 library 必须有独立 namespace，
    // 因此这里不能与 app 模块共用 top.yixiangren.blockdirs
    // 注意：namespace 会用于生成 R 类包名，不能使用 Java 关键字（如 "native"）
    namespace = "top.yixiangren.blockdirs.nativelib"
    compileSdk = 36
    // 用 NDK 27.2.12479018（本项目实测通过的版本）。
    // shadowhook 的 common/bytesig.h 用了一套很脆的变参宏（MACRO_CHOOSER / NO_ARG_EXPANDER）
    // 实现崩溃信号保护，在更新的 clang（NDK 30 = clang 21+）上会直接编译失败；
    // 上游最后一次提交是 2026-08，暂时没有可用修复，所以这里先不追新 NDK。
    ndkVersion = "27.2.12479018"

    defaultConfig {
        minSdk = 30

        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
