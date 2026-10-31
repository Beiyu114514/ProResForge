package com.proresforge.app.engine

import com.proresforge.app.data.JobRecord
import com.proresforge.app.data.JobSpec
import com.proresforge.app.data.JobState
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.util.UUID

/**
 * 转码任务总线(契约 §7):进程内单例队列。
 *
 * - [jobs] 保存全部任务(**含 QUEUED**),用 LinkedHashMap 保持入队顺序,因此
 *   「第一个 QUEUED」就是队首;
 * - 串行执行由 [TranscodeService] 驱动(它按 [currentId]/QUEUED 顺序取任务),
 *   本对象不做任何并发/线程调度;
 * - [begin]/[progress]/[finish] 只有 Service 会调;UI 只读 [jobs]/[currentId] 并调 [cancel]。
 * - 所有变更都通过 [MutableStateFlow.update] 原子完成,可由 UI 线程与 Service 回调线程交错调用。
 * - [cancel]/[begin]/[finish] 三处状态机转换共用同一把锁:否则「begin() 刚把记录
 *   标成 RUNNING、但 _currentId 还没写入」的间隙里并发 cancel(),会把正在运行的任务
 *   误判成排队任务,取消动作落在错误的分支上。
 */
object JobBus {

    /** 尚未开始的任务的初始阶段文案。 */
    const val STAGE_QUEUED = "queued"

    /** 开始跑时的阶段(§5 第一阶段)。 */
    const val STAGE_INIT = "init"

    /** 取消后的阶段文案(与 JobsScreen / 历史记录一致)。 */
    const val STAGE_CANCELLED = "cancelled"

    /** [cancel]/[begin]/[finish] 的状态机锁。 */
    private val lock = Any()

    private val _jobs = MutableStateFlow<Map<String, JobRecord>>(LinkedHashMap())

    /** 全部任务(含 QUEUED),保持入队顺序。 */
    val jobs: StateFlow<Map<String, JobRecord>> = _jobs.asStateFlow()

    private val _currentId = MutableStateFlow<String?>(null)

    /** 正在执行的任务 id;没有则为 null。 */
    val currentId: StateFlow<String?> = _currentId.asStateFlow()

    /**
     * 入队:按传入顺序生成任务(UUID 前 8 位作 id),初始 [JobState.QUEUED]。
     *
     * @return 生成的任务 id(与 [list] 顺序一致)。调用方(K-04)在启动前台服务
     *   失败时用它调 [removeQueued] 回滚。
     */
    fun enqueue(list: List<JobSpec>): List<String> {
        if (list.isEmpty()) return emptyList()
        val added = LinkedHashMap<String, JobRecord>()
        val ids = ArrayList<String>(list.size)
        list.forEach { spec ->
            var id = newId()
            while (_jobs.value.containsKey(id) || added.containsKey(id)) id = newId()
            added[id] = JobRecord(
                id = id,
                spec = spec,
                state = JobState.QUEUED,
                pct = 0,
                stage = STAGE_QUEUED,
                frame = 0L,
                totalFrames = 0L,
                error = null,
                startedAt = 0L,
                endedAt = 0L,
                outSize = 0L,
                inSize = 0L,
                durationMs = 0L
            )
            ids += id
        }
        _jobs.update { cur ->
            val next = LinkedHashMap(cur)
            next.putAll(added)
            next
        }
        return ids
    }

    /**
     * 取消任务。
     * - 正在运行(RUNNING):调 FFmpeg.nativeCancel(),并立刻把状态改成 CANCELLED。
     *   nativeRun 返回后 Service 的 finish() 不会把 CANCELLED 覆盖成 DONE/FAILED
     *   (见 [finish] 的终态守卫),通知按最终状态发;
     * - 还在排队(QUEUED):同样标成 CANCELLED 并**保留记录**,让 UI/历史看得到
     *   「排队阶段就被取消」,而不是无声无息从队列里消失;
     * - 已是终态:不再覆盖。
     */
    fun cancel(id: String) {
        if (id.isEmpty()) return
        synchronized(lock) {
            val rec = _jobs.value[id] ?: return
            if (rec.state != JobState.RUNNING && rec.state != JobState.QUEUED) return
            if (rec.state == JobState.RUNNING) {
                try {
                    FFmpeg.nativeCancel()
                } catch (e: Throwable) {
                    // .so 未就绪时 nativeCancel 抛 UnsatisfiedLinkError;此时也不可能有任务在跑
                }
            }
            _jobs.update { map ->
                val cur = map[id] ?: return@update map
                val next = LinkedHashMap(map)
                next[id] = cur.copy(
                    state = JobState.CANCELLED,
                    stage = STAGE_CANCELLED,
                    endedAt = System.currentTimeMillis()
                )
                next
            }
        }
    }

    /** 清掉已终结(DONE/FAILED/CANCELLED)的任务;队列与当前任务不受影响。 */
    fun clearFinished() {
        synchronized(lock) {
            _jobs.update { map ->
                val next = LinkedHashMap<String, JobRecord>()
                map.forEach { (k, v) -> if (!v.state.isTerminalState()) next[k] = v }
                next
            }
        }
    }

    /**
     * K-04 回滚:把仍处于 QUEUED 的 [ids] 从队列里移除(排队期已取消的记录一并移除,
     * 与「取消排队任务保留记录」不冲突 —— 那条语义针对的是用户主动取消,这里回滚
     * 的是从未开始过、也不该被看见的入队失败残留)。
     * 只在**还没** begin 的记录上生效:服务已经开始跑的(id 不在 QUEUED)不动,
     * 避免回滚误删正在执行的任务。
     */
    fun removeQueued(ids: Collection<String>) {
        if (ids.isEmpty()) return
        val idSet = ids.toHashSet()
        synchronized(lock) {
            _jobs.update { map ->
                val next = LinkedHashMap<String, JobRecord>()
                map.forEach { (k, v) ->
                    if (k in idSet && v.state == JobState.QUEUED) return@forEach
                    next[k] = v
                }
                next
            }
        }
    }

    // ---------------------------------------------------------- 以下仅 Service 调用

    /**
     * 标记任务开始执行。
     *
     * @return true = 该任务确实存在、处于 QUEUED 并已被标记 RUNNING;
     *   false = 不存在或已被取消(排队期取消),Service 应跳过
     */
    fun begin(id: String): Boolean {
        val started = System.currentTimeMillis()
        var ok = false
        synchronized(lock) {
            _jobs.update { map ->
                val rec = map[id] ?: return@update map
                if (rec.state != JobState.QUEUED) return@update map
                ok = true
                val next = LinkedHashMap(map)
                next[id] = rec.copy(
                    state = JobState.RUNNING,
                    pct = 0,
                    stage = STAGE_INIT,
                    frame = 0L,
                    totalFrames = 0L,
                    error = null,
                    startedAt = started
                )
                next
            }
            // _currentId 的写入与「标 RUNNING」在同一把锁内完成:
            // 否则 cancel() 会在间隙里把当前任务当成排队任务处理(见 lock 注释)。
            if (ok) _currentId.value = id
        }
        return ok
    }

    /** 进度更新(节流后由 Service 调用)。 */
    fun progress(id: String, pct: Int, stage: String, frame: Long, totalFrames: Long) {
        _jobs.update { map ->
            val rec = map[id] ?: return@update map
            if (rec.state != JobState.RUNNING) return@update map
            val next = LinkedHashMap(map)
            next[id] = rec.copy(
                pct = pct.coerceIn(0, 100),
                stage = stage,
                frame = frame,
                totalFrames = totalFrames
            )
            next
        }
    }

    /**
     * 收尾:DONE/FAILED/CANCELLED 之一,由 Service 在 nativeRun 返回后调用。
     *
     * 终态守卫:记录已经是终态时不再改写。否则「用户点了取消 → nativeRun 返回 0」
     * 会把 CANCELLED 覆盖成 DONE,Service 据此弹出「完成」通知,而用户看到的是取消。
     *
     * @return 终态落定后的记录(已是终态则原样返回;id 不存在为 null)。
     *   R12-13:调用方落库/发通知一律用返回值,不得再回读 [jobs] 活值 ——
     *   否则与 UI 的 clearFinished 竞态时记录可能刚被清掉,历史写入被跳过。
     */
    fun finish(id: String, state: JobState, error: String?, outSize: Long, durationMs: Long, inSize: Long = 0L): JobRecord? {
        val ended = System.currentTimeMillis()
        var result: JobRecord? = null
        synchronized(lock) {
            _jobs.update { map ->
                val rec = map[id] ?: return@update map
                if (rec.state.isTerminalState()) {
                    result = rec
                    return@update map
                }
                val next = LinkedHashMap(map)
                val updated = rec.copy(
                    state = state,
                    error = error,
                    outSize = outSize,
                    inSize = inSize,
                    durationMs = durationMs,
                    endedAt = ended
                )
                next[id] = updated
                result = updated
                next
            }
            if (_currentId.value == id) _currentId.value = null
        }
        return result
    }

    /**
     * 只回填输出大小(不碰状态)。
     *
     * R10-9:runJob 现在在 nativeRun 返回后、statSize 之前就提前 finish(DONE)
     * (销毁窗口的成功保护),而 [finish] 有终态守卫、不会二次写入,文件大小只能
     * 走这一条单独通道补。仅对 DONE 有意义;取消/失败的任务大小本就是 0,no-op。
     *
     * @return 更新后的记录(守卫拒绝时原样返回;id 不存在为 null),语义同 [finish]。
     */
    fun updateOutSize(id: String, outSize: Long): JobRecord? {
        var result: JobRecord? = null
        synchronized(lock) {
            _jobs.update { map ->
                val rec = map[id] ?: return@update map
                if (rec.state != JobState.DONE) {
                    result = rec
                    return@update map
                }
                if (rec.outSize == outSize) {
                    result = rec
                    return@update map
                }
                val next = LinkedHashMap(map)
                val updated = rec.copy(outSize = outSize)
                next[id] = updated
                result = updated
                next
            }
        }
        return result
    }

    // ------------------------------------------------------------------ 内部实现

    private fun newId(): String = UUID.randomUUID().toString().take(8)
}

/** 是否终结态(与 [JobState] 定义保持一致;[JobBus.finish] 的守卫同源)。 */
internal fun JobState.isTerminalState(): Boolean = when (this) {
    JobState.QUEUED, JobState.RUNNING -> false
    JobState.DONE, JobState.FAILED, JobState.CANCELLED -> true
}

/** [resolveInterruptedOutcome] 的判定结果:落什么终态、要不要删输出文档。 */
internal data class InterruptedOutcome(val state: JobState, val deleteOutput: Boolean)

/**
 * R10-9:服务销毁撞上任务时的终态判定(**纯函数**,由 JVM 单测直接覆盖)。
 *
 * [outcome] 是 native 已经跑完、登记在 ActiveRun 上的最终结论:
 * - `DONE` = native 返回 0,成品已写完 → 落 DONE,输出**不删**;
 * - `FAILED` / `CANCELLED` → 落该状态,删除半成品;
 * - `null` = native 还没返回(或根本没有 run) → 维持 CANCELLED,删除输出;
 * - 其它非终态值按「没有结论」处理(防御,正常不会出现)。
 *
 * 记录本身已是终态时状态不变:这正是 [JobBus.finish] 终态守卫的语义 ——
 * 用户取消过的任务不会因为 native 稍后返回 0 被盖回 DONE。删除输出与否统一
 * 看最终状态:「不是 DONE 就删」。
 */
internal fun resolveInterruptedOutcome(record: JobRecord, outcome: JobState?): InterruptedOutcome {
    val state = when {
        record.state.isTerminalState() -> record.state
        outcome == JobState.DONE -> JobState.DONE
        outcome == JobState.FAILED -> JobState.FAILED
        outcome == JobState.CANCELLED -> JobState.CANCELLED
        else -> JobState.CANCELLED
    }
    return InterruptedOutcome(state, state != JobState.DONE)
}
