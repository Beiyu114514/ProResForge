// HomeScreen.kt — 转码主界面:LazyColumn 依次为文件选择、媒体信息、输出格式、质量、缩放/帧率、
// LUT(含前后对比预览)、音频、输出目录、开始按钮;状态全部来自 HomeViewModel.state。
package com.proresforge.app.ui

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.BitmapFactory
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.SnackbarDuration
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.SnackbarResult
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.proresforge.app.R
import com.proresforge.app.data.AudioMode
import com.proresforge.app.data.DngColorMode
import com.proresforge.app.data.DngQuality
import com.proresforge.app.data.DngMap
import com.proresforge.app.data.FormatCatalog
import com.proresforge.app.data.FpsMode
import com.proresforge.app.data.OutFormat
import com.proresforge.app.data.QualityTier
import com.proresforge.app.data.ScaleMode
import com.proresforge.app.ui.FactRow
import com.proresforge.app.ui.LabeledSlider
import com.proresforge.app.ui.SectionCard
import com.proresforge.app.ui.SegmentedRow
import com.proresforge.app.ui.formatDurationUs
import com.proresforge.app.ui.formatSize
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

@Composable
fun HomeScreen(modifier: Modifier = Modifier, onOpenJobs: () -> Unit = {}) {
    val vm: HomeViewModel = viewModel()
    val state by vm.state.collectAsStateWithLifecycle()

    // 多选视频:OpenMultipleDocuments 的 uri 本次可用(无需持久化授权)
    val pickFiles = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenMultipleDocuments(),
    ) { uris ->
        if (uris.isNotEmpty()) vm.onFilesPicked(uris)
    }
    // LUT 文件(权限在 VM.onLutPicked 中持久化)
    val pickLut = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument(),
    ) { uri ->
        uri?.let(vm::onLutPicked)
    }
    // 输出目录:初始指向 primary:Movies/ProResForge(权限在 OutputDir.persist 中持久化)
    val pickTree = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocumentTree(),
    ) { uri ->
        uri?.let(vm::onOutputDirPicked)
    }

    val snackbarHostState = remember { SnackbarHostState() }
    var advancedExpanded by rememberSaveable { mutableStateOf(false) }
    var showNotificationRationale by rememberSaveable { mutableStateOf(false) }
    var pendingStart by rememberSaveable { mutableStateOf(false) }
    val context = LocalContext.current

    val notificationPermissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) {
        if (pendingStart) {
            pendingStart = false
            vm.startTranscode()
        }
    }

    fun startWithoutNotificationPermission() {
        NotificationPermissionGate.asked = true
        pendingStart = false
        showNotificationRationale = false
        vm.startTranscode()
    }

    fun requestStart() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED &&
            !NotificationPermissionGate.asked
        ) {
            pendingStart = true
            showNotificationRationale = true
        } else {
            vm.startTranscode()
        }
    }

    // 用 messageSeq 做 key:同文案的第二次提示(如 4 秒内连点两次开始)也能再弹一次。
    val viewJobsLabel = stringResource(R.string.btn_view_jobs)
    LaunchedEffect(state.messageSeq) {
        val msg = state.message ?: return@LaunchedEffect
        val queued = state.queuedHint
        // N11-2:showSnackbar 会挂起到 Snackbar 消失为止;若等它返回后才 consumeMessage,
        // 中途离开本页(切 Jobs)取消协程,message 永远不会被消费,回来时 LaunchedEffect
        // 重启又弹同一条,反复横跳看起来就是「永不消失」。先消费再送出:key(messageSeq)
        // 不变,本协程不会被打断,Snackbar 展示不受影响。
        // duration 显式 Long:入队提示带动作,Short(4s)对可点击提示太短,Long(10s)内必自动消失。
        vm.consumeMessage()
        val result = if (queued) {
            snackbarHostState.showSnackbar(msg, actionLabel = viewJobsLabel, duration = SnackbarDuration.Long)
        } else {
            snackbarHostState.showSnackbar(msg, duration = SnackbarDuration.Long)
        }
        if (result == SnackbarResult.ActionPerformed) onOpenJobs()
    }

    // ---------- LUT 前后对比预览(第一个文件 40% 处;LUT/强度变化重取) ----------
    val firstFile = state.files.firstOrNull()
    val previewKey = (firstFile?.uri?.toString() ?: "") to state.lut?.path
    var beforeBitmap by remember(previewKey) { mutableStateOf<ImageBitmap?>(null) }
    var afterBitmap by remember(previewKey) { mutableStateOf<ImageBitmap?>(null) }
    // 强度变化只重取「后」图:seek+decode 是大头,前图不依赖强度,不必跟着拖滑块重跑。
    // K-08:滑条 steps=19(20 档),拖动时每档都会改 lutIntensity 并重启本协程;
    // 不去抖的话一次拖动就是十几次完整的 native 解码(缩放+LUT+JPEG 编码)。
    // 先 delay 再干活:连续拖动时旧协程在 delay 里被 key 变化取消,停顿 ≥250ms
    // 才真正进 native。
    LaunchedEffect(previewKey, state.lutIntensity, advancedExpanded) {
        // U-09:预览瓦片在「高级设置」里,折叠时没人看 —— 不再解 native 预览帧
        // (此前 Advanced 折叠着,选完文件照样解 4096×3072 的 DNG,白耗一次解码)。
        if (firstFile == null || !advancedExpanded) {
            return@LaunchedEffect
        }
        delay(PREVIEW_DEBOUNCE_MS)
        val intensity = state.lutIntensity
        if (beforeBitmap == null) {
            val pair = withContext(Dispatchers.IO) {
                vm.previewBefore() to vm.previewAfter(intensity)
            }
            beforeBitmap = pair.first.decodeToImage()
            afterBitmap = pair.second.decodeToImage()
        } else {
            val after = withContext(Dispatchers.IO) { vm.previewAfter(intensity) }
            afterBitmap = after.decodeToImage()
        }
    }

    Box(modifier.fillMaxSize()) {
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(start = 12.dp, end = 12.dp, top = 8.dp, bottom = 96.dp),
        ) {
            // ---------- 1. 文件选择 ----------
            item {
                SectionCard(title = stringResource(R.string.files_title)) {
                    Button(
                        onClick = {
                            // application/zip:DNG 序列 zip(MotionCam);octet-stream 兜底
                            pickFiles.launch(
                                arrayOf(
                                    "video/*",
                                    "application/zip",
                                    "application/x-zip-compressed",
                                    "application/octet-stream",
                                )
                            )
                        },
                    ) {
                        Text(stringResource(R.string.pick_files))
                    }
                    Spacer(Modifier.height(8.dp))
                    if (state.files.isEmpty()) {
                        Text(
                            text = stringResource(R.string.pick_files_hint),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }

            // U-10:文件清单改成 LazyColumn 的 items(每行一个 item、key = uri);
            // 此前整批文件挤在同一个 item 里 forEach,几十个文件会一次性组合、滚动也卡。
            items(state.files, key = { it.uri.toString() }) { file ->
                FileRowCard(
                    file = file,
                    busy = state.busy,
                    onRemove = { vm.removeFile(file.uri) },
                )
            }

            // ---------- 2. 媒体信息(第一个文件) ----------
            item {
                SectionCard(title = stringResource(R.string.media_title)) {
                    val facts = firstFile?.facts
                    if (facts == null) {
                        Text(
                            text = stringResource(R.string.media_empty),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    } else {
                        FactRow(
                            stringResource(R.string.fact_codec),
                            "${facts.codec} · ${facts.container}",
                        )
                        FactRow(
                            stringResource(R.string.fact_resolution),
                            buildString {
                                append(facts.width)
                                append(" × ")
                                append(facts.height)
                                if (facts.rotation != 0) {
                                    append(
                                        " (${
                                            stringResource(
                                                R.string.fact_rotation,
                                                facts.rotation,
                                            )
                                        })"
                                    )
                                }
                            },
                        )
                        FactRow(
                            stringResource(R.string.fact_fps),
                            if (facts.fpsNum > 0 && facts.fpsDen > 0) {
                                "%.2f fps".format(
                                    Locale.US,
                                    facts.fpsNum.toDouble() / facts.fpsDen.toDouble(),
                                )
                            } else {
                                "-"
                            },
                        )
                        FactRow(
                            stringResource(R.string.fact_duration),
                            formatDurationUs(facts.durationUs),
                        )
                        if (facts.pixFmt.isNotBlank()) {
                            FactRow(stringResource(R.string.fact_pixfmt), facts.pixFmt)
                        }
                        if (facts.bitrate > 0) {
                            FactRow(
                                stringResource(R.string.fact_bitrate),
                                "%.1f Mbps".format(Locale.US, facts.bitrate / 1_000_000.0),
                            )
                        }
                        if (facts.nbFrames > 0) {
                            FactRow(
                                stringResource(R.string.fact_frames),
                                facts.nbFrames.toString(),
                            )
                        }
                        FactRow(
                            stringResource(R.string.fact_audio),
                            if (facts.audioCodec.isNullOrEmpty()) {
                                stringResource(R.string.fact_audio_none)
                            } else {
                                "${facts.audioCodec} · ${facts.audioChannels}ch · ${facts.audioRate}Hz"
                            },
                        )
                        val colorParts = listOf(facts.cspace, facts.cprim, facts.ctrc)
                            .mapNotNull { it?.takeIf { s -> s.isNotBlank() } }
                        if (colorParts.isNotEmpty()) {
                            FactRow(
                                stringResource(R.string.fact_color),
                                colorParts.joinToString(" / "),
                            )
                        }
                        if (facts.isHdr) {
                            FactRow(
                                label = "HDR",
                                value = stringResource(R.string.media_hdr_hint),
                                warn = true,
                            )
                        }
                        if (facts.isDngZip) {
                            // zip 专属提示:帧数/尺寸 + 预估耗时(契约 §10.6)
                            val frames = if (facts.frames > 0) facts.frames else facts.nbFrames
                            if (frames > 0) {
                                Spacer(Modifier.height(6.dp))
                                Text(
                                    text = stringResource(
                                        R.string.dng_zip_hint,
                                        frames,
                                        facts.width,
                                        facts.height,
                                        stringResource(
                                            R.string.dng_zip_hint_frames,
                                            zipEstimateMinutes(frames, facts.width, facts.height, state.dngQuality),
                                        ),
                                    ),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                            if (facts.width > BIG_RES_EDGE || facts.height > BIG_RES_EDGE) {
                                Spacer(Modifier.height(4.dp))
                                FactRow(
                                    label = stringResource(R.string.fact_resolution),
                                    value = stringResource(
                                        R.string.dng_zip_hint_res,
                                        facts.width,
                                        facts.height,
                                    ),
                                    warn = true,
                                )
                            }
                        }
                    }
                }
            }

            // ---------- 3. 输出格式 ----------
            item {
                SectionCard(title = stringResource(R.string.format_title)) {
                    val spec = FormatCatalog.spec(state.format)
                    SegmentedRow(
                        options = OutFormat.values().map { stringResource(it.labelRes) },
                        selectedIndex = state.format.ordinal,
                        onSelect = { index -> vm.setFormat(OutFormat.values()[index]) },
                    )
                    if (spec.supports10Bit) {
                        Spacer(Modifier.height(8.dp))
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text(
                                text = stringResource(R.string.tenbit_toggle),
                                style = MaterialTheme.typography.bodyMedium,
                            )
                            Switch(
                                checked = state.tenBit,
                                onCheckedChange = { vm.setTenBit(it) },
                            )
                        }
                    }
                }
            }

            // ---------- 4. 质量 ----------
            item {
                SectionCard(title = stringResource(R.string.quality_title)) {
                    if (state.format == OutFormat.PRORES) {
                        Text(
                            text = stringResource(R.string.quality_desc_prores),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    } else {
                        SegmentedRow(
                            options = listOf(
                                stringResource(R.string.quality_high),
                                stringResource(R.string.quality_medium),
                                stringResource(R.string.quality_low),
                            ),
                            selectedIndex = state.tier.ordinal,
                            onSelect = { index -> vm.setTier(QualityTier.values()[index]) },
                        )
                        Spacer(Modifier.height(8.dp))
                        Text(
                            text = qualityDescription(state.format, state.tier),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }

            // ---------- 5. 缩放与帧率(DNG zip 另有解raw/白平衡档位) ----------
            item {
                val zipFacts = state.files.firstOrNull { it.facts?.isDngZip == true }?.facts
                SectionCard(
                    title = stringResource(R.string.scale_title) + " / " + stringResource(R.string.fps_title),
                ) {
                    SegmentedRow(
                        options = ScaleMode.values().map { stringResource(it.labelRes) },
                        selectedIndex = state.scale.ordinal,
                        onSelect = { index -> vm.setScale(ScaleMode.values()[index]) },
                    )
                    Spacer(Modifier.height(10.dp))
                    if (zipFacts != null) {
                        // zip 没有容器帧率,「原始」不可用;默认 25(契约 §10.6)。
                        // 档位清单共享 FpsMode.zipOptions(L-22:不再内联字面量)。
                        val options = FpsMode.zipOptions
                        val current =
                            if (state.fps == FpsMode.ORIG) FpsMode.F25 else state.fps
                        SegmentedRow(
                            options = options.map { stringResource(it.labelRes) },
                            selectedIndex = options.indexOf(current).coerceAtLeast(0),
                            onSelect = { index -> vm.setFps(options[index]) },
                        )
                        Spacer(Modifier.height(10.dp))
                        Text(
                            text = stringResource(R.string.dng_quality_title),
                            style = MaterialTheme.typography.labelLarge,
                        )
                        Spacer(Modifier.height(4.dp))
                        SegmentedRow(
                            options = DngQuality.values().map { stringResource(it.labelRes) },
                            selectedIndex = state.dngQuality.ordinal,
                            onSelect = { index -> vm.setDngQuality(DngQuality.values()[index]) },
                        )
                        Spacer(Modifier.height(10.dp))
                        Text(
                            text = stringResource(R.string.dng_color_title),
                            style = MaterialTheme.typography.labelLarge,
                        )
                        Spacer(Modifier.height(4.dp))
                        SegmentedRow(
                            options = DngColorMode.values().map { stringResource(it.labelRes) },
                            selectedIndex = state.dngColor.ordinal,
                            onSelect = { index -> vm.setDngColor(DngColorMode.values()[index]) },
                        )
                        Spacer(Modifier.height(10.dp))
                        Text(
                            text = stringResource(R.string.dngmap_title),
                            style = MaterialTheme.typography.labelLarge,
                        )
                        Spacer(Modifier.height(4.dp))
                        SegmentedRow(
                            options = DngMap.values().map { stringResource(it.labelRes) },
                            selectedIndex = state.dngMap.ordinal,
                            onSelect = { index -> vm.setDngMap(DngMap.values()[index]) },
                        )
                    } else {
                        val options = FpsMode.standard
                        // R12-10:zip 专属帧率(如 48)不在这份选项里,coerce 只会伪装高亮;
                        // 把越界选择真正归位,显示才与实际 spec 一致。但归位只认「非空且
                        // 全部探完」的批次:空列表(刚启动)与探测窗口(facts 未就绪)同样
                        // 落到本分支,提前归位会抹掉用户为 zip 批次选择的/持久化的 48/50。
                        val allProbed = state.files.all { it.probed }
                        LaunchedEffect(state.fps, allProbed) {
                            if (state.files.isNotEmpty() && allProbed && state.fps !in options) {
                                vm.setFps(FpsMode.ORIG)
                            }
                        }
                        SegmentedRow(
                            options = options.map { stringResource(it.labelRes) },
                            selectedIndex = options.indexOf(state.fps).coerceAtLeast(0),
                            onSelect = { index -> vm.setFps(options[index]) },
                        )
                    }
                }
            }

            // ---------- 6. 高级设置 ----------
            item {
                SectionCard(title = stringResource(R.string.advanced_settings)) {
                    OutlinedButton(
                        onClick = { advancedExpanded = !advancedExpanded },
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Text(
                            stringResource(
                                if (advancedExpanded) R.string.advanced_hide else R.string.advanced_show,
                            ),
                        )
                    }
                }
            }

            if (advancedExpanded) {
                // ---------- 7. LUT ----------
                item {
                    SectionCard(title = stringResource(R.string.pick_lut)) {
                    Button(
                        onClick = {
                            // 最后一个兜底 MIME 让选择器能列出未知扩展名(如 .dat)的 LUT
                            pickLut.launch(arrayOf("text/*", "application/octet-stream", "*/*"))
                        },
                    ) {
                        Text(stringResource(R.string.lut_pick_action))
                    }
                    Text(
                        text = stringResource(R.string.lut_format_hint),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    val lut = state.lut
                    if (lut == null) {
                        Spacer(Modifier.height(8.dp))
                        Text(
                            text = stringResource(R.string.lut_none),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    } else {
                        Spacer(Modifier.height(8.dp))
                        FactRow(stringResource(R.string.fact_file), lut.title.orEmpty())
                        val grid = lut.size
                        if (grid != null) {
                            FactRow(
                                stringResource(R.string.fact_lut_grid),
                                "$grid × $grid × $grid",
                            )
                        }
                        FactRow(
                            stringResource(R.string.fact_size),
                            formatSize(lut.bytes),
                        )
                        Spacer(Modifier.height(4.dp))
                        LabeledSlider(
                            label = stringResource(R.string.lut_intensity),
                            value = state.lutIntensity,
                            valueRange = 0f..1f,
                            steps = 19,
                            onChange = { vm.setLutIntensity(it) },
                            valueText = stringResource(
                                R.string.lut_strength,
                                (state.lutIntensity * 100).toInt(),
                            ),
                        )
                        Spacer(Modifier.height(4.dp))
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            PreviewTile(
                                label = stringResource(R.string.lut_preview_before),
                                bitmap = beforeBitmap,
                                modifier = Modifier.weight(1f),
                            )
                            PreviewTile(
                                label = stringResource(R.string.lut_preview_after),
                                bitmap = afterBitmap,
                                modifier = Modifier.weight(1f),
                            )
                        }
                        Spacer(Modifier.height(8.dp))
                        OutlinedButton(onClick = { vm.clearLut() }) {
                            Text(stringResource(R.string.btn_clear_lut))
                        }
                    }
                }
            }
            }

            // ---------- 8. 音频 ----------
            item {
                SectionCard(title = stringResource(R.string.audio_title)) {
                    SegmentedRow(
                        options = listOf(
                            stringResource(R.string.audio_copy),
                            stringResource(R.string.audio_aac),
                            stringResource(R.string.audio_drop),
                        ),
                        selectedIndex = state.audio.ordinal,
                        onSelect = { index -> vm.setAudio(AudioMode.values()[index]) },
                    )
                }
            }

            // ---------- 9. 输出目录 ----------
            item {
                SectionCard(title = stringResource(R.string.output_dir_title)) {
                    val treeMissing = state.outputDirName == null
                    FactRow(
                        label = stringResource(R.string.output_dir_current),
                        value = state.outputDirName ?: stringResource(R.string.output_dir_unset),
                        warn = treeMissing, // 未授权时必须红字,不能装作有默认目录
                    )
                    Spacer(Modifier.height(8.dp))
                    OutlinedButton(onClick = { pickTree.launch(vm.initialTreeUri()) }) {
                        Text(
                            if (treeMissing) {
                                stringResource(R.string.output_dir_pick)
                            } else {
                                stringResource(R.string.output_dir_change)
                            }
                        )
                    }
                }
            }

            // ---------- 10. 开始转码 ----------
            item {
                val startHint = when {
                    state.busy -> null
                    state.files.isEmpty() -> R.string.start_hint_pick_files
                    state.files.any { !it.probed } -> R.string.start_hint_probing
                    state.files.any { it.probed && it.facts == null } -> R.string.start_hint_unparseable
                    state.outputDirName == null -> R.string.start_hint_output_dir
                    else -> null
                }
                Button(
                    onClick = ::requestStart,
                    enabled = !state.busy && state.canStart,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 12.dp)
                        .height(52.dp),
                ) {
                    if (state.busy) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(20.dp),
                            strokeWidth = 2.dp,
                            color = MaterialTheme.colorScheme.onPrimary,
                        )
                        Spacer(Modifier.size(8.dp))
                        Text(stringResource(R.string.btn_busy))
                    } else {
                        Text(stringResource(R.string.btn_start))
                    }
                }
                if (startHint != null) {
                    Text(
                        text = stringResource(startHint),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(bottom = 12.dp),
                    )
                }
            }
        }

        if (showNotificationRationale) {
            AlertDialog(
                onDismissRequest = { showNotificationRationale = false },
                title = { Text(stringResource(R.string.notification_rationale_title)) },
                text = { Text(stringResource(R.string.notification_rationale_message)) },
                confirmButton = {
                    TextButton(
                        onClick = {
                            NotificationPermissionGate.asked = true
                            showNotificationRationale = false
                            notificationPermissionLauncher.launch(
                                Manifest.permission.POST_NOTIFICATIONS,
                            )
                        },
                    ) {
                        Text(stringResource(R.string.notification_allow))
                    }
                },
                dismissButton = {
                    TextButton(onClick = ::startWithoutNotificationPermission) {
                        Text(stringResource(R.string.notification_skip))
                    }
                },
            )
        }

        SnackbarHost(
            hostState = snackbarHostState,
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(16.dp),
        )
    }
}

/** 单张预览瓦片(16:9,底色占位;解码失败显示"无预览")。 */
/**
 * U-10:文件清单里的一行(独立 composable,作为 LazyColumn 的 item 内容)。
 * 外观与 SectionCard 对齐:横向 12dp 外边距、同底色 Card。
 */
@Composable
    private fun FileRowCard(file: HomeViewModel.PickedFile, busy: Boolean, onRemove: () -> Unit) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 6.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(start = 16.dp, end = 8.dp, top = 8.dp, bottom = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text(
                    text = file.name,
                    style = MaterialTheme.typography.bodyMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                val unknownLabel = if (file.probed) {
                    stringResource(R.string.file_unparseable)
                } else {
                    stringResource(R.string.file_probing)
                }
                // U-08:只为「有值」的字段出内容 —— 此前 zip 会露出「60.0 MB · - · dng」
                // (时长无效时 formatDurationUs 返回 "-",codec 也可能是空串)。
                val meta = buildList {
                    if (file.size > 0L) add(formatSize(file.size))
                    file.facts?.durationUs?.takeIf { it > 0 }?.let { add(formatDurationUs(it)) }
                    file.facts?.codec?.takeIf { it.isNotBlank() }?.let { add(it) }
                    if (!file.probed || file.facts == null) add(unknownLabel)
                }.joinToString(" · ")
                Text(
                    text = meta,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            TextButton(onClick = onRemove, enabled = !busy) {
                Text(stringResource(R.string.btn_remove_file))
            }
        }
    }
}

@Composable
private fun PreviewTile(label: String, bitmap: ImageBitmap?, modifier: Modifier = Modifier) {
    Column(modifier) {
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .aspectRatio(16f / 9f)
                .background(MaterialTheme.colorScheme.surfaceVariant),
            contentAlignment = Alignment.Center,
        ) {
            if (bitmap != null) {
                Image(
                    bitmap = bitmap,
                    contentDescription = label,
                    modifier = Modifier.fillMaxSize(),
                    contentScale = ContentScale.Crop,
                )
            } else {
                Text(
                    text = stringResource(R.string.preview_none),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        Spacer(Modifier.height(4.dp))
        Text(
            text = label,
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/** 当前格式/档位的数值说明:PRORES = 母版质量;VP9 = CQ;H.264/H.265 = CRF。 */
@Composable
private fun qualityDescription(format: OutFormat, tier: QualityTier): String {
    val spec = FormatCatalog.spec(format)
    val lowerIsBetter = stringResource(R.string.quality_lower_is_better)
    return when (format) {
        OutFormat.PRORES ->
            stringResource(R.string.quality_desc_prores)
        OutFormat.VP9 -> {
            val cq = when (tier) {
                QualityTier.HIGH -> spec.vp9CqHigh
                QualityTier.MEDIUM -> spec.vp9CqMedium
                QualityTier.LOW -> spec.vp9CqLow
            }
            stringResource(R.string.quality_desc_vp9, cq, lowerIsBetter)
        }
        else -> {
            val crf = when (tier) {
                QualityTier.HIGH -> spec.crfHigh
                QualityTier.MEDIUM -> spec.crfMedium
                QualityTier.LOW -> spec.crfLow
            }
            stringResource(R.string.quality_desc_crf, crf, lowerIsBetter)
        }
    }
}

/** JPEG 字节 → ImageBitmap;失败返回 null。 */
private fun ByteArray?.decodeToImage(): ImageBitmap? =
    this?.let { bytes ->
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.asImageBitmap()
    }

/** 超过这个边长(任一边)就提示"精细档极慢"(契约 §10.6)。 */
private const val BIG_RES_EDGE = 4096

private object NotificationPermissionGate {
    @Volatile
    var asked: Boolean = false
}

/** LUT 预览去抖:滑条停顿 ≥此时长才进 native 解码(K-08)。 */
private const val PREVIEW_DEBOUNCE_MS = 250L

/**
 * DNG zip 转码耗时粗估(分钟,≥1):帧数 × 单帧秒数 × 面积倍率。
 * 单帧秒数是主流旗舰 soc + LibRaw 单线程 + prores_ks 的经验值:
 * 1080p 快速 ~0.08s / 均衡 ~0.15s / 精细 ~0.6s,面积按 1080p 归一线性放大。
 * 只用于提示,不承诺精度(契约 §10.6 要求"UI 给预估耗时")。
 */
private fun zipEstimateMinutes(frames: Long, w: Int, h: Int, quality: DngQuality): Long {
    if (frames <= 0) return 0L
    val area = (w.toDouble() * h) / (1920.0 * 1080.0)
    val perFrame = when (quality) {
        DngQuality.FAST -> 0.08
        DngQuality.BALANCED -> 0.15
        DngQuality.FINE -> 0.6
    }
    val seconds = frames * perFrame * area.coerceAtLeast(0.1)
    return maxOf(1L, Math.round(seconds / 60.0))
}
