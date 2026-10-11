package com.proresforge.app.engine

import com.proresforge.app.fakeUri
import com.proresforge.app.data.AudioMode
import com.proresforge.app.data.FpsMode
import com.proresforge.app.data.JobRecord
import com.proresforge.app.data.JobSpec
import com.proresforge.app.data.JobState
import com.proresforge.app.data.OutFormat
import com.proresforge.app.data.QualityTier
import com.proresforge.app.data.ScaleMode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * TranscodeService 核心逻辑测试(P2-03;不涉及 Android 组件)。
 *
 * 限制:纯 JVM 测试无法测试 Service 生命周期,只测试纯函数部分 —— 即
 * [resolveInterruptedOutcome](JobBus.kt 中的 internal 顶层函数,Service 销毁
 * 路径 finalizeInterrupted 的终态判定,R10-9 语义)。
 * 完整测试需要 Instrumentation Test(真机/模拟器)。
 *
 * 说明:这批断言与 JobBusTest 的 R10-9 小节互为双保险 —— JobBusTest 从
 * 「JobBus 状态机」角度覆盖,本类从「Service 销毁收尾」角度固定契约:
 * 不是 DONE 就删输出、终态记录不被 outcome 覆盖。
 */
class TranscodeServiceTest {

    // ---------------------------------------------------- resolveInterruptedOutcome

    @Test
    fun `resolveInterruptedOutcome - native 返回 DONE 应落 DONE`() {
        val record = mockJobRecord(state = JobState.RUNNING)
        val outcome = JobState.DONE

        val result = resolveInterruptedOutcome(record, outcome)

        assertEquals(JobState.DONE, result.state)
        assertFalse("DONE 不应删除输出", result.deleteOutput)
    }

    @Test
    fun `resolveInterruptedOutcome - native 返回 CANCELLED 应删除输出`() {
        val record = mockJobRecord(state = JobState.RUNNING)
        val outcome = JobState.CANCELLED

        val result = resolveInterruptedOutcome(record, outcome)

        assertEquals(JobState.CANCELLED, result.state)
        assertTrue("CANCELLED 应删除输出", result.deleteOutput)
    }

    @Test
    fun `resolveInterruptedOutcome - native 未返回应判 CANCELLED`() {
        val record = mockJobRecord(state = JobState.RUNNING)
        val outcome = null

        val result = resolveInterruptedOutcome(record, outcome)

        assertEquals(JobState.CANCELLED, result.state)
        assertTrue(result.deleteOutput)
    }

    @Test
    fun `resolveInterruptedOutcome - 记录已 CANCELLED 不被 outcome 覆盖`() {
        val record = mockJobRecord(state = JobState.CANCELLED)
        val outcome = JobState.DONE // native 返回成功

        val result = resolveInterruptedOutcome(record, outcome)

        assertEquals("终态守卫:CANCELLED 不被盖回 DONE",
            JobState.CANCELLED, result.state)
        assertTrue(result.deleteOutput)
    }

    @Test
    fun `resolveInterruptedOutcome - 记录已 DONE 不删除输出`() {
        val record = mockJobRecord(state = JobState.DONE)
        val outcome = JobState.CANCELLED // 不应发生,但测试守卫

        val result = resolveInterruptedOutcome(record, outcome)

        assertEquals(JobState.DONE, result.state)
        assertFalse("已 DONE 的记录不删除输出", result.deleteOutput)
    }

    @Test
    fun `resolveInterruptedOutcome - FAILED 应删除输出`() {
        val record = mockJobRecord(state = JobState.RUNNING)
        val outcome = JobState.FAILED

        val result = resolveInterruptedOutcome(record, outcome)

        assertEquals(JobState.FAILED, result.state)
        assertTrue("FAILED 应删除半成品", result.deleteOutput)
    }

    // ------------------------------------------------------------------ Mock

    private fun mockJobRecord(state: JobState) = JobRecord(
        id = "test-id",
        spec = mockJobSpec(),
        state = state,
        pct = 0,
        stage = "",
        frame = 0L,
        totalFrames = 0L,
        error = null,
        startedAt = 0L,
        endedAt = 0L,
        outSize = 0L,
        inSize = 0L,
        durationMs = 0L
    )

    private fun mockJobSpec() = JobSpec(
        inputUri = fakeUri(),
        inputName = "test.mp4",
        outputUri = fakeUri(),
        outputName = "output.mp4",
        format = OutFormat.H264,
        tier = QualityTier.MEDIUM,
        scale = ScaleMode.ORIG,
        fps = FpsMode.ORIG,
        lutPath = null,
        lutName = null,
        lutIntensity = 1.0f,
        audio = AudioMode.COPY,
        tenBit = false,
        faststart = true
    )
}
