// 顶层构建脚本（Kotlin DSL）
//
// AGP 9.x 起内置 Kotlin 支持（android.builtInKotlin 默认 true），
// 因此不再需要单独 apply "org.jetbrains.kotlin.android"。
// AGP 自带的 KGP 版本较旧，这里通过 buildscript classpath 显式提升到最新稳定版。
buildscript {
    repositories {
        google()
        mavenCentral()
    }
    dependencies {
        classpath("org.jetbrains.kotlin:kotlin-gradle-plugin:2.4.21")
    }
}

plugins {
    id("com.android.application") version "9.4.1" apply false
    id("com.android.library") version "9.4.1" apply false
}
