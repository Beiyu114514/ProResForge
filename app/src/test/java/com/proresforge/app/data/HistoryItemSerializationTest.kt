package com.proresforge.app.data

import kotlinx.serialization.encodeToString
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 历史条目序列化回归(JobStore 的 on-disk 格式)。
 *
 * 关键约束(见 model.kt 的 KDoc 与审查 L16):format/state 两个枚举按 **name** 落盘。
 * 枚举常量改名/重排会让旧 jobs.json 反序列化失败,JobStore catch 后回落空列表 →
 * 用户历史被静默清空。本测试把这条线格式钉住:谁改名,这里先红。
 */
class HistoryItemSerializationTest {

    private val item = HistoryItem(
        id = "3f2a1b0c",
        ts = 1_700_000_000_000L,
        inputName = "A001_C001.mov",
        outputName = "A001_C001.mp4",
        format = OutFormat.VP9,
        state = JobState.FAILED,
        error = "err_ffmpeg:-1094995529",
        lutName = "teal_orange.cube",
        lutIntensity = 0.75f,
        inSize = 1_048_576L,
        outSize = 524_288L,
        durationMs = 45_000L,
        outputUriString = "content://com.android.externalstorage.documents/document/primary%3AMovies%2Fx.mp4",
    )

    /** 全字段 round-trip(含 null 的 error/outputUriString)。 */
    @Test
    fun roundTripAllFields() {
        val text = Json.encodeToString(HistoryItem.serializer(), item)
        val back = Json.decodeFromString(HistoryItem.serializer(), text)
        assertEquals(item, back)
    }

    @Test
    fun roundTripWithNullOptionals() {
        val minimal = HistoryItem(
            id = "0",
            ts = 0L,
            inputName = "in.mkv",
            outputName = "out.mp4",
            format = OutFormat.H264,
            state = JobState.DONE,
            error = null,
            lutName = null,
            lutIntensity = 1f,
            inSize = 0L,
            outSize = 0L,
            durationMs = 0L,
            outputUriString = null,
        )
        val back = Json.decodeFromString<HistoryItem>(
            HistoryItem.serializer(),
            Json.encodeToString(HistoryItem.serializer(), minimal),
        )
        assertEquals(minimal, back)
    }

    /** 每个输出格式/任务状态都能被记录(枚举表变了这里先发现)。 */
    @Test
    fun everyEnumValueRoundTrips() {
        for (format in OutFormat.entries) {
            for (state in JobState.entries) {
                val probe = item.copy(format = format, state = state)
                val text = Json.encodeToString(HistoryItem.serializer(), probe)
                assertEquals(
                    "$format/$state",
                    probe,
                    Json.decodeFromString(HistoryItem.serializer(), text),
                )
            }
        }
    }

    /** 线格式必须是 name 而非 ordinal —— 改 name 会让旧文件读不出(静默清空)。 */
    @Test
    fun enumsAreSerializedByNameNotOrdinal() {
        val text = Json.encodeToString(HistoryItem.serializer(), item)
        assertTrue("format 应按 name 存: $text", text.contains("\"format\":\"VP9\""))
        assertTrue("state 应按 name 存: $text", text.contains("\"state\":\"FAILED\""))
        // 反序列化时按 name 匹配,而不是按数组下标
        val byName = Json.decodeFromString<HistoryItem>(
            HistoryItem.serializer(),
            text.replace("\"format\":\"VP9\"", "\"format\":\"PRORES\""),
        )
        assertEquals(OutFormat.PRORES, byName.format)
    }
}
