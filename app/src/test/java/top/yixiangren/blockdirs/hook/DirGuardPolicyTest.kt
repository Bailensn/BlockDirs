package top.yixiangren.blockdirs.hook

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * DirGuardPolicy（Java 层兜底用的「位置判定法」）的 JVM 单测。
 *
 * 用例与 native 侧 `native/src/test/hook_engine_test.cpp` 里的 guard_policy 断言
 * **保持同一张表**：两边判定不一致时，这里或那边一定会红。
 *
 * 注意一处**有意的差异**：native 的 is_top_level_on_external 多一条规则 ——
 * 第一层目录不存在时，它下面的子路径也拦（避免「父目录被拦 → 子路径 ENOENT → 调用方崩」）。
 * Java 层不需要这条：`File.mkdirs()` 是逐级调 `mkdir` 的，每一级都会被单独判定到。
 */
class DirGuardPolicyTest {

    private fun check(path: String?, expect: Boolean) {
        assertEquals("path=$path", expect, DirGuardPolicy.isTopLevelOnExternal(path))
    }

    @Test
    fun blocksFirstLevelOnExternalVolumes() {
        check("/storage/1234-ABCD/Music", true)
        check("/storage/1234-ABCD/Music/", true)      // 尾部 '/' 不影响
        check("/storage/1234-ABCD/Android", true)
        check("/mnt/media_rw/1234-ABCD/DCIM", true)
        check("/mnt/secure/asec/Movies", true)
        check("/mnt/user/0/1234-ABCD/Pictures", true)
        check("/mnt/runtime/write/1234-ABCD/Download", true)
    }

    @Test
    fun ignoresInternalStorage() {
        check("/storage/emulated/0/Music", false)
        check("/storage/emulated/0/Download/x", false)
        check("/mnt/user/0/emulated/0/Music", false)
        check("/mnt/runtime/read/emulated/0/Music", false)
    }

    @Test
    fun ignoresVolumeRootItself() {
        check("/storage/1234-ABCD", false)
        check("/storage/1234-ABCD/", false)
    }

    @Test
    fun ignoresUnrelatedPaths() {
        check("/data/system/foo", false)
        check("/sdcard/Music", false)
        check("/storage", false)
        check("", false)
        check(null, false)
    }

    @Test
    fun ignoresDeeperPaths() {
        // Java 层这一侧：更深层一律放过（逐级判定由 mkdirs 拆分完成）
        check("/storage/1234-ABCD/Music/Album", false)
        check("/storage/1234-ABCD/Music/Album/cover", false)
    }
}
