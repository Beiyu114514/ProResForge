package com.proresforge.app.data

import android.content.Context
import com.proresforge.app.engine.AppLog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 历史记录持久化(契约 §8.2):filesDir/jobs.json,最多 [MAX_ITEMS] 条,新的在前。
 *
 * 说明:
 * - 复用 model.kt 的 [HistoryItem](已标注 @Serializable),不重复定义。
 * - 首次读取在 IO 协程里做(init 里触发,不阻塞调用线程);[recordFinish] 与首次加载
 *   用同一把 Mutex 串起来,避免「加载尚未完成就写入」把旧数据又盖回去。
 * - 写盘用临时文件 + rename,防止进程被杀留下半截 JSON。
 * - **坏档不清空**(S-3):解析失败时把原文件备份成 jobs.json.corrupt-<时间戳>,
 *   打日志后从空历史继续;不备份的话,下一次 recordFinish 会把空列表原子写回,
 *   用户历史连人工恢复的机会都没有(触发条件见 HistoryItem 的枚举改名说明)。
 * - **必须**用 [get] 拿进程内唯一实例:历史是进程共享状态,多实例各持一把 Mutex
 *   读同一文件会互相覆盖丢更新(UI 与 Service 各 new 一个就是这么丢的)。
 */
class JobStore internal constructor(private val file: File) {

    internal constructor(context: Context) :
        this(File(context.applicationContext.filesDir, "jobs.json"))

    private val json = Json {
        ignoreUnknownKeys = true   // 旧版本字段也能读
        encodeDefaults = true
    }
    private val listSerializer = ListSerializer(HistoryItem.serializer())

    private val mutex = Mutex()
    private var loaded = false

    private val _history = MutableStateFlow<List<HistoryItem>>(emptyList())

    /** 历史列表(新的在前);Service 写完立刻可见,UI 直接 collect。 */
    val history: StateFlow<List<HistoryItem>> = _history.asStateFlow()

    init {
        // 首加载异步进行:job 完成时会立刻拿到最新列表,UI 则在下一次 emit 时刷新
        JobStoreIoScope.scope.launch {
            mutex.withLock { loadLocked() }
        }
    }

    /** 完成/失败/取消都写;重跑同 id 覆盖(按 id 去重后插到最前)。 */
    suspend fun recordFinish(record: JobRecord): Unit = withContext(Dispatchers.IO) {
        val item = HistoryItem(
            id = record.id,
            ts = if (record.endedAt > 0L) record.endedAt else System.currentTimeMillis(),
            inputName = record.spec.inputName,
            outputName = record.spec.outputName,
            format = record.spec.format,
            state = record.state,
            error = record.error,
            lutName = record.spec.lutName,
            lutIntensity = record.spec.lutIntensity,
            inSize = record.inSize,
            outSize = record.outSize,
            durationMs = record.durationMs,
            outputUriString = record.spec.outputUri.toString(),
            frames = record.totalFrames,
            fps = record.spec.fps.fps
        )
        mutex.withLock {
            if (!loaded) loadLocked()
            val next = (listOf(item) + _history.value.filterNot { it.id == item.id })
                .take(MAX_ITEMS)
            if (writeLocked(next)) _history.value = next
        }
    }

    /** 清空历史。 */
    suspend fun clear(): Unit = withContext(Dispatchers.IO) {
        mutex.withLock {
            loaded = true
            if (writeLocked(emptyList())) _history.value = emptyList()
        }
    }

    // ------------------------------------------------------------------ 内部实现

    /** 读盘;调用方必须已持有 [mutex]。 */
    private fun loadLocked() {
        if (loaded) return
        loaded = true
        if (!file.isFile) return
        val parsed = try {
            json.decodeFromString(listSerializer, file.readText())
        } catch (e: Exception) {
            // S-3:文件损坏/旧格式不阻塞使用,但**不能静默** —— 备份原文件再从空
            // 历史开始,并打日志。否则下次 recordFinish 把空列表写回,坏档彻底消失。
            backupCorruptFile(e)
            emptyList()
        }
        _history.value = parsed.sortedByDescending { it.ts }.take(MAX_ITEMS)
    }

    /** 坏档备份:jobs.json → jobs.json.corrupt-<时间戳>;备份失败也只记日志,不抛。 */
    private fun backupCorruptFile(cause: Exception) {
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val backup = File(file.parentFile, "${file.name}.corrupt-$stamp")
        val copied = runCatching { file.copyTo(backup, overwrite = true).isFile }
            .getOrDefault(false)
        AppLog.w(
            TAG,
            "jobs.json 解析失败(${cause.javaClass.simpleName}: ${cause.message});" +
                (if (copied) "已备份到 ${backup.name}" else "备份失败(不阻塞使用)") +
                ",从空历史重建"
        )
    }

    /** 落盘;成功返回 true。调用方必须已持有 [mutex]。 */
    private fun writeLocked(list: List<HistoryItem>): Boolean = try {
        val tmp = File(file.parentFile, file.name + ".tmp")
        tmp.writeText(json.encodeToString(listSerializer, list))
        // rename 本身就是原子替换,不需要先 delete:先删反而把「删除成功、
        // rename 失败」变成真丢数据,窗口更大。Windows 的 File.renameTo 不允许
        // 覆盖已有目标,仅在原子替换失败且目标存在时做兼容回退。
        if (tmp.renameTo(file)) {
            true
        } else if (file.exists() && file.delete() && tmp.renameTo(file)) {
            true
        } else {
            tmp.delete()
            false
        }
    } catch (e: Exception) {
        false
    }

    companion object {
        private const val TAG = "JobStore"

        /** 历史上限(契约 §8.2)。 */
        const val MAX_ITEMS: Int = 200

        @Volatile
        private var instance: JobStore? = null

        /** 进程内唯一实例(应用 Context,不泄漏 Activity)。 */
        fun get(context: Context): JobStore =
            instance ?: synchronized(this) {
                instance ?: JobStore(context.applicationContext).also { instance = it }
            }
    }
}

/**
 * JobStore 首加载用的进程内共享 IO 作用域。
 * 只干一次小文件读,不随 JobStore 实例生命周期取消(实例可能被反复创建)。
 */
private object JobStoreIoScope {
    val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
}
