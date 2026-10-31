package com.proresforge.app.engine

import com.proresforge.app.fakeUri
import com.proresforge.app.data.AudioMode
import com.proresforge.app.data.FpsMode
import com.proresforge.app.data.JobSpec
import com.proresforge.app.data.JobState
import com.proresforge.app.data.OutFormat
import com.proresforge.app.data.QualityTier
import com.proresforge.app.data.ScaleMode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking

/**
 * JobBus 状态机回归(第 9 轮审查 §7 测试缺口②;K-01/K-02 都在这一带)。
 *
 * JobBus 是全局单例:每个用例都用 [JobBus.enqueue] 自己生成的 id(入队返回值),
 * 只对「自己的」记录断言,用例之间互不依赖全局 map 的整体内容。
 *
 * 说明:cancel(RUNNING) 会调 FFmpeg.nativeCancel();纯 JVM 下 System.loadLibrary
 * 失败抛 UnsatisfiedLinkError,被 JobBus 的 catch(Throwable) 吞掉(与真机 .so
 * 未就绪的路径一致),因此本类可以直接覆盖「取消运行中任务」的状态转换。
 */
class JobBusTest {

    private fun spec(name: String) = JobSpec(
        inputUri = fakeUri(), inputName = name,
        outputUri = fakeUri(), outputName = "out-$name",
        format = OutFormat.H264, tier = QualityTier.MEDIUM,
        scale = ScaleMode.ORIG, fps = FpsMode.ORIG,
        lutPath = null, lutName = null, lutIntensity = 1f,
        audio = AudioMode.COPY, tenBit = false, faststart = true,
    )

    private fun enqueueOne(name: String = "a"): String =
        JobBus.enqueue(listOf(spec(name))).single()

    private fun enqueueTwo(): Pair<String, String> =
        JobBus.enqueue(listOf(spec("a"), spec("b"))).let { it[0] to it[1] }

    private fun record(id: String) = JobBus.jobs.value.getValue(id)

    // ------------------------------------------------------------------ enqueue

    @Test
    fun enqueueReturnsDistinctIdsAndQueuedRecords() {
        val (a, b) = enqueueTwo()
        assertNotEquals(a, b)
        assertEquals(JobState.QUEUED, record(a).state)
        assertEquals(JobBus.STAGE_QUEUED, record(a).stage)
        assertEquals(JobState.QUEUED, record(b).state)
    }

    // ------------------------------------------------------------------ begin

    @Test
    fun beginMovesQueuedToRunningAndSetsCurrentId() {
        val id = enqueueOne("x")
        assertTrue(JobBus.begin(id))
        val rec = record(id)
        assertEquals(JobState.RUNNING, rec.state)
        assertEquals(JobBus.STAGE_INIT, rec.stage)
        assertEquals(id, JobBus.currentId.value)
        assertTrue(rec.startedAt > 0L)
    }

    @Test
    fun beginUnknownOrNonQueuedReturnsFalse() {
        assertFalse(JobBus.begin("no-such-id"))
        val id = enqueueOne("y")
        assertTrue(JobBus.begin(id))
        // 已是 RUNNING:第二次 begin 必须拒绝(串行队列不变量)
        assertFalse(JobBus.begin(id))
    }

    // ------------------------------------------------------------------ progress / finish

    @Test
    fun progressOnlyAppliesToRunning() {
        val (run, queued) = enqueueTwo()
        JobBus.begin(run)
        JobBus.progress(run, 42, "decode", 10L, 20L)
        val r = record(run)
        assertEquals(42, r.pct)
        assertEquals("decode", r.stage)
        assertEquals(10L, r.frame)
        // QUEUED 记录不受 progress 影响
        JobBus.progress(queued, 99, "decode", 1L, 1L)
        assertEquals(0, record(queued).pct)
        // 越界 pct 被夹紧
        JobBus.progress(run, 120, "decode", 11L, 20L)
        assertEquals(100, record(run).pct)
    }

    @Test
    fun finishSetsTerminalStateAndClearsCurrentId() {
        val id = enqueueOne("f")
        JobBus.begin(id)
        JobBus.finish(id, JobState.DONE, null, outSize = 123L, durationMs = 5L, inSize = 456L)
        val rec = record(id)
        assertEquals(JobState.DONE, rec.state)
        assertEquals(123L, rec.outSize)
        assertEquals(456L, rec.inSize)
        assertNull(JobBus.currentId.value)
    }

    @Test
    fun finishTerminalGuardPreventsOverwrite() {
        // 终态守卫:已 CANCELLED 的记录不能被 finish(DONE) 覆盖
        // (用户点了取消但 nativeRun 返回 0 的竞态,契约 §7)
        val id = enqueueOne("g")
        JobBus.begin(id)
        JobBus.cancel(id)
        JobBus.finish(id, JobState.DONE, null, 1L, 1L, 1L)
        assertEquals(JobState.CANCELLED, record(id).state)
    }

    // ------------------------------------------------------------------ cancel

    @Test
    fun cancelQueuedKeepsRecordMarkedCancelled() {
        val id = enqueueOne("c")
        JobBus.cancel(id)
        val rec = record(id)
        assertEquals(JobState.CANCELLED, rec.state)
        assertEquals(JobBus.STAGE_CANCELLED, rec.stage)
        // 取消仍保留记录(历史留痕),begin 必须拒绝
        assertFalse(JobBus.begin(id))
    }

    @Test
    fun cancelRunningThenBeginNextTaskIsClean() {
        // K-01 场景的 JobBus 侧:任务 A 运行中被取消后,下一任务 B 的 begin 必须
        // 正常返回 true、currentId 切到 B(残留 pending 的清理在 Service 层)。
        val (a, b) = enqueueTwo()
        assertTrue(JobBus.begin(a))
        JobBus.cancel(a)
        assertEquals(JobState.CANCELLED, record(a).state)
        assertTrue(JobBus.begin(b))
        assertEquals(b, JobBus.currentId.value)
        assertFalse(JobBus.begin(a))
    }

    @Test
    fun cancelUnknownOrTerminalIsNoOp() {
        JobBus.cancel("no-such-id") // 不抛
        val id = enqueueOne("t")
        JobBus.begin(id)
        JobBus.finish(id, JobState.FAILED, "boom", 0L, 0L, 0L)
        JobBus.cancel(id) // 终态:不覆盖
        assertEquals(JobState.FAILED, record(id).state)
    }

    // ------------------------------------------------------------------ removeQueued / clearFinished

    @Test
    fun removeQueuedRemovesOnlyQueuedRecords() {
        // K-04 回滚:入队后启动服务失败,把仍 QUEUED 的任务撤下来;
        // 已被 begin 的(RUNNING)不动。
        val ids = JobBus.enqueue(listOf(spec("ra"), spec("rb"), spec("rc")))
        assertEquals(3, ids.size)
        assertTrue(JobBus.begin(ids[2]))
        JobBus.removeQueued(ids)
        assertFalse(JobBus.jobs.value.containsKey(ids[0]))
        assertFalse(JobBus.jobs.value.containsKey(ids[1]))
        assertTrue(JobBus.jobs.value.containsKey(ids[2]))
        assertEquals(JobState.RUNNING, record(ids[2]).state)
    }

    @Test
    fun clearFinishedKeepsActiveRecords() {
        val (done, queued) = enqueueTwo()
        JobBus.begin(done)
        JobBus.finish(done, JobState.DONE, null, 1L, 1L, 1L)
        JobBus.clearFinished()
        assertFalse(JobBus.jobs.value.containsKey(done))
        assertTrue(JobBus.jobs.value.containsKey(queued))
        assertEquals(JobState.QUEUED, record(queued).state)
        assertNotEquals(done, JobBus.currentId.value)
    }

    // ---------------------------------------------------- R10-9 销毁窗口的终态判定

    @Test
    fun interruptedOutcomeKeepsNativeSuccess() {
        // native 返回 0、runQueue 的 finish 还没跑就被销毁:落 DONE,输出不删
        val id = enqueueOne("ok")
        JobBus.begin(id)
        val d = resolveInterruptedOutcome(record(id), JobState.DONE)
        assertEquals(JobState.DONE, d.state)
        assertFalse(d.deleteOutput)
    }

    @Test
    fun interruptedOutcomeDeletesFailedAndCancelled() {
        val f = enqueueOne("f")
        JobBus.begin(f)
        val df = resolveInterruptedOutcome(record(f), JobState.FAILED)
        assertEquals(JobState.FAILED, df.state)
        assertTrue(df.deleteOutput)

        val c = enqueueOne("c")
        JobBus.begin(c)
        val dc = resolveInterruptedOutcome(record(c), JobState.CANCELLED)
        assertEquals(JobState.CANCELLED, dc.state)
        assertTrue(dc.deleteOutput)
    }

    @Test
    fun interruptedOutcomeWithoutResultIsCancelled() {
        // native 还没返回(销毁发生在 nativeRun 期间):维持取消,输出在 native 停止后删
        val id = enqueueOne("run")
        JobBus.begin(id)
        val d = resolveInterruptedOutcome(record(id), null)
        assertEquals(JobState.CANCELLED, d.state)
        assertTrue(d.deleteOutput)
    }

    @Test
    fun interruptedOutcomeNeverOverridesTerminalRecord() {
        // 运行期用户取消 + native 之后返回 0:仍是取消、删输出,不被盖回 DONE
        val id = enqueueOne("mid")
        JobBus.begin(id)
        JobBus.cancel(id)
        val d = resolveInterruptedOutcome(record(id), JobState.DONE)
        assertEquals(JobState.CANCELLED, d.state)
        assertTrue(d.deleteOutput)

        // 记录已经是 DONE:销毁路径不改状态、不删成品
        val ok = enqueueOne("done")
        JobBus.begin(ok)
        JobBus.finish(ok, JobState.DONE, null, 7L, 1L, 1L)
        val d2 = resolveInterruptedOutcome(record(ok), JobState.CANCELLED)
        assertEquals(JobState.DONE, d2.state)
        assertFalse(d2.deleteOutput)
    }

    // ---------------------------------------------------- R10-9 文件大小单独回填

    @Test
    fun updateOutSizeFillsOnlyDoneRecords() {
        val id = enqueueOne("size")
        JobBus.begin(id)
        JobBus.finish(id, JobState.DONE, null, 0L, 1L, 1L)
        JobBus.updateOutSize(id, 4242L)
        assertEquals(4242L, record(id).outSize)
        assertEquals(JobState.DONE, record(id).state)

        // 取消/失败的任务大小本就是 0,no-op(也不改状态)
        val c = enqueueOne("csize")
        JobBus.begin(c)
        JobBus.finish(c, JobState.CANCELLED, null, 0L, 1L, 0L)
        JobBus.updateOutSize(c, 4242L)
        assertEquals(0L, record(c).outSize)
        assertEquals(JobState.CANCELLED, record(c).state)
    }

    // ---------------------------------------------------- R12-13 finish/updateOutSize 原子回传

    @Test
    fun finishReturnsFinalRecordWithoutRereading() {
        // finish 返回终态落定后的记录,调用方落库直接用它,不再回读 jobs 活值
        // (否则与 clearFinished 竞态时记录刚被清掉,持久历史写入被跳过)。
        val id = enqueueOne("ret")
        JobBus.begin(id)
        val rec = JobBus.finish(id, JobState.DONE, null, 123L, 5L, 456L)
        assertEquals(JobState.DONE, rec?.state)
        assertEquals(123L, rec?.outSize)
        assertEquals(456L, rec?.inSize)
        // 终态守卫:已终态时原样返回,不被覆盖
        val again = JobBus.finish(id, JobState.FAILED, "boom", 0L, 1L, 1L)
        assertEquals(JobState.DONE, again?.state)
    }

    @Test
    fun updateOutSizeReturnsUpdatedRecord() {
        val id = enqueueOne("ret2")
        JobBus.begin(id)
        JobBus.finish(id, JobState.DONE, null, 0L, 1L, 1L)
        assertEquals(4242L, JobBus.updateOutSize(id, 4242L)?.outSize)
        // 非 DONE:原样返回,不改写状态与大小
        val c = enqueueOne("ret3")
        JobBus.begin(c)
        JobBus.finish(c, JobState.CANCELLED, null, 0L, 1L, 0L)
        val rec = JobBus.updateOutSize(c, 9L)
        assertEquals(JobState.CANCELLED, rec?.state)
        assertEquals(0L, rec?.outSize)
    }

    @Test
    fun terminalStateCoversDurations() {
        assertFalse(JobState.QUEUED.isTerminalState())
        assertFalse(JobState.RUNNING.isTerminalState())
        assertTrue(JobState.DONE.isTerminalState())
        assertTrue(JobState.FAILED.isTerminalState())
        assertTrue(JobState.CANCELLED.isTerminalState())
    }

    // ---------------------------------------------------- P2-04 并发场景

    @Test
    fun `并发取消与 finish 不应导致状态不一致`() = runBlocking {
        // 准备:入队并开始一个任务
        val id = enqueueOne("conc")
        assertTrue(JobBus.begin(id))

        // 并发场景:一个协程反复取消,另一个协程反复 finish
        val job1 = launch(Dispatchers.Default) {
            repeat(100) {
                JobBus.cancel(id)
                delay(1)
            }
        }

        val job2 = launch(Dispatchers.Default) {
            repeat(100) {
                JobBus.finish(id, JobState.DONE, null, 1000L, 100L, 500L)
                delay(1)
            }
        }

        job1.join()
        job2.join()

        // 断言:最终状态应是终态(CANCELLED 或 DONE),不应是中间态。
        // 终态守卫:cancel 在前 → CANCELLED;finish 在前 → DONE;
        // 两者都是合法结果,不断言具体值。
        val finalRec = JobBus.jobs.value[id]
        assertNotNull(finalRec)
        assertTrue("最终状态应是终态", finalRec!!.state.isTerminalState())
    }

    @Test
    fun `并发 begin 同一任务应只有一个成功`() = runBlocking {
        // 10 个协程同时 begin 仍处于 QUEUED 的任务:状态机锁保证只有一个
        // QUEUED→RUNNING 转换成功(串行队列不变量,P2-04)。
        val id = enqueueOne("race")
        val results = (1..10).map {
            async(Dispatchers.Default) {
                JobBus.begin(id)
            }
        }.awaitAll()

        val successCount = results.count { it }
        assertEquals("只有一个 begin 应成功", 1, successCount)
        assertEquals(JobState.RUNNING, record(id).state)
    }

    @Test
    fun `并发 begin 已运行任务应全部拒绝`() = runBlocking {
        // 已是 RUNNING:并发重试 begin 也必须全部拒绝(串行队列不变量)。
        val id = enqueueOne("race2")
        assertTrue(JobBus.begin(id))
        val results = (1..10).map {
            async(Dispatchers.Default) {
                JobBus.begin(id)
            }
        }.awaitAll()

        val successCount = results.count { it }
        assertEquals("运行中的任务不允许再次 begin", 0, successCount)
    }
}
