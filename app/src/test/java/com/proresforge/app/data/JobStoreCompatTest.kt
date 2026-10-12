package com.proresforge.app.data

import com.proresforge.app.fakeUri
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * JobStore 坏档/旧档兼容回归(第 9 轮审查 §7 测试缺口③ + S-3)。
 *
 * S-3 的核心断言:jobs.json 损坏时**不能静默清空** —— 必须把坏档备份成
 * jobs.json.corrupt-<时间戳> 并从空历史继续,否则下次 recordFinish 会把空列表
 * 原子写回,用户历史连人工恢复的机会都没有(枚举改名一次就能踩到)。
 *
 * JobStore 的 internal ctor 接收目录,进程内单例与 Android Context 不参与,
 * 因此可以纯 JVM 跑(build.gradle 已开 isReturnDefaultValues:
 * AppLog→android.util.Log 与 Uri 桩方法返回默认值)。
 */
class JobStoreCompatTest {

    @get:Rule
    val tmp = TemporaryFolder()

    /** HistoryItem JSON(minId 时长一个可序列化快照)。 */
    private fun json(vararg items: String): String = "[${items.joinToString()}]"

    private fun itemJson(
        id: String, ts: Long,
        format: String = "H264", state: String = "DONE",
        extraKeys: String = "", omitTrailing: Boolean = false,
    ): String {
        val base = """{"id":"$id","ts":$ts,"inputName":"in.mov","outputName":"out.mp4",""" +
            """"format":"$format","state":"$state","error":null,"lutName":null,""" +
            """"lutIntensity":1.0,"inSize":1,"outSize":2,"durationMs":3,""" +
            """"outputUriString":null"""
        val tail = if (omitTrailing) "" else ""","frames":0,"fps":0"""
        return base + tail + extraKeys + "}"
    }

    private fun awaitHistory(store: JobStore, predicate: (List<HistoryItem>) -> Boolean): List<HistoryItem> {
        // 首加载在 init 的 IO 协程里:轮询直到满足条件(上限 5s,超时按当前值返回)
        val deadline = System.currentTimeMillis() + 5_000
        var value = store.history.value
        while (!predicate(value) && System.currentTimeMillis() < deadline) {
            Thread.sleep(20)
            value = store.history.value
        }
        return value
    }

    /** JobStore 的 internal ctor 接收 jobs.json 的**文件路径**(目录会退化成 isFile=false)。 */
    private fun storeIn(dir: File): JobStore = JobStore(File(dir, "jobs.json"))

    private fun spec() = JobSpec(
        inputUri = fakeUri(), inputName = "in.mov",
        outputUri = fakeUri(), outputName = "out.mp4",
        format = OutFormat.H264, tier = QualityTier.MEDIUM,
        scale = ScaleMode.ORIG, fps = FpsMode.ORIG,
        lutPath = null, lutName = null, lutIntensity = 1f,
        audio = AudioMode.COPY, tenBit = false, faststart = true,
    )

    private fun record(id: String, outSize: Long = 2L, endedAt: Long = 2L) = JobRecord(
        id = id, spec = spec(), state = JobState.DONE, pct = 100,
        stage = "done", frame = 0L, totalFrames = 0L, error = null,
        startedAt = 1L, endedAt = endedAt, outSize = outSize, inSize = 1L, durationMs = 3L
    )

    // ------------------------------------------------------------------ S-3 坏档

    @Test
    fun corruptJsonIsBackedUpNotSilentlyCleared() {
        val dir = tmp.newFolder()
        val file = File(dir, "jobs.json")
        val corrupt = """[{"id":"x","ts":1,"state":"NOT_A_REAL_STATE"""" // 半截 + 非法枚举
        file.writeText(corrupt)

        val store = storeIn(dir)
        // 首加载在 init 的 IO 协程里:备份文件出现 = loadLocked 已跑过
        val deadline = System.currentTimeMillis() + 5_000
        var backups = dir.listFiles { f -> f.name.startsWith("jobs.json.corrupt-") }.orEmpty()
        while (backups.isEmpty() && System.currentTimeMillis() < deadline) {
            Thread.sleep(20)
            backups = dir.listFiles { f -> f.name.startsWith("jobs.json.corrupt-") }.orEmpty()
        }
        assertTrue("坏档应从空历史开始", store.history.value.isEmpty())
        // 原文件必须原样保留(没有被空列表覆盖),且存在 corrupt 备份
        assertEquals("坏档内容不得被改写", corrupt, file.readText())
        assertEquals("应恰好生成一个坏档备份", 1, backups.size)
        assertEquals("备份内容 = 坏档原文", corrupt, backups.single().readText())
    }

    @Test
    fun corruptFileThenRecordFinishContinuesOnFreshHistory() {
        val dir = tmp.newFolder()
        File(dir, "jobs.json").writeText("not json at all")
        val store = storeIn(dir)
        awaitHistory(store) { true }

        runBlocking { store.recordFinish(record("after-corrupt")) }
        // 落库后文件重建为合法 JSON,坏档备份仍在
        assertEquals(listOf("after-corrupt"), store.history.value.map { it.id })
        assertEquals(1, dir.listFiles { f -> f.name.startsWith("jobs.json.corrupt-") }!!.size)
        assertTrue(File(dir, "jobs.json").readText().contains("after-corrupt"))
    }

    // ------------------------------------------------------------------ 旧版本兼容

    @Test
    fun legacyJsonMissingFramesAndFpsAndUnknownKeysLoads() {
        val dir = tmp.newFolder()
        // 旧版本快照:没有 frames/fps 字段;还有现在已不存在的 legacy 键
        File(dir, "jobs.json").writeText(
            json(itemJson(id = "old1", ts = 100L, extraKeys = ""","legacyField":"?"""", omitTrailing = true))
        )
        val store = storeIn(dir)
        val history = awaitHistory(store) { it.isNotEmpty() }
        assertEquals(1, history.size)
        val item = history.single()
        assertEquals("old1", item.id)
        assertEquals(OutFormat.H264, item.format)   // 枚举按 name
        assertEquals(JobState.DONE, item.state)
        assertEquals(0L, item.frames)               // model.kt 默认值
        assertEquals(0, item.fps)
        assertNull(item.outputUriString)
    }

    @Test
    fun itemsAreSortedByTsDescending() {
        val dir = tmp.newFolder()
        File(dir, "jobs.json").writeText(
            json(itemJson("a", 100L), itemJson("b", 300L), itemJson("c", 200L))
        )
        val store = storeIn(dir)
        val history = awaitHistory(store) { it.size == 3 }
        assertEquals(listOf("b", "c", "a"), history.map { it.id })
    }

    // ------------------------------------------------------------------ 落盘/去重

    @Test
    fun recordFinishDedupsByIdAndPersists() = runBlocking {
        val dir = tmp.newFolder()
        val store = storeIn(dir)
        awaitHistory(store) { true }
        store.recordFinish(record("same-id", outSize = 2L))
        store.recordFinish(record("same-id", outSize = 99L))
        store.recordFinish(record("other-id"))

        assertEquals(listOf("other-id", "same-id"), store.history.value.map { it.id })
        assertEquals(99L, store.history.value.single { it.id == "same-id" }.outSize)

        // 新实例从磁盘读到同一份(内部构造,绕过进程单例)
        val second = storeIn(dir)
        val reloaded = awaitHistory(second) { it.size == 2 }
        assertEquals(listOf("other-id", "same-id"), reloaded.map { it.id })
    }

    @Test
    fun clearRewritesEmptyHistoryAndKeepsFile() = runBlocking {
        val dir = tmp.newFolder()
        File(dir, "jobs.json").writeText(json(itemJson("x", 1L)))
        val store = storeIn(dir)
        awaitHistory(store) { it.size == 1 }

        store.clear()
        assertTrue(store.history.value.isEmpty())
        assertEquals("[]", File(dir, "jobs.json").readText())
    }

    // ------------------------------------------------------------------ MAX_ITEMS 上限(第 11 轮审查 §11)

    @Test
    fun recordFinishTruncatesHistoryAtMaxItems() = runBlocking {
        val dir = tmp.newFolder()
        val store = storeIn(dir)
        awaitHistory(store) { true }

        // 写 MAX_ITEMS + 1 条(endedAt 递增 = ts 递增,最先写的最旧)。
        // recordFinish 是「新条目插到最前 + take(MAX_ITEMS)」:第 201 条进、
        // 最旧的 job-000 淘汰,总量恒为 200。
        repeat(JobStore.MAX_ITEMS + 1) { i ->
            store.recordFinish(record("job-%03d".format(i), endedAt = 1000L + i))
        }

        val ids = store.history.value.map { it.id }
        assertEquals(JobStore.MAX_ITEMS, ids.size)
        assertFalse("最旧一条应被淘汰", ids.contains("job-000"))
        assertEquals("新条目在最前", "job-200", ids.first())
        assertEquals("次旧一条保留", "job-001", ids.last())

        // 落盘的也是截断后的列表(不是只在内存里截)
        val second = storeIn(dir)
        val reloaded = awaitHistory(second) { it.size == JobStore.MAX_ITEMS }
        assertEquals(ids, reloaded.map { it.id })
        assertFalse(reloaded.map { it.id }.contains("job-000"))
    }
}
