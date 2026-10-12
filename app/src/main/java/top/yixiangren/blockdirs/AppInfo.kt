package top.yixiangren.blockdirs

/**
 * 仓库信息单一来源。
 *
 * 「检查更新」和 UI 里的 GitHub 卡片都从这里取地址，换仓库时只改这一个文件。
 * 注意：这里的仓库必须与 `.github/workflows/build.yml` 发布 Release 的仓库一致
 * （CI 里用的是 `${{ github.repository }}`，也就是当前仓库）。
 */
object AppInfo {

    const val REPO_OWNER = "Bailensn"
    const val REPO_NAME = "BlockDirs"

    /** 形如 `owner/repo`，用于 GitHub API */
    const val REPO_SLUG = "$REPO_OWNER/$REPO_NAME"

    /** 形如 `https://github.com/owner/repo` */
    const val REPO_URL = "https://github.com/$REPO_SLUG"

    /** 最新 Release 查询接口（版本号约定：tag 前缀数字 = versionCode） */
    const val LATEST_RELEASE_API = "https://api.github.com/repos/$REPO_SLUG/releases/latest"
}
