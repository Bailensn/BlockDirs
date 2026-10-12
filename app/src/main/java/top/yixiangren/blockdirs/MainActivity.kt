package top.yixiangren.blockdirs

import android.app.Activity
import android.content.ComponentName
import android.content.Intent
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.util.TypedValue
import android.view.View
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.core.graphics.ColorUtils
import com.google.android.material.color.DynamicColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.snackbar.Snackbar

/**
 * BlockDirs 模块主页（视觉复刻 WeKit 风格）。
 *
 * 浅绿主题 + Material 卡片布局 + 居中标题栏。
 * 顶部卡片点击跳转到博客说明页。
 */
class MainActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Material You 动态取色：跟随系统壁纸取色，新设备生效，旧设备自动降级
        DynamicColors.applyToActivitiesIfAvailable(application)
        setContentView(R.layout.activity_main)
        applyStatusBar()

        bindVersion()
        bindLinks()
        bindHideIconSwitch()
        bindActivationStatus()
        bindUpdateCheck()
        checkUpdate()
    }

    /**
     * 让状态栏背景跟随动态取色的 colorSurface，并把状态栏图标/文字设为深色。
     * 避免 Material3 默认把状态栏染成偏深蓝/深色的 colorPrimary 造成的突兀感。
     */
    private fun applyStatusBar() {
        // 取动态取色后的 surface 色作为状态栏背景（与页面背景一致）
        // colorSurface 是 Material Components 定义的 attr，不是框架 android.R.attr
        val surface = resolveThemeColor(com.google.android.material.R.attr.colorSurface)
        window.statusBarColor = surface
        // 浅色背景 -> 深色前景图标，深色背景 -> 浅色前景图标
        val isLight = ColorUtils.calculateLuminance(surface) > 0.5
        var flags = window.decorView.systemUiVisibility
        flags = if (isLight) {
            flags or View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR
        } else {
            flags and View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR.inv()
        }
        window.decorView.systemUiVisibility = flags
    }

    private fun resolveThemeColor(attr: Int): Int {
        val tv = TypedValue()
        return if (theme.resolveAttribute(attr, tv, true)) tv.data else Color.WHITE
    }

    /**
     * 绑定「模块激活状态」卡片。
     *
     * 通过 libxposed Service API（[ActivationStatus]）检测：
     * 框架是否已连接、模块作用域是否包含 MediaProvider、是否有进程已被注入。
     * 状态由框架实时决定，不依赖 zygote 注入，也不落任何持久化文件。
     */
    private fun bindActivationStatus() {
        ActivationStatus.observe { active -> runOnUiThread { renderActivationStatus(active) } }
    }

    private fun renderActivationStatus(active: Boolean) {
        val card = findViewById<LinearLayout>(R.id.card_status) ?: return
        val title = findViewById<TextView>(R.id.status_title) ?: return
        val desc = findViewById<TextView>(R.id.status_desc) ?: return
        val icon = findViewById<ImageView>(R.id.status_icon) ?: return

        if (active) {
            card.setBackgroundResource(R.drawable.card_status_green)
            icon.setImageResource(R.drawable.ic_status_ok)
            title.setText(R.string.module_status_title_active)
            desc.setText(R.string.module_status_desc_active)
        } else {
            card.setBackgroundResource(R.drawable.card_status_red)
            icon.setImageResource(R.drawable.ic_status_warn)
            title.setText(R.string.module_status_title_inactive)
            desc.setText(R.string.module_status_desc_inactive)
        }
    }

    /** 绑定「隐藏桌面图标」开关：切换 activity-alias 的 enabled 状态 */
    private fun bindHideIconSwitch() {
        val sw = findViewById<MaterialSwitch>(R.id.switch_hide_icon) ?: return
        sw.isChecked = isIconHidden()
        sw.setOnCheckedChangeListener { _, isChecked -> setIconHidden(isChecked) }
    }

    private fun isIconHidden(): Boolean {
        val alias = ComponentName(this, "$packageName.MainActivityAlias")
        return when (packageManager.getComponentEnabledSetting(alias)) {
            PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
            PackageManager.COMPONENT_ENABLED_STATE_DISABLED_USER,
            PackageManager.COMPONENT_ENABLED_STATE_DISABLED_UNTIL_USED -> true
            else -> false
        }
    }

    private fun setIconHidden(hidden: Boolean) {
        val alias = ComponentName(this, "$packageName.MainActivityAlias")
        val newState = if (hidden) {
            PackageManager.COMPONENT_ENABLED_STATE_DISABLED
        } else {
            PackageManager.COMPONENT_ENABLED_STATE_ENABLED
        }
        packageManager.setComponentEnabledSetting(alias, newState, PackageManager.DONT_KILL_APP)
    }

    private fun bindVersion() {
        val footer = findViewById<TextView>(R.id.version_footer) ?: return
        val version = getVersionName()
        footer.text = getString(R.string.version_footer).replace("{version}", version)
    }

    private fun bindLinks() {
        val repoUrl = AppInfo.REPO_URL
        val cards = intArrayOf(R.id.card_guide, R.id.card_github, R.id.card_blog)
        val urls = arrayOf("https://yixiangren.top/463187456", repoUrl, "https://yixiangren.top")
        for (i in cards.indices) {
            val url = urls[i]
            findViewById<View>(cards[i])?.setOnClickListener { openUrl(url) }
        }
        // 仓库地址只在 AppInfo 一处维护，这里同步 Github 卡片的副标题
        findViewById<TextView>(R.id.link_github_subtitle)?.text = AppInfo.REPO_SLUG
    }

    private fun bindUpdateCheck() {
        findViewById<View>(R.id.card_update)?.setOnClickListener { manualCheckUpdate() }
    }

    private fun openUrl(url: String) {
        try {
            startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        } catch (_: Exception) {
        }
    }

    /** 启动时异步检查更新，有新版则弹窗；无更新或失败则静默。 */
    private fun checkUpdate() {
        UpdateChecker.check(this, getVersionCode()) { info, _ ->
            if (info != null) showUpdateDialog(info)
        }
    }

    /** 手动检查更新：弹「正在检查」进度，完成后提示最新或弹更新弹窗。 */
    private fun manualCheckUpdate() {
        val progress: AlertDialog = MaterialAlertDialogBuilder(this)
            .setMessage(R.string.update_checking)
            .setCancelable(false)
            .create()
        progress.show()

        UpdateChecker.check(this, getVersionCode()) { info, error ->
            progress.dismiss()
            when {
                info != null -> showUpdateDialog(info)
                error -> snack(R.string.update_check_failed)
                else -> snack(R.string.update_dialog_no_update)
            }
        }
    }

    /** 更新弹窗：立即更新 / 忽略本次 / 稍后再说。 */
    private fun showUpdateDialog(info: UpdateChecker.UpdateInfo) {
        val message = getString(
            R.string.update_dialog_message,
            getVersionCode(), info.versionCode, info.versionName
        )

        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.update_dialog_title)
            .setMessage(message)
            .setPositiveButton(R.string.update_dialog_now) { _, _ ->
                val url = info.releaseUrl.ifEmpty { info.downloadUrl }
                openUrl(url)
            }
            .setNeutralButton(R.string.update_dialog_ignore) { _, _ ->
                UpdateChecker.ignoreVersion(this, info.versionCode)
            }
            .setNegativeButton(R.string.update_dialog_later, null)
            .show()
    }

    /** Material 风格短提示（Snackbar）。 */
    private fun snack(resId: Int) {
        findViewById<View>(android.R.id.content)?.let {
            Snackbar.make(it, resId, Snackbar.LENGTH_SHORT).show()
        }
    }

    private fun getVersionName(): String = runCatching {
        val pi: PackageInfo = packageManager.getPackageInfo(packageName, 0)
        pi.versionName ?: "1.0"
    }.getOrDefault("1.0")

    private fun getVersionCode(): Int = runCatching {
        packageManager.getPackageInfo(packageName, 0).longVersionCode.toInt()
    }.getOrDefault(1)
}
