pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        // HighCapable 公共仓库（KavaRef / YukiHook 系列），作为 Maven Central 的备用镜像
        // 中国大陆用户可把 raw.githubusercontent.com 换成 raw.gitmirror.com
        maven("https://raw.githubusercontent.com/HighCapable/maven-repository/main/repository/releases")
        // 保留给旧版 rovo89 Xposed API（当前 libxposed 102 并不需要，仅为兼容旧依赖留位）
        maven("https://api.xposed.info/")
    }
}

rootProject.name = "top.yixiangren.blockdirs"
include(":app", ":native")
