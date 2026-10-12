import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
}

// 自动版本号：versionCode = git 提交数，versionName = "git+<短hash>"
// 形如 1819-git+b22aa570，每次提交自动递增，无需手动维护
fun gitOutput(vararg args: String): String = project.providers.exec {
    commandLine("git", *args)
    workingDirectory = project.projectDir
}.standardOutput.asText.get().trim()

val verCode = gitOutput("rev-list", "--count", "HEAD").toInt()
val verName = "git+" + gitOutput("rev-parse", "--short=8", "HEAD")

android {
    namespace = "top.yixiangren.blockdirs"
    compileSdk = 37

    defaultConfig {
        applicationId = "top.yixiangren.blockdirs"
        minSdk = 30
        targetSdk = 36
        versionCode = verCode
        versionName = verName

        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            // 复用 debug 签名：release 与 debug 可互覆盖安装，无需单独 keystore
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

// AGP 9 内置 Kotlin：直接使用 kotlin {} 配置编译器选项（无需 kotlin-android 插件）
kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    // === 现代 Xposed API：libxposed API 102 ===
    // 入口类继承 io.github.libxposed.api.XposedModule，运行时由框架提供，因此 compileOnly
    compileOnly("io.github.libxposed:api:102.0.0")
    compileOnly("io.github.libxposed:annotation:1.0.0")
    // Service API：模块 App 用来探测框架是否已连接、模块作用域是否包含目标进程
    // （AAR 自带 XposedProvider，manifest 合并后由框架回传服务 Binder）
    implementation("io.github.libxposed:service:102.0.0")

    // === KavaRef：现代 Java 反射 API（YukiHookAPI 1.3.0+ 的官方反射引擎） ===
    implementation("com.highcapable.kavaref:kavaref-core:1.1.1")
    implementation("com.highcapable.kavaref:kavaref-android:1.1.1")
    implementation("com.highcapable.kavaref:kavaref-extension:1.1.1")

    implementation(project(":native"))
    implementation("androidx.appcompat:appcompat:1.8.0")
    implementation("com.google.android.material:material:1.14.0")
}
