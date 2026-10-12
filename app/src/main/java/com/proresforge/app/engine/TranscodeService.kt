package com.proresforge.app.engine

import android.app.Notification
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import androidx.core.content.ContextCompat
import androidx.documentfile.provider.DocumentFile
import com.proresforge.app.R
import com.proresforge.app.data.JobRecord
import com.proresforge.app.data.JobSpec
import com.proresforge.app.data.JobState
import com.proresforge.app.data.JobStore
import com.proresforge.app.data.MediaProbe
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/**
 * 转码前台服务(契约 §7 / §9):串行驱动 [JobBus] 队列。
 *
 * 职责边界:
 * - 队列调度在这:循环取「第一个 QUEUED」→ begin → 跑一个任务 → finish → 写历史 → 下一个;
 * - 真正干活的 FFmpeg.nativeRun 在 Dispatchers.IO 上执行;进度回调切回主线程更新 JobBus 并
 *   节流(≥1s 且 ≥1%)刷新前台通知;
 * - 队列空 / 用户主动停止 → stopSelf;前台通知随服务销毁而消失。
 *
 * fd 语义(契约 §2):FFmpeg.writeFd/createOutFd 返回的裸 fd 归 native 所有,
 * 用完由 native close,这里**不** close。
 *
 * 不引用任何 UI 层类(不含 MainActivity/ui 目录)。
 */
class TranscodeService : Service() {

    /** 单任务执行结果(落库/发通知用的最终数据)。 */
    private class JobOutcome(
        val state: JobState,
        val error: String?,
        val outSize: Long,
        val durationMs: Long,
        val inSize: Long
    )

    /**
     * 当前任务的完成闸门;由 IO 侧完成,供 onDestroy 有界等待后再删输出。
     *
     * R10-9:[outcome] 登记 native 已跑完后的最终结论,**必须在 [done] 倒数之前写**:
     * 销毁路径(finalizeInterrupted)先等闸门再读它,读到的才是最终值。为 null 表示
     * native 还没返回(或任务在 native 之前就早退了之外的一切没有结论的情况)。
     */
    private class ActiveRun(
        val id: String,
        val done: CountDownLatch = CountDownLatch(1)
    ) {
        @Volatile
        var outcome: JobOutcome? = null
    }

    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    // 只在 worker 确认退出后做延迟收尾;历史优先在 onDestroy 有界同步等待。
    private val finalizeScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    @Volatile
    private var activeRun: ActiveRun? = null
    private lateinit var probe: MediaProbe
    private lateinit var jobStore: JobStore

    /** 用户在通知栏点了「停止服务」;当前任务跑完即收工。 */
    @Volatile
    private var stopRequested = false

    /** API 34 以下 mediaProcessing 前台服务没有系统自动持有的 wake lock,手动补上。 */
    private var wakeLock: android.os.PowerManager.WakeLock? = null

    private val resolver get() = applicationContext.contentResolver
    private val mainHandler = Handler(Looper.getMainLooper())

    override fun onCreate() {
        super.onCreate()
        probe = MediaProbe(applicationContext)
        jobStore = JobStore.get(applicationContext)
        Notifications.ensureChannel(this)
        AppLog.i(TAG, "service create")
        // 长队列/启动慢时尽早进前台,避免被系统判定为后台服务(5s 内必须 startForeground)。
        // R10-10:失败意味着没法以前台身份跑(函数内部已 stopSelf),此时**不**继续:
        // 旧代码无条件往下拿 wakelock + 启动 runQueue,于是要么以非前台身份开转码,
        // 要么马上走进销毁路径把本批已建的空文档当作残file删掉、并留下取消记录,
        // 用户看到的是「点了开始就没了」。回滚由 Service 自己做:enqueueStart 走
        // startForegroundService,它本身不抛,ViewModel 的 K-04 回滚不会跑到。
        if (!promoteToForeground(getString(R.string.job_state_queued), 0)) {
            rollbackQueuedJobs()
            return
        }
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            wakeLock = (getSystemService(POWER_SERVICE) as android.os.PowerManager)
                .newWakeLock(android.os.PowerManager.PARTIAL_WAKE_LOCK, "proresforge:transcode")
                .apply {
                    setReferenceCounted(false)
                    // 上限兜底,与 Android 15 mediaProcessing 6 小时时限一致
                    acquire(6 * 60 * 60 * 1000L)
                }
        }
        serviceScope.launch { runQueue() }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_CANCEL_CURRENT -> JobBus.currentId.value?.let { JobBus.cancel(it) }
            ACTION_STOP_SERVICE -> {
                // 只置停止标志并取消当前任务:跑完当前这条(照常落历史)后,
                // runQueue 循环看到 stopRequested 自然退出并收工
                stopRequested = true
                JobBus.currentId.value?.let { JobBus.cancel(it) }
            }
        }
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        // 先请 native 停下来再取消协程:nativeRun 是阻塞 JNI 调用,协程取消中断不了它。
        // 顺序反了的话 FFmpeg 仍在跑,但 JobBus.finish / JobStore / 通知都不会发生,
        // 前面的 wakelock 也被提前释放。
        val interruptedId = JobBus.currentId.value
        val running = activeRun
        val finalizeId = interruptedId ?: running?.id
        if (finalizeId != null) {
            // nativeCancel 只是置原子取消标记,不等待 nativeRun 收尾;异常时也不能阻断
            // Service 生命周期收尾(与 JobBus.cancel 的容错语义一致)。
            runCatching { FFmpeg.nativeCancel() }
                .onFailure { AppLog.w(TAG, "nativeCancel 失败", it) }
        }
        serviceScope.cancel()
        // nativeRun 是阻塞 JNI，必须等它真正返回后才能删除输出；否则 native 仍可能
        // 持有 fd/写 SAF 文档。输出删除由 finalizeScope 等完成闸门后执行;历史写入
        // 也投在 finalizeScope(K-02)。这里只在「nativeRun 还没返回」时才于主线程
        // 有界等待(K-06):任务已到终态的销毁路径最多差一次落库/删输出,主线程
        // 干等 2 秒只有 ANR 风险没有收益。
        if (finalizeId != null) {
            finalizeInterrupted(finalizeId, running, nativeActive = interruptedId != null)
        }
        // R10-9:runQueue 被打断时它自己的队尾补录(见 runQueue 末尾)跑不到;已经
        // finish 的任务可能没有历史,还没 begin 的 QUEUED 任务则留着入队时建的
        // 0 字节文档。这里统一兜底(细节见 finalizeRemainingJobs)。
        finalizeRemainingJobs()
        clearPendingCancel()
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        AppLog.i(TAG, "service destroy")
        stopForeground(STOP_FOREGROUND_REMOVE)
        super.onDestroy()
    }

    // ------------------------------------------------------------------ 队列驱动

    /** 串行消费队列:一个跑完再下一个;队列空则收工。 */
    private suspend fun runQueue() {
        // 已经写过历史的 id(排队期被取消的任务没有 finish() 路径,收尾时补写)
        val recorded = mutableSetOf<String>()
        while (serviceScope.isActive) {
            if (stopRequested) break
            val nextId = JobBus.jobs.value.entries
                .firstOrNull { it.value.state == JobState.QUEUED }
                ?.key
            if (nextId == null) break
            val spec = JobBus.jobs.value[nextId]?.spec ?: continue
            if (!JobBus.begin(nextId)) continue // 已被取消/移除,跳过
            // K-01:上一任务的取消可能落在「nativeRun 已返回但 finish/落库未完」的窗口,
            // JobBus.cancel 里的 nativeCancel 会因转码不在跑而把请求留在 pending;
            // 不在这里清掉,本任务开局就会被残留 pending 取消(RC_CANCELLED),
            // 然后被误标 CANCELLED 并删掉输出 —— 级联失败。
            clearPendingCancel()
            AppLog.i(TAG, "job begin: ${spec.inputName} → ${spec.outputName} (${spec.format})")

            val run = ActiveRun(nextId)
            activeRun = run
            try {
                val outcome = runJob(nextId, spec, run)
                // finish() 带终态守卫:运行期被取消时保持 CANCELLED,不会被 outcome.state 覆盖
                JobBus.finish(nextId, outcome.state, outcome.error, outcome.outSize, outcome.durationMs, outcome.inSize)
                // 通知/历史按 JobBus 里的最终状态发,而不是 outcome —— 用户点了取消却收到
                // 「完成」通知(进度对不上)就是这么来的。
                val finalRec = JobBus.jobs.value[nextId]
                val finalState = finalRec?.state ?: outcome.state
                AppLog.i(
                    TAG,
                    "job end: ${spec.inputName} state=$finalState dur=${outcome.durationMs}ms " +
                        "in=${outcome.inSize} out=${outcome.outSize} err=${outcome.error ?: "-"}"
                )

                // 历史:完成/失败/取消都写(JobStore 内部按 id 覆盖、上限 200 条)。
                // K-02/P2-06:落库 + 终端通知包进 NonCancellable —— serviceScope 在
                // onDestroy 被 cancel 时,协程会在 recordFinish 的挂起点被打断,
                // 后面的「完成/失败」通知就永远发不出;历史/通知属于收尾语义,
                // 必须跑完而不是被取消截断。
                withContext(NonCancellable) {
                    if (finalRec != null && recorded.add(nextId)) jobStore.recordFinish(finalRec)

                    // 结束通知(用 NOTIF_ID + 1 起的会话级递增 id,错开进度通知且批量不互相覆盖)
                    when (finalState) {
                        JobState.DONE ->
                            Notifications.done(this@TranscodeService, terminalNotifId(nextId), spec.inputName, spec.outputUri)

                        JobState.FAILED ->
                            Notifications.failed(
                                this@TranscodeService,
                                terminalNotifId(nextId),
                                spec.inputName,
                                (finalRec?.error ?: outcome.error) ?: getString(R.string.err_unsupported)
                            )

                        else -> Unit // 取消不打扰用户
                    }
                }
            } finally {
                // 通常由 runJob 的 IO finally 完成;这里覆盖 runJob 未能进入 IO 的取消竞态。
                run.done.countDown()
                if (activeRun === run) activeRun = null
            }
        }
        // 排队期就被取消的任务从不进 runJob,补写一次历史,让「取消」在历史里留痕。
        // P2-06:这里也是被取消的 serviceScope 里的普通 suspend 调用 —— 不包
        // NonCancellable 的话,onDestroy 打断 runQueue 时这段补录会被静默跳过。
        withContext(NonCancellable) {
            JobBus.jobs.value.values.forEach { rec ->
                val terminal = rec.state == JobState.DONE ||
                    rec.state == JobState.FAILED ||
                    rec.state == JobState.CANCELLED
                if (terminal && recorded.add(rec.id)) jobStore.recordFinish(rec)
            }
        }
        finishQueue()
    }

    /** 队列跑完:撤掉前台通知并停止服务。 */
    private fun finishQueue() {
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    // ------------------------------------------------------------------ 单个任务

    /**
     * 跑一个任务([id] 必须已 begin)。
     *
     * 步骤:probe(失败走 R.string.err_probe)→ 开输入/输出 fd → buildSpec → nativeRun
     * → 按返回码映射状态 → 失败/取消时删除残 file;成功时取最终文件大小。
     *
     * K-02:任务体整体 NonCancellable —— nativeRun 是阻塞 JNI,协程取消打不断它;
     * 若不这样,onDestroy 取消 serviceScope 后,任务体在 nativeRun 返回后的挂起点
     * 炸出 CancellationException,状态映射/删残file/落库/通知全部被跳过。onDestroy
     * 已先 nativeCancel(),这里让任务体自然跑完(native 以 RC_CANCELLED 返回),
     * 终态照常落库。
     *
     * R10-9:[run.outcome] 在完成闸门倒数之前统一登记,且 [run.done] 无条件打开:
     * onDestroy 的收尾靠这个闸门等待 native 停止写输出,靠 outcome 判断「native
     * 是否已经成功返回」(返回过就不再判取消、不删成品)。
     */
    private suspend fun runJob(id: String, spec: JobSpec, run: ActiveRun): JobOutcome =
        withContext(NonCancellable + Dispatchers.IO) {
            try {
                val outcome = runJobInner(id, spec)
                // 顺序要紧:先登记结论再开闸门,销毁路径等闸门后读到的才是最终值。
                run.outcome = outcome
                outcome
            } finally {
                // 闸门必须无条件打开:否则 onDestroy 的有界等待(K-06)与
                // finalizeScope 的收尾都会卡住 native 停止之后的清理。
                run.done.countDown()
            }
        }

    /** [runJob] 的任务体(不含闸门/结论登记)。所有出口都返回终态 [JobOutcome]。 */
    private suspend fun runJobInner(id: String, spec: JobSpec): JobOutcome {
            val startedAt = SystemClock.elapsedRealtime()

            // 1) 探测输入(顺便验证 native 可用)
            val facts = try {
                probe.probe(spec.inputUri)
            } catch (t: Throwable) {
                AppLog.w(TAG, "probe 异常", t)
                null
            }
            if (facts == null) {
                // probe 早退:取消可能已把 native 的 pending 取消置上,清掉再走
                clearPendingCancel()
                deleteOutputDocument(spec.outputUri)
                return JobOutcome(
                    JobState.FAILED, getString(R.string.err_probe), 0L, elapsed(startedAt), 0L
                )
            }

            // 2) 输入/输出 fd(detachFd;native 拥有,Kotlin 不 close)
            val inPair = FFmpeg.writeFd(spec.inputUri, resolver)
            val outFd = FFmpeg.createOutFd(spec.outputUri, resolver) ?: -1
            if (inPair == null || outFd < 0) {
                // detachFd() 已转移所有权；nativeRun 未调用时必须在这里收回，
                // 否则输出创建失败会泄漏输入 fd（反过来也一样）。
                inPair?.first?.let(::closeDetachedFd)
                if (outFd >= 0) closeDetachedFd(outFd)
                clearPendingCancel()
                deleteOutputDocument(spec.outputUri)
                AppLog.w(TAG, "打不开输入/输出 fd: in=${inPair != null} out=$outFd")
                return JobOutcome(
                    JobState.FAILED, getString(R.string.err_fd), 0L, elapsed(startedAt), 0L
                )
            }
            // 输入长度(writeFd 已 stat;取不到时为 -1)
            val inSize = inPair.second.coerceAtLeast(0L)

            // 3) spec 串(契约 §3)。序列化失败不再「退回默认参数」:那会用默认
            //    尺寸/质量跑完整个转码,产出与用户选择不符的文件。直接失败。
            val specStr = try {
                FFmpeg.buildSpec(spec, facts)
            } catch (t: Throwable) {
                AppLog.e(TAG, "buildSpec 异常,任务失败", t)
                runCatching { closeDetachedFd(inPair.first) }
                if (outFd >= 0) closeDetachedFd(outFd)
                clearPendingCancel()
                deleteOutputDocument(spec.outputUri)
                return JobOutcome(
                    JobState.FAILED,
                    getString(R.string.err_spec, spec.format),
                    0L,
                    elapsed(startedAt),
                    inSize
                )
            }

            // 3.5) 取消可能在 nativeRun 之前就已到达(probe/fd/buildSpec 期间)。
            //   JobBus 里已经是 CANCELLED ⇒ 不必再跑,也别把 pending 留给下一个任务。
            if (JobBus.jobs.value[id]?.state == JobState.CANCELLED) {
                AppLog.i(TAG, "任务在 nativeRun 前已被取消,跳过")
                clearPendingCancel()
                runCatching { closeDetachedFd(inPair.first) }
                if (outFd >= 0) closeDetachedFd(outFd)
                deleteOutputDocument(spec.outputUri)
                return JobOutcome(
                    JobState.CANCELLED, null, 0L, elapsed(startedAt), inSize
                )
            }

            // 4) 跑。activeRun 的完成闸门必须在 nativeRun 返回/抛异常后才打开；
            // onDestroy 依此等待 native 停止写输出，再删除半成品，避免并发写删。
            // P2-02:异常按类型分流成可读文案(库缺失 ≠ 内存不足 ≠ 权限),
            // 兜底 Throwable 才落到「未知错误」;各分支收尾同构 —— native 从没
            // 拿到 fd,所有权还在我们手上,收回成对的两个 fd(否则每次失败
            // 泄漏 2 个 fd)、清 pending 取消、删掉 0 字节输出文档,再早退
            // (与上面 buildSpec 失败路径同构;runQueue 会对早退结果补 finish)。
            val rc: Int? = try {
                FFmpeg.nativeRun(inPair.first, inPair.second, outFd, specStr, makeProgress(id, spec))
            } catch (e: UnsatisfiedLinkError) {
                // .so 缺失或 JNI 符号不匹配(装包不完整/ABI 没编进去)
                AppLog.e(TAG, "Native 库未加载: ${e.message}", e)
                runCatching { closeDetachedFd(inPair.first) }
                runCatching { closeDetachedFd(outFd) }
                clearPendingCancel()
                deleteOutputDocument(spec.outputUri)
                return JobOutcome(
                    JobState.FAILED, getString(R.string.err_native_library),
                    0L, elapsed(startedAt), inSize
                )
            } catch (e: OutOfMemoryError) {
                // 大文件/高分辨率下 JVM 或 native 分配失败
                AppLog.e(TAG, "内存不足: ${e.message}", e)
                runCatching { closeDetachedFd(inPair.first) }
                runCatching { closeDetachedFd(outFd) }
                clearPendingCancel()
                deleteOutputDocument(spec.outputUri)
                return JobOutcome(
                    JobState.FAILED, getString(R.string.err_out_of_memory),
                    0L, elapsed(startedAt), inSize
                )
            } catch (e: SecurityException) {
                // SAF/SELinux 拒绝(fd 已开出,但 native 侧访问被拒等)
                AppLog.e(TAG, "权限不足: ${e.message}", e)
                runCatching { closeDetachedFd(inPair.first) }
                runCatching { closeDetachedFd(outFd) }
                clearPendingCancel()
                deleteOutputDocument(spec.outputUri)
                return JobOutcome(
                    JobState.FAILED, getString(R.string.err_security),
                    0L, elapsed(startedAt), inSize
                )
            } catch (t: Throwable) {
                // 兜底:其它未预期异常,带上类型名便于排查
                AppLog.e(TAG, "nativeRun 未知异常: ${t.javaClass.simpleName} - ${t.message}", t)
                runCatching { closeDetachedFd(inPair.first) }
                runCatching { closeDetachedFd(outFd) }
                clearPendingCancel()
                deleteOutputDocument(spec.outputUri)
                return JobOutcome(
                    JobState.FAILED, getString(R.string.err_unknown, t.javaClass.simpleName),
                    0L, elapsed(startedAt), inSize
                )
            }
            AppLog.i(TAG, "nativeRun rc=$rc")
            // M-新A:nativeRun 返回后 end_transcode 已清掉 active 边界;若用户恰在
            // 「stat 输出大小」这个窗口点取消,JobBus.cancel 的 nativeCancel 会因
            // !active 把请求置成 pending。此处不清,下一任务 begin_transcode 会
            // 消费它 → 开局即取消被误标 CANCELLED。clear 只对「转码未在跑」生效,
            // 运行期取消不受影响,统一补一次最安全。
            clearPendingCancel()

            // 5) 状态映射(契约 §2 返回码:0 成功 / 1 取消 / 2 输出不支持 /
            //    3 输入损坏或无法解码 / <0 错误码取反)
            val state: JobState
            val error: String?
            when (rc) {
                FFmpeg.RC_OK -> {
                    state = JobState.DONE
                    error = null
                }

                FFmpeg.RC_CANCELLED -> {
                    state = JobState.CANCELLED
                    error = null
                }

                FFmpeg.RC_UNSUPPORTED -> {
                    state = JobState.FAILED
                    error = getString(R.string.err_unsupported)
                }

                FFmpeg.RC_INVALID_INPUT -> {
                    state = JobState.FAILED
                    error = getString(R.string.err_invalid_input)
                }

                null -> {
                    state = JobState.FAILED
                    error = getString(R.string.err_native_call)
                }

                else -> {
                    state = JobState.FAILED
                    error = getString(R.string.err_ffmpeg, -rc)
                }
            }

            // 5.5) R10-9:native 已返回、状态已映射 —— 在 NonCancellable 块内立刻把
            //   终态交给 JobBus。不能等到 runQueue 里再 finish:serviceScope 在
            //   onDestroy 被 cancel 时,runJob 返回后的第一个挂起点就抛
            //   CancellationException,runQueue 的 finish/落库/通知全部被跳过,
            //   销毁路径便把「native 已返回 0、成品已写完」的任务记成取消并删掉。
            //   finish() 自带终态守卫:运行期用户取消已把记录写成 CANCELLED 时,
            //   这里不会改回 DONE —— 沿用取消并删输出(M-新A 的窗口同理)。
            JobBus.finish(id, state, error, 0L, elapsed(startedAt), inSize)

            // 6) 失败/取消:删掉半截文件;成功:取最终大小。
            //   以 JobBus 里的最终状态为准(可能已被取消盖掉),保证「删/不删」
            //   与落库状态一致。
            val finalState = JobBus.jobs.value[id]?.state ?: state
            val outSize = if (finalState == JobState.DONE) {
                // finish() 的守卫挡住第二次写入,文件大小走单独通道回填
                try {
                    resolver.openFileDescriptor(spec.outputUri, "r")?.use { it.statSize } ?: 0L
                } catch (e: Exception) {
                    0L
                }
            } else {
                try {
                    DocumentFile.fromSingleUri(applicationContext, spec.outputUri)?.delete()
                } catch (e: Exception) {
                    false
                }
                0L
            }
            if (finalState == JobState.DONE && outSize > 0L) JobBus.updateOutSize(id, outSize)

            return JobOutcome(finalState, error, outSize, elapsed(startedAt), inSize)
        }

    /** outputUri 是入队前创建的文档;native 未启动时也要清理空文档。 */
    private fun deleteOutputDocument(uri: Uri) {
        runCatching {
            DocumentFile.fromSingleUri(applicationContext, uri)?.delete()
        }
    }

    /** 作废 native 侧未消费的 pending 取消(失败不致命,只影响下一次转码的取消语义)。 */
    private fun clearPendingCancel() {
        runCatching { FFmpeg.clearPendingCancel() }
            .onFailure { AppLog.w(TAG, "clearPendingCancel 失败", it) }
    }

    /**
     * onDestroy 时补齐当前任务的终态。历史先投递到独立 IO 作用域；输出删除必须等待
     * nativeRun 的完成闸门，避免 native 仍持有 fd 时发生写删竞态。
     * 即使 cancel() 已先把 JobBus 改成 CANCELLED，也不能因此跳过历史收尾。
     *
     * @param nativeActive currentId 在销毁时尚未清空(= finish() 还没跑,nativeRun
     *   可能仍在执行)。只有这种情况才在主线程有界等待收尾完成;任务已终态时收尾
     *   只剩落库/删输出,全部由 finalizeScope 完成,主线程不再等待(K-06)。
     *
     * R10-9:顺序是「等闸门 → 读 outcome → 才决定状态与删不删」。旧代码在 await 之前
     * 先写 CANCELLED 当保险，于是「native 已返回 0、成品已写完、runQueue 的 finish
     * 还没执行」这个窗口被销毁时，成品被记成取消并删掉。outcome 非空表示 native 已经
     * 跑完并有结论(见 [ActiveRun.outcome])，必须按它的结论收场。
     */
    private fun finalizeInterrupted(id: String, run: ActiveRun?, nativeActive: Boolean) {
        val rec = runCatching { JobBus.jobs.value[id] }.getOrNull() ?: return
        val outputUri = rec.spec.outputUri
        val finalized = CountDownLatch(1)

        // recordFinish 没有同步 API；独立 scope 不随 serviceScope.cancel() 取消。
        finalizeScope.launch {
            try {
                // 先等 native 停止写输出，再读 native 的结论 —— 顺序反了就会把
                // 已成功的任务判成取消。
                run?.done?.await()
                val cur = JobBus.jobs.value[id] ?: rec
                val decision = resolveInterruptedOutcome(cur, run?.outcome?.state)
                // 守卫保证:用户取消已写下的 CANCELLED 不会被 outcome 覆盖回去;
                // 已 DONE 的记录也不会被改判取消。
                val dur = cur.durationMs.takeIf { it > 0L }
                    ?: (System.currentTimeMillis() - rec.startedAt).coerceAtLeast(0L)
                JobBus.finish(id, decision.state, cur.error, cur.outSize, dur, cur.inSize)
                val finalRec = JobBus.jobs.value[id] ?: rec
                runCatching { jobStore.recordFinish(finalRec) }
                    .onFailure { AppLog.w(TAG, "interrupted job 落库失败: ${it.message}") }
                if (decision.deleteOutput) deleteOutputDocument(outputUri)
                AppLog.i(
                    TAG,
                    "job interrupted by service destroy: ${finalRec.spec.inputName} -> ${decision.state}"
                )
            } finally {
                finalized.countDown()
            }
        }

        // onDestroy 不能无限等待；超时后 IO finalizer 仍会等待闸门并完成删除。
        // K-06:native 已停(任务已终态)时跳过这个等待,消除主线程 ANR 风险。
        if (nativeActive) {
            runCatching { finalized.await(NATIVE_STOP_WAIT_MS, TimeUnit.MILLISECONDS) }
        }
    }

    /**
     * R10-9:runQueue 被打断时,它自己的队尾补录(见 runQueue 末尾)跑不到 —— 已 finish
     * 的任务可能没有历史,还没 begin 的 QUEUED 任务则留着入队时建的 0 字节文档。
     * 统一兜底:已终态 → 补落库(recordFinish 按 id 覆盖,重复写无害,状态取自 JobBus);
     * 仍 QUEUED → 删输出文档 + 移出队列(与 ViewModel 的 K-04 回滚同义:这种任务
     * 从没开跑,不该留下空文件,也不该伪造一条取消记录冒充跑过)。
     */
    private fun finalizeRemainingJobs() {
        val snapshot = JobBus.jobs.value
        if (snapshot.isEmpty()) return
        val needsRecord = ArrayList<JobRecord>()
        val queuedRecs = ArrayList<JobRecord>()
        for (rec in snapshot.values) {
            when {
                rec.state.isTerminalState() -> needsRecord += rec
                rec.state == JobState.QUEUED -> queuedRecs += rec
            }
        }
        if (needsRecord.isEmpty() && queuedRecs.isEmpty()) return
        if (queuedRecs.isNotEmpty()) JobBus.removeQueued(queuedRecs.map { it.id })
        val all = needsRecord + queuedRecs
        finalizeScope.launch {
            for (rec in all) {
                runCatching { jobStore.recordFinish(rec) }
                    .onFailure { AppLog.w(TAG, "destroy 兜底落库失败: ${it.message}") }
                // 未开跑的任务不留 0 字节文档(与 ViewModel 的 K-04 回滚同义)
                if (rec.state == JobState.QUEUED) deleteOutputDocument(rec.spec.outputUri)
            }
        }
    }

    /**
     * R10-10:onCreate 里 startForeground 失败(函数内部已 stopSelf)时的回滚。
     * enqueueStart 走 startForegroundService 且本身不抛,ViewModel 的 K-04 回滚
     * 不会跑,所以由 Service 自己删掉已入队任务的 0 字节文档并移出队列。
     */
    private fun rollbackQueuedJobs() {
        val snapshot = JobBus.jobs.value
        if (snapshot.isEmpty()) return
        val specs = ArrayList<JobSpec>()
        val queuedIds = ArrayList<String>()
        for (rec in snapshot.values) {
            if (rec.state == JobState.QUEUED) {
                specs += rec.spec
                queuedIds += rec.id
            }
        }
        if (queuedIds.isNotEmpty()) JobBus.removeQueued(queuedIds)
        AppLog.w(TAG, "startForeground 失败,回滚 ${specs.size} 个已入队任务")
        finalizeScope.launch {
            for (spec in specs) deleteOutputDocument(spec.outputUri)
        }
    }

    /** 进度回调:native 线程 → 切主线程更新 JobBus + 节流刷新通知(1s / 1%)。 */
    private fun makeProgress(id: String, spec: JobSpec): FFmpeg.Progress {
        val cancelPI = cancelPendingIntent()
        return object : FFmpeg.Progress {
            private var lastPct = -1
            private var lastStage = ""
            private var lastAt = 0L
            private var maxPct = 0

            override fun onProgress(pct: Int, stage: String, frame: Long, totalFrames: Long) {
                val now = SystemClock.elapsedRealtime()
                // 终态封顶:decode/audio 也会报 100(早于收尾写盘),只有 done 允许满格,
                // 否则进度条会在 moov 等收尾工作完成前就显示 100%。
                val shown = if (stage == "done") pct.coerceIn(0, 100) else pct.coerceIn(0, 99)
                val worthIt = stage != lastStage ||
                    shown >= 100 ||
                    (lastPct >= 0 && shown - lastPct >= 1 && now - lastAt >= THROTTLE_MS)
                if (!worthIt) return
                lastPct = shown
                lastStage = stage
                lastAt = now
                // 单调化:audio/finalize 是固定占位值(-1)或回退信号,decode 曲线与它们交错,
                // 直接透传会让进度条来回跳;这里只进不退。
                maxPct = maxOf(maxPct, shown)
                val c = maxPct
                mainHandler.post {
                    if (!serviceScope.isActive) return@post
                    JobBus.progress(id, c, stage, frame, totalFrames)
                    Notifications.progress(
                        this@TranscodeService,
                        NOTIF_ID,
                        spec.inputName,
                        getString(R.string.notif_running, c),
                        c,
                        cancelPI
                    )
                }
            }
        }
    }

    // ------------------------------------------------------------------ 前台通知

    /**
     * 立刻把服务提到前台(初始/无进度状态),API 29+ 带 mediaProcessing 类型。
     * @return true = 已进前台,调用方可以继续跑队列;false = 失败(内部已 stopSelf,
     *   调用方必须中止后续流程,见 onCreate 的 R10-10 分支)。
     */
    private fun promoteToForeground(text: String, pct: Int): Boolean {
        val n = buildForegroundNotification(text, pct)
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROCESSING)
            } else {
                startForeground(NOTIF_ID, n)
            }
            true
        } catch (e: Exception) {
            // K-03:Android 15 的 mediaProcessing 前台服务有 6h/日配额,超额时
            // startForeground 抛 ForegroundServiceStartNotAllowedException;
            // 个别 ROM 还会以 SecurityException/IllegalStateException 拒绝。这里不
            // 捕获就是进程 FATAL(服务一启动就崩)。捕获后停掉服务:转码没法以前台
            // 身份继续,但至少不崩,用户重开 App 或隔天配额重置后再试。
            // R10-10:返回 false 让 onCreate 中止 —— 否则它会继续拿 wakelock 并启动
            // runQueue,要么以非前台身份开转码,要么马上走进销毁路径把本批空文档删掉、
            // 留一条取消记录,用户看到的是「点了开始就没了」。
            AppLog.e(TAG, "startForeground 失败,停止服务", e)
            stopSelf()
            false
        }
    }

    private fun buildForegroundNotification(text: String, pct: Int): Notification =
        Notifications.progress(
            this,
            NOTIF_ID,
            getString(R.string.app_name),
            text,
            pct,
            cancelPendingIntent()
        )

    /** 通知栏「取消当前任务」的 PendingIntent(PendingIntent 经通知栏启动服务不受后台启动限制)。 */
    private fun cancelPendingIntent(): PendingIntent {
        val intent = Intent(this, TranscodeService::class.java).setAction(ACTION_CANCEL_CURRENT)
        return PendingIntent.getService(
            this,
            REQ_CANCEL_CURRENT,
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
    }

    /** 关闭尚未交给 native 的 detachFd，避免异常路径泄漏。 */
    private fun closeDetachedFd(fd: Int) {
        runCatching { android.os.ParcelFileDescriptor.adoptFd(fd).close() }
    }

    private fun elapsed(startedAt: Long): Long = SystemClock.elapsedRealtime() - startedAt

    companion object {
        private const val TAG = "TranscodeService"

        /** 前台进度通知 id。 */
        const val NOTIF_ID = 1001

        /**
         * 终态通知 id(完成/失败):会话内单调递增分配,同一 jobId 复用其首次分配值。
         *
         * 为什么不用固定 id:批量(一次多选 N 个文件)时任务一个接一个结束,固定 id 下
         * 「第 N 个的完成通知」会覆盖前 N-1 条,通知栏只剩最后一条(P2-03)。
         * 为什么不按 jobId 哈希派生:哈希取值域只有 4096 个,批量重转约 200 条历史时
         * 按生日界约 5% 概率两条完成通知撞同一 id 互相覆盖(连 PendingIntent 的
         * requestCode 也被改写)。改为会话级计数器后 id 天然互不碰撞;同一 jobId 只
         * 终态一次(DONE/FAILED 互斥),map 只防「重试同 id 的极窗口」,规模有界
         * (上限 = 本会话任务数,清掉旧映射即可防溢出)。
         * 取值域从 NOTIF_ID+1 起,不与进度通知 1001 冲突。
         */
        private val terminalNotifIds = ConcurrentHashMap<String, Int>()
        private val terminalNotifSeq = AtomicInteger(0)

        private fun terminalNotifId(jobId: String): Int =
            terminalNotifIds.computeIfAbsent(jobId) { NOTIF_ID + 1 + terminalNotifSeq.incrementAndGet() }

        private const val REQ_CANCEL_CURRENT = 2001

        /** 销毁时等待 nativeRun 退出的有界时长；超时后由 finalizeScope 延迟收尾。 */
        private const val NATIVE_STOP_WAIT_MS = 2000L

        /** 通知节流:至少间隔 1s 且进度至少 1%。 */
        private const val THROTTLE_MS = 1000L

        private const val ACTION_CANCEL_CURRENT = "com.proresforge.app.action.CANCEL_CURRENT"
        private const val ACTION_STOP_SERVICE = "com.proresforge.app.action.STOP_SERVICE"

    /** UI/外部入口:确保服务在跑(前台启动)。 */
    fun enqueueStart(context: Context) {
        ContextCompat.startForegroundService(
            context.applicationContext,
            Intent(context.applicationContext, TranscodeService::class.java)
        )
    }
    // 说明:曾有一个 enqueueStop(后台 startService 发 ACTION_STOP_SERVICE)但没有
    // 任何 UI 入口(K-10 死代码),已删除;通知栏的「停止」走 ACTION_STOP_SERVICE
    // 由 onStartCommand 处理,队列跑完由 finishQueue 收工。
}
}
