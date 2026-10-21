import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    // YukiHook 的代码生成器（KSP）会产出 Xposed 入口类与 META-INF/xposed/* 元数据
    id("com.google.devtools.ksp")
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
    compileSdk = 36

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
    // === YukiHook 1.5（libxposed 底座） ===
    // 1.5.0 起拆分为多个模块并统一版本发布，用 BOM 统一管理版本
    implementation(platform("com.highcapable.yukihookapi:yukihook-bom:1.5.0"))
    implementation("com.highcapable.yukihookapi:yukihook-core")
    // libxposed API 底座
    implementation("com.highcapable.yukihookapi:yukihook-runtime-libxposed")
    // KavaRef 与 YukiHook 的衔接（MemberResolver.hook 等），KavaRef 本身需单独引入
    implementation("com.highcapable.yukihookapi:yukihook-extension-kavaref")
    // KSP 代码生成器：产出入口类 + META-INF/xposed/{java_init.list,module.prop,scope.list}
    ksp("com.highcapable.yukihookapi:yukihook-compiler")

    // === libxposed API 102：仅编译期需要，运行时由框架提供 ===
    compileOnly("io.github.libxposed:api:102.0.0")
    compileOnly("io.github.libxposed:annotation:1.0.0")

    // === KavaRef：现代 Java 反射 API ===
    implementation(platform("com.highcapable.kavaref:kavaref-bom:1.1.1"))
    implementation("com.highcapable.kavaref:kavaref-core")
    implementation("com.highcapable.kavaref:kavaref-android")
    implementation("com.highcapable.kavaref:kavaref-extension")

    implementation(project(":native"))
    implementation("androidx.appcompat:appcompat:1.8.0")
    implementation("com.google.android.material:material:1.14.0")
}
