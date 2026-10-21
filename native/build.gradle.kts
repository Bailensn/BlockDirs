plugins {
    id("com.android.library")
}

android {
    // AGP 9 起 android.uniquePackageNames 默认开启：每个 library 必须有独立 namespace，
    // 因此这里不能与 app 模块共用 top.yixiangren.blockdirs
    // 注意：namespace 会用于生成 R 类包名，不能使用 Java 关键字（如 "native"）
    namespace = "top.yixiangren.blockdirs.nativelib"
    compileSdk = 37
    ndkVersion = "30.0.16248370"

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
