package top.yixiangren.blockdirs

import android.content.Context
import android.os.Handler
import android.os.Looper
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL
import java.nio.charset.StandardCharsets
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * 检查 GitHub Releases 是否有新版本。
 *
 * 版本号约定：release 的 tag_name 形如 "92-git+03d914ec"，前缀数字即
 * versionCode（git rev-list --count），与本机 versionCode 直接比较大小。
 */
class UpdateChecker private constructor() {

    /** 检测回调。 */
    fun interface Callback {
        /**
         * @param info 有可用更新时非 null；无更新或失败时为 null
         * @param error 是否因网络/解析失败导致无法判断
         */
        fun onResult(info: UpdateInfo?, error: Boolean)
    }

    class UpdateInfo internal constructor(
        val versionCode: Int,
        val versionName: String,
        val releaseUrl: String,
        val downloadUrl: String
    )

    companion object {

        /** GitHub Releases 最新版 API */
        private const val LATEST_API =
            "https://api.github.com/repos/yixiangdebufengren/top.yixiangren.blockdirs/releases/latest"

        private const val PREFS = "update_check"
        private const val KEY_IGNORED = "ignored_version_code"

        private val executor: ExecutorService = Executors.newSingleThreadExecutor()
        private val mainHandler = Handler(Looper.getMainLooper())

        /** 异步检查更新，忽略已标记「忽略本次」的版本。 */
        fun check(context: Context, currentVersionCode: Int, callback: Callback) {
            executor.execute {
                val info = fetchLatest()
                mainHandler.post {
                    when {
                        // 拉取失败（网络/解析异常），无法判断有无更新
                        info == null -> callback.onResult(null, true)
                        // 无更新（远端 <= 本机），或已被忽略
                        info.versionCode <= currentVersionCode ||
                            isIgnored(context, info.versionCode) -> callback.onResult(null, false)

                        else -> callback.onResult(info, false)
                    }
                }
            }
        }

        /** 拉取 latest release，解析出 versionCode / versionName / url。失败返回 null。 */
        private fun fetchLatest(): UpdateInfo? {
            var conn: HttpURLConnection? = null
            return try {
                conn = (URL(LATEST_API).openConnection() as HttpURLConnection).apply {
                    requestMethod = "GET"
                    connectTimeout = 10_000
                    readTimeout = 10_000
                    setRequestProperty("Accept", "application/vnd.github+json")
                    setRequestProperty("User-Agent", "BlockDirs")
                }

                if (conn.responseCode != 200) return null

                val body = conn.inputStream.use { input ->
                    BufferedReader(InputStreamReader(input, StandardCharsets.UTF_8)).readText()
                }

                val json = JSONObject(body)
                val tag = json.optString("tag_name", "")
                val htmlUrl = json.optString("html_url", "")

                val versionCode = parseVersionCode(tag)
                if (versionCode <= 0) return null

                val downloadUrl = json.optJSONArray("assets")
                    ?.takeIf { it.length() > 0 }
                    ?.optJSONObject(0)
                    ?.optString("browser_download_url", "")
                    .orEmpty()

                UpdateInfo(versionCode, tag, htmlUrl, downloadUrl)
            } catch (_: Exception) {
                null
            } finally {
                conn?.disconnect()
            }
        }

        /** 从 "92-git+03d914ec" 解析出 92。 */
        private fun parseVersionCode(tag: String): Int {
            if (tag.isEmpty()) return 0
            val end = tag.indexOfFirst { !it.isDigit() }.let { if (it < 0) tag.length else it }
            if (end == 0) return 0
            return tag.substring(0, end).toIntOrNull() ?: 0
        }

        /** 是否已忽略该版本。 */
        private fun isIgnored(context: Context, versionCode: Int): Boolean =
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getInt(KEY_IGNORED, -1) == versionCode

        /** 标记「忽略本次」：记住该版本号，之后不再提醒。 */
        fun ignoreVersion(context: Context, versionCode: Int) {
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit()
                .putInt(KEY_IGNORED, versionCode)
                .apply()
        }
    }
}
