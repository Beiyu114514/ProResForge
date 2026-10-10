// HomeViewModel.kt — 转码页状态持有者(activity scope):文件选择与探测、LUT 暂存/强度/
// 输出目录管理、生成 JobSpec 并入队 TranscodeService;退出时把设置写回 SettingsStore。
// 依赖契约 §8.2 的 data/engine API(签名逐字匹配,未改名)。
package com.proresforge.app.ui

import android.app.Application
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.proresforge.app.R
import com.proresforge.app.data.FormatCatalog
import com.proresforge.app.data.LutManager
import com.proresforge.app.data.MediaProbe
import com.proresforge.app.data.OutputDir
import com.proresforge.app.data.SettingsStore
import com.proresforge.app.data.StagedLut
import com.proresforge.app.data.AudioMode
import com.proresforge.app.data.DngColorMode
import com.proresforge.app.data.DngQuality
import com.proresforge.app.data.DngMap
import com.proresforge.app.data.FpsMode
import com.proresforge.app.data.JobSpec
import com.proresforge.app.data.MediaFacts
import com.proresforge.app.data.OutFormat
import com.proresforge.app.data.QualityTier
import com.proresforge.app.data.ScaleMode
import com.proresforge.app.engine.AppLog
import com.proresforge.app.engine.JobBus
import com.proresforge.app.engine.TranscodeService
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

class HomeViewModel(app: Application) : AndroidViewModel(app) {

    /** 已选中的一个输入文件;facts 为 null 且 probed=false 表示还在探测,probed=true 表示探测已结束(含失败)。 */
    data class PickedFile(
        val uri: Uri,
        val name: String,
        val size: Long,
        val facts: MediaFacts?,
        val probed: Boolean = false,
    )

    /** 转码页完整 UI 状态。 */
    data class HomeUiState(
        val files: List<PickedFile> = emptyList(),
        val format: OutFormat = OutFormat.H264,
        val tier: QualityTier = QualityTier.HIGH,
        val scale: ScaleMode = ScaleMode.ORIG,
        val fps: FpsMode = FpsMode.ORIG,
        val audio: AudioMode = AudioMode.COPY,
        val tenBit: Boolean = false,
        val faststart: Boolean = true,
        val lut: StagedLut? = null,
        val lutIntensity: Float = 1f,
        /** DNG 序列 zip:解raw 档位(契约 §10.6)。 */
        val dngQuality: DngQuality = DngQuality.BALANCED,
        /** DNG 序列 zip:白平衡策略(契约 §10.6)。 */
        val dngColor: DngColorMode = DngColorMode.CAMERA,
        val dngMap: DngMap = DngMap.NONE,
        val outputDirName: String? = null,
        val busy: Boolean = false,
        val message: String? = null,
        /**
         * 每条消息的自增序号(配合 [message] 使用)。HomeScreen 的 Snackbar 用
         * `LaunchedEffect(state.messageSeq)` 驱动;若只靠 message 文案,4 秒内第二次
         * 弹同文案会被 Compose 判为「值没变」而丢掉。
         */
        val messageSeq: Long = 0L,
        /**
         * U-11:本条 [message] 是否来自「入队成功」。为 true 时 HomeScreen 的 Snackbar
         * 会带一个「查看任务」动作(入队后 Home 不再忙,提示一次即可跳任务页)。
         */
        val queuedHint: Boolean = false,
    ) {
        /**
         * 「开始转码」是否可点:
         * - 探测中的文件(probed=false)不能开始 —— 上一版把「探完但失败」当成唯一
         *   禁点条件,于是 batch 还在探测时就放行,而 facts 尚未到位;
         * - 探测已结束(probed=true)却拿不到 facts 的文件必然转码失败,禁掉按钮,
         *   比入队后再报错更早、更可操作。
         */
        val canStart: Boolean
            get() = files.isNotEmpty() &&
                files.all { it.probed && it.facts != null } &&
                outputDirName != null
    }

    private val context: Application = getApplication()

    private val mediaProbe = MediaProbe(context)
    private val lutManager = LutManager(context)
    private val outputDir = OutputDir(context)
    private val settings = SettingsStore(context)

    private val _state = MutableStateFlow(
        HomeUiState(
            files = emptyList(),
            format = settings.format,
            tier = settings.tier,
            scale = settings.scale,
            fps = settings.fps,
            audio = settings.audio,
            // 10bit 仅在当前格式支持时恢复,否则强制关闭
            tenBit = settings.tenBit && FormatCatalog.spec(settings.format).supports10Bit,
            faststart = settings.faststart,
            lut = null,
            lutIntensity = settings.lutIntensity.coerceIn(0f, 1f),
            dngQuality = settings.dngQuality,
            dngColor = settings.dngColor,
            dngMap = settings.dngMap,
            outputDirName = outputDir.treeUri.value?.let(::treeDisplayName),
            busy = false,
            message = null,
        )
    )

    val state: StateFlow<HomeUiState> = _state.asStateFlow()

    /** Snackbar 序号,保证同文案也能再次弹出(配合 messageSeq);原子因为探测回调跑在 IO 线程。 */
    private val messageCounter = AtomicLong(0L)

    /** 发一条提示(自动带序号)。 */
    /** 发一条提示(自动带序号);[queued] = 入队成功提示(U-11:Snackbar 带「查看任务」动作)。 */
    private fun postMessage(text: String, queued: Boolean = false) {
        _state.update {
            it.copy(
                message = text,
                messageSeq = messageCounter.incrementAndGet(),
                queuedHint = queued,
            )
        }
    }

    init {
        // 输出目录(在其他入口/上次会话被改过)变化 → 同步展示名
        viewModelScope.launch {
            outputDir.treeUri.collect { uri ->
                val name = uri?.let(::treeDisplayName)
                _state.update { current ->
                    if (current.outputDirName == name) current
                    else current.copy(outputDirName = name)
                }
            }
        }
        // 静默恢复上次使用的 LUT(权限已持久化则重新暂存到 cacheDir/luts)
        settings.lastLutUri?.let { lastUri ->
            viewModelScope.launch(Dispatchers.IO) {
                val staged = runCatching { lutManager.stage(Uri.parse(lastUri)) }.getOrNull()
                if (staged != null) {
                    _state.update { it.copy(lut = staged) }
                } else {
                    settings.lastLutUri = null
                    settings.lastLutName = null
                }
            }
        }
    }

    // ---------- 文件选择 ----------

    /**
     * 探测批次的代记(monotonic):新一批到达时递增,过期批次不得再写回 facts/busy。
     * 上一版用 mapIndexed 的 idx 回写,批次被替换后 idx 指向别的文件;也没有把
     * 正在跑的探测协程取消,旧批次的结果会把新选择覆盖掉。
     */
    private val probeGeneration = AtomicLong(0L)

    /** 当前探测协程;新批次到来时先取消,避免两份协程交错写 state。 */
    private var probeJob: kotlinx.coroutines.Job? = null

    /** 只有 [gen] 仍是最新批次时才允许写回状态。 */
    private fun isCurrentProbe(gen: Long): Boolean = probeGeneration.get() == gen

    /**
     * 先把文件列表(名字/大小)立刻上屏,再逐个探测;探完一个更新一个,失败也落终态
     * (probed=true),UI 不再出现永远停在「解析中」的条目。
     * 不在可解封装白名单里的文件(如 MotionCam 的 .mcraw 原始容器)直接标记失败,
     * 不进 native 探测(它没有 demuxer,探测必然失败且白耗一次 fd)。
     *
     * K-07:每一处写 files/busy 的 lambda 里**再读一次** generation,过期就原样
     * 返回旧状态。update {} 本身不挂起,但 Dispatchers.IO 是多线程的,协程取消要到
     * 下一个挂起点才生效 —— 只在外层 isCurrentProbe() 检查不够:慢一批可能在
     * 「新批次已写入」之后才拿到执行权,把文件列表盖回旧值。
     */
    fun onFilesPicked(uris: List<Uri>) {
        if (uris.isEmpty()) return
        val generation = probeGeneration.incrementAndGet()
        probeJob?.cancel()
        viewModelScope.launch(Dispatchers.IO) {
            try {
                _state.update {
                    if (isCurrentProbe(generation)) it.copy(busy = true) else it
                }
                val picked = uris.map { uri ->
                    val (name, size) = queryNameSize(uri)
                    PickedFile(uri = uri, name = name, size = size, facts = null, probed = false)
                }
                // queryNameSize 期间用户又选了一批:这批已经过期,直接退出
                if (!isCurrentProbe(generation)) return@launch
                // busy 覆盖整批探测(名称查询 + 逐个 probe):上一版在这里就 busy=false,
                // 按钮在探测未完成时可点,而 canStart 又不看 probed,于是用户点下去时
                // facts 可能还没到位(甚至整批已被新选择取代)。
                _state.update {
                    if (isCurrentProbe(generation)) it.copy(files = picked) else it
                }
                postMessage(context.getString(R.string.msg_files_picked, picked.size))
                for (file in picked) {
                    if (!isCurrentProbe(generation)) return@launch
                    val ext = file.name.substringAfterLast('.', "").lowercase()
                    val facts = if (ext in PROBEABLE_EXTENSIONS) {
                        try {
                            mediaProbe.probe(file.uri)
                        } catch (t: Throwable) {
                            // 兜底日志:native 库缺失/加载失败等不该静默成「无法解析」
                            AppLog.e(TAG, "探测异常: ${file.name}", t)
                            null
                        }
                    } else {
                        AppLog.i(TAG, "跳过探测(不支持的封装): ${file.name}")
                        null
                    }
                    if (facts == null) {
                        AppLog.w(TAG, "探测失败: ${file.name}")
                    }
                    if (!isCurrentProbe(generation)) return@launch
                    _state.update { st ->
                        // K-07:lambda 内再确认一次仍是当前批次(上面的外层检查与
                        // 这次执行之间可能已被新批次取代)。
                        if (!isCurrentProbe(generation)) return@update st
                        // 按 uri 回写而不是按 idx:批次替换过列表后 mapIndexed 的 idx
                        // 已经指向别的文件,会把上一批的 facts 写到新文件上。
                        val target = st.files.firstOrNull { it.uri == file.uri }
                            ?: return@update st
                        st.copy(
                            files = st.files.map {
                                if (it.uri == file.uri) it.copy(facts = facts, probed = true) else it
                            }
                        )
                    }
                }
            } finally {
                // 整批探测完成/异常/被下一批取代都走这里;只有自己仍是当前批次才复位
                // busy(批次被取代时 busy 属于新批次)。K-07:finally 只清自己这一代的
                // busy,不改、也不恢复文件列表 —— 恢复会把新批次的列表盖掉。
                if (isCurrentProbe(generation)) {
                    _state.update {
                        if (isCurrentProbe(generation)) it.copy(busy = false) else it
                    }
                }
            }
        }.also { probeJob = it }
    }

    // ---------- 输出参数 ----------
    // 每个 setter 立即落盘:旧代码只在 onCleared() 统一写回,进程被系统杀掉即丢失本次调整。

    fun setFormat(f: OutFormat) {
        val supports10Bit = FormatCatalog.spec(f).supports10Bit
        val tenBit = _state.value.tenBit && supports10Bit
        _state.update { it.copy(format = f, tenBit = tenBit) }
        settings.format = f
        settings.tenBit = tenBit
    }

    fun setTier(t: QualityTier) {
        _state.update { it.copy(tier = t) }
        settings.tier = t
    }

    fun setScale(s: ScaleMode) {
        _state.update { it.copy(scale = s) }
        settings.scale = s
    }

    fun setFps(f: FpsMode) {
        _state.update { it.copy(fps = f) }
        settings.fps = f
    }

    fun setAudio(a: AudioMode) {
        _state.update { it.copy(audio = a) }
        settings.audio = a
    }

    fun setTenBit(v: Boolean) {
        _state.update { it.copy(tenBit = v) }
        settings.tenBit = v
    }

    fun setFaststart(v: Boolean) {
        _state.update { it.copy(faststart = v) }
        settings.faststart = v
    }

    // ---------- LUT ----------

    /** 暂存 LUT 到 cacheDir 并尝试持久化读取权限;成功后记录最近 LUT(强度保持不动)。 */
    fun onLutPicked(uri: Uri) {
        viewModelScope.launch(Dispatchers.IO) {
            _state.update { it.copy(busy = true) }
            try {
                context.contentResolver.takePersistableUriPermission(
                    uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION,
                )
            } catch (_: SecurityException) {
                // 个别提供者不允许持久化授权,忽略(本次会话仍可用)
            }
            val staged = runCatching { lutManager.stage(uri) }.getOrNull()
            if (staged != null) {
                AppLog.i(TAG, "LUT 暂存: ${staged.title} size=${staged.size}")
                _state.update { it.copy(busy = false, lut = staged) }
                postMessage(context.getString(R.string.msg_lut_loaded, staged.title ?: ""))
                settings.lastLutUri = uri.toString()
                settings.lastLutName = staged.title
            } else {
                AppLog.w(TAG, "LUT 解析失败")
                _state.update { it.copy(busy = false) }
                postMessage(context.getString(R.string.msg_lut_invalid))
            }
        }
    }

    fun clearLut() {
        _state.update { it.copy(lut = null) }
        settings.lastLutUri = null
        settings.lastLutName = null
    }

    fun setLutIntensity(v: Float) {
        val clamped = v.coerceIn(0f, 1f)
        _state.update { it.copy(lutIntensity = clamped) }
        // LUT 强度即时持久化
        settings.lutIntensity = clamped
    }

    /** DNG zip 解raw 档位(契约 §10.4)。 */
    fun setDngQuality(q: DngQuality) {
        _state.update { it.copy(dngQuality = q) }
        settings.dngQuality = q
    }

    /** DNG zip 白平衡策略。 */
    fun setDngColor(c: DngColorMode) {
        _state.update { it.copy(dngColor = c) }
        settings.dngColor = c
    }

    /** DNG zip 内置色彩映射。 */
    fun setDngMap(m: DngMap) {
        _state.update { it.copy(dngMap = m) }
        settings.dngMap = m
    }

    // ---------- 输出目录 ----------

    fun onOutputDirPicked(uri: Uri) {
        viewModelScope.launch(Dispatchers.IO) {
            // persist 内部已 takePersistableUriPermission
            outputDir.persist(uri)
            postMessage(context.getString(R.string.msg_output_dir_updated))
        }
    }

    /** OpenDocumentTree 的初始位置(已授权时为当前目录,否则指向真实存在的 Movies/ProResForge)。 */
    fun initialTreeUri(): Uri? = outputDir.initialTreeUri()

    // ---------- 预览 ----------

    /**
     * 第一个文件 40% 处抽原帧(未套 LUT)。suspend:真实调用链是 MediaProbe 的 suspend API,
     * 由调用方(UI 的 LaunchedEffect)在协程里 await;禁止在 ViewModel 内套 runBlocking。
     */
    suspend fun previewBefore(): ByteArray? = preview(graded = false, lutPath = null, intensity = 1.0)

    /** 第一个文件 40% 处抽帧并套当前 LUT(强度取 [intensity])。无 LUT 返回 null。 */
    suspend fun previewAfter(intensity: Float = _state.value.lutIntensity): ByteArray? {
        val lut = _state.value.lut ?: return null
        return preview(graded = true, lutPath = lut.path, intensity = intensity.toDouble())
    }

    private suspend fun preview(graded: Boolean, lutPath: String?, intensity: Double): ByteArray? {
        val first = _state.value.files.firstOrNull() ?: return null
        val facts = first.facts
        // DNG zip 没有时间轴:atUs 被 native 当「帧号」用(契约 §10.5),取中间那帧;
        // 普通文件仍按 40% 时长抽帧。
        val atUs = if (facts?.isDngZip == true) {
            val frames = if (facts.frames > 0) facts.frames else facts.nbFrames
            if (frames > 1) frames / 2 else 1L
        } else {
            val durationUs = facts?.durationUs ?: 0L
            if (durationUs > 0) durationUs * 2 / 5 else 0L
        }
        // N-7:预览解 DNG zip 的档位必须与转码 spec 一致,否则预览和成品画质对不上。
        return runCatching {
            mediaProbe.previewJpeg(
                first.uri, atUs, lutPath, intensity, graded, PREVIEW_MAX_W,
                _state.value.dngQuality.ordinal,
                _state.value.dngColor.ordinal,
                _state.value.dngMap.ordinal
            )
        }.getOrNull()
    }

    // ---------- 开始转码 ----------

    /** 一次性入队 guard:见 startTranscode()。 */
    private val enqueueGuard = AtomicBoolean(false)

    /** 从「重置 busy + guard」通道返回:各早退分支统一调用,避免漏一条。 */
    private fun MutableStateFlow<HomeUiState>.exitStart(
        msgRes: Int? = null,
        arg: Any? = null,
        queued: Boolean = false,
    ) {
        update { it.copy(busy = false) }
        if (msgRes != null) postMessage(context.getString(msgRes, arg), queued = queued)
    }

    /**
     * 入队前预检:返回错误文案资源 id,null = 可以开始。
     * 这些都是「必然失败」的条件,提前挡住并给可操作提示,而不是把整个队列跑完再报错。
     */
    private fun preflightError(snapshot: HomeUiState): Int? = when {
        // 未授权输出目录:outputUriFor 必然返回 null(旧行为是跑到队列里才报错)
        outputDir.treeUri.value == null -> R.string.err_no_output_dir
        // 已探测过且拿不到 facts 的文件(损坏/无 demuxer),nativeRun 也必然失败
        snapshot.files.any { it.probed && it.facts == null } -> R.string.err_unprobeable
        else -> null
    }

    /**
     * 从已选文件里移除一个(K-05):探测失败的文件此前会把整批卡死(canStart 要求
     * 全部 facts 就绪、文件列表又没有移除入口),给用户一个「只剔除坏文件」的出口,
     * 不用重新选一遍整批。探测中的批次照常按 uri 回写,被移除的文件自然跳过。
     */
    fun removeFile(uri: Uri) {
        _state.update { it.copy(files = it.files.filterNot { f -> f.uri == uri }) }
    }

    /** 为每个已选文件生成 JobSpec(outputUri 由 OutputDir 创建)并入队,启动前台服务。 */
    fun startTranscode() {
        val snapshot = _state.value
        if (snapshot.busy || !snapshot.canStart) return
        // busy 的「判定 + 置位」必须原子完成。旧代码把置位放在 IO 协程里,同一帧内
        // 连点两次都读到 busy=false → 同一批 specs 入队两遍(输出被追加 " (2)",
        // 时间与存储翻倍)。用 compareAndSet 关掉这个窗口。
        if (!enqueueGuard.compareAndSet(false, true)) return
        _state.update { it.copy(busy = true) }
        viewModelScope.launch(Dispatchers.IO) {
            // L-新B:本批已在 SAF 里建出的文档。两个失败退出点(建档失败/启动服务失败)
            // 都要回删,否则半批失败会留一排 0 字节文件。
            val createdDocs = ArrayList<OutputDir.OutputDoc>(snapshot.files.size)
            try {
                val err = preflightError(snapshot)
                if (err != null) {
                    _state.exitStart(err)
                    return@launch
                }
                // K-11:输出名整批算好、SAF 目录枚举只做一次(逐个 outputUriFor 是
                // N 次全量 children 枚举,且结果集随目录体积线性增长)。
                val names = snapshot.files.map { file ->
                    outputDir.fileName(
                        file.name.substringBeforeLast('.', file.name),
                        snapshot.format,
                    )
                }
                val docs = outputDir.outputDocsFor(names)
                val specs = ArrayList<JobSpec>(snapshot.files.size)
                for (i in snapshot.files.indices) {
                    val doc = docs[i]
                    if (doc == null) {
                        // R10-11:回删**返回列表里**每一个已建成的文档。失败即停之后
                        // 它与 createdDocs 前缀恰好等价,但不变量按「已创建」算,
                        // 不按「循环走到哪儿」算 —— 之前只回删前缀累加器,失败点
                        // 之后建成的 0 字节文档没人删,名字也留在 pendingNames。
                        outputDir.deleteCreated(docs.filterNotNull())
                        _state.exitStart(R.string.msg_out_create_failed, names[i])
                        return@launch
                    }
                    createdDocs += doc
                    val file = snapshot.files[i]
                    specs += JobSpec(
                        inputUri = file.uri,
                        inputName = file.name,
                        outputUri = doc.uri,
                        // 用 provider 侧最终采用的名字(个别 provider 会自己改名)
                        outputName = doc.name,
                        format = snapshot.format,
                        tier = snapshot.tier,
                        scale = snapshot.scale,
                        fps = snapshot.fps,
                        lutPath = snapshot.lut?.path,
                        lutName = snapshot.lut?.title,
                        lutIntensity = snapshot.lutIntensity,
                        audio = snapshot.audio,
                        tenBit = snapshot.tenBit,
                        faststart = snapshot.faststart,
                        dngQuality = snapshot.dngQuality,
                        dngColor = snapshot.dngColor,
                        dngMap = snapshot.dngMap,
                    )
                }
                // K-04:enqueue 返回 id,供启动服务失败时回滚(见下)
                val ids = JobBus.enqueue(specs)
                try {
                    TranscodeService.enqueueStart(context)
                } catch (t: Throwable) {
                    // 前台服务启动失败(后台启动限制 / Android 15 FGS 配额等):
                    // 任务会永久 QUEUED(JobBus 没有消费者)、SAF 里一排 0 字节文档
                    // —— 不回滚就是「点了开始没反应,存储还被吃掉」。
                    AppLog.e(TAG, "启动 TranscodeService 失败,回滚 ${ids.size} 个入队任务", t)
                    JobBus.removeQueued(ids)
                    outputDir.deleteCreated(createdDocs)
                    _state.exitStart(R.string.err_enqueue_failed)
                    return@launch
                }
                AppLog.i(TAG, "入队 ${specs.size} 个任务: ${specs.joinToString { it.outputName }}")
                // 输出文档已移交服务消费:此后外层异常路径不得再回删,清空登记。
                createdDocs.clear()
                // 入队成功后 busy 解除(转码在服务里跑,Home 不再忙);
                // 此处不清 files —— JobBus/jobs 有关闭逻辑,清掉会让文件列表空白。
                _state.exitStart(R.string.msg_queued, specs.size, queued = true)
            } catch (t: Throwable) {
                // 异常路径(busy=true 已在协程外置位)也要复位 busy,否则按钮永久禁用、
                // 界面一直显示「处理中」。M-5a。此处回删本批已建文档:走到这说明
                // enqueue/enqueueStart 没有成功接管,输出文档无人消费。
                AppLog.e(TAG, "入队失败", t)
                outputDir.deleteCreated(createdDocs)
                _state.exitStart(R.string.err_enqueue_failed)
            } finally {
                // 必须复位:否则 guard 永久停在 true,之后再也入队不了队(按钮看着能点)
                enqueueGuard.set(false)
            }
        }
    }

    /** Snackbar 展示完成后调用,清除一次性文案。 */
    fun consumeMessage() {
        _state.update { it.copy(message = null) }
    }

    override fun onCleared() {
        // 各 setter 已即时落盘,这里只是兜底再刷一次(幂等),防止将来新增字段忘记加即时写回。
        val s = _state.value
        settings.format = s.format
        settings.tier = s.tier
        settings.scale = s.scale
        settings.fps = s.fps
        settings.audio = s.audio
        settings.tenBit = s.tenBit
        settings.faststart = s.faststart
        settings.lutIntensity = s.lutIntensity
        settings.dngQuality = s.dngQuality
        settings.dngColor = s.dngColor
        settings.dngMap = s.dngMap
        super.onCleared()
    }

    // ---------- 内部工具 ----------

    /** 从 contentResolver 读展示名与大小(SAF OpenMultipleDocuments 授予的 uri)。 */
    private fun queryNameSize(uri: Uri): Pair<String, Long> {
        val fallbackName = uri.lastPathSegment?.substringAfterLast('/')
            ?: context.getString(R.string.fallback_file_name)
        try {
            context.contentResolver.query(
                uri,
                arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE),
                null,
                null,
                null,
            )?.use { cursor ->
                if (cursor.moveToFirst()) {
                    val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    val sizeIndex = cursor.getColumnIndex(OpenableColumns.SIZE)
                    val name = if (nameIndex >= 0) cursor.getString(nameIndex) else null
                    val size = if (sizeIndex >= 0 && !cursor.isNull(sizeIndex)) {
                        cursor.getLong(sizeIndex)
                    } else {
                        0L
                    }
                    return (name ?: fallbackName) to size
                }
            }
        } catch (_: Exception) {
            // 查询失败用降级值
        }
        return fallbackName to 0L
    }

    /** tree uri → 展示名,如 primary:Movies/ProResForge → ProResForge、primary:Movies → Movies。 */
    private fun treeDisplayName(uri: Uri): String =
        try {
            DocumentsContract.getTreeDocumentId(uri)
                .substringAfterLast('/')
                .substringAfterLast(':')
        } catch (_: Exception) {
            uri.lastPathSegment ?: ""
        }

    private companion object {
        private const val TAG = "HomeVM"

        /** 预览图最大边(缩略图,控制 nativePreview 成本)。 */
        const val PREVIEW_MAX_W = 640

        /** 值得进 native 探测的封装(FFmpeg 有 demuxer);之外的(.mcraw 等)直接判不可解析。 */
        val PROBEABLE_EXTENSIONS =
            setOf("mov", "mp4", "mxf", "m4v", "webm", "mkv", "avi", "zip")
    }
}
