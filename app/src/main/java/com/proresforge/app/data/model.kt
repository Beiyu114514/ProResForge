package com.proresforge.app.data

import android.net.Uri
import com.proresforge.app.R
import kotlinx.serialization.Serializable

/**
 * 数据模型单一定义处(契约 §8.1)。全 App(含 engine)一律引用这里,不得重复定义。
 * 命名与 UI 代理/引擎代理约定一致,改名即违约。
 */

/**
 * 输出格式:labelRes 给 UI 展示(走 strings.xml,中英双语),
 * ext/mime 给 SAF 建文档(OutputDir.outputUriFor)。
 */
enum class OutFormat(val labelRes: Int, val ext: String, val mime: String) {
    H264(R.string.fmt_h264, "mp4", "video/mp4"),
    H265(R.string.fmt_h265, "mp4", "video/mp4"),
    VP9(R.string.fmt_vp9, "webm", "video/webm"),
    PRORES(R.string.fmt_prores, "mov", "video/quicktime")
}

/**
 * 单个格式的编码参数画像。
 * crfHigh/crfMedium/crfLow:三档 CRF(HIGH 画质最好 → 数值最小);
 * vp9CqHigh/Medium/Low:VP9 的 CQ 三档。
 * ProRes(prores_ks)无 CRF,这些字段为 0、被忽略。
 */
data class FormatSpec(
    val format: OutFormat,
    val vcodec: String,
    val pixFmt: String,
    val supports10Bit: Boolean,
    val audioModes: List<AudioMode>,
    val crfHigh: Int,
    val crfMedium: Int,
    val crfLow: Int,
    val vp9CqHigh: Int,
    val vp9CqMedium: Int,
    val vp9CqLow: Int
)

/** 音频处理方式。VP9/WebM 的 copy 仅在源为 opus/vorbis 时有效,引擎侧会自动降级。 */
enum class AudioMode { COPY, AAC, DROP }

/** 缩放模式:shortEdge 仅作语义标注(0=原始,-1/-2=百分比),实际取值为 0 时按原始尺寸处理。 */
enum class ScaleMode(val labelRes: Int, val shortEdge: Int) {
    ORIG(R.string.scale_orig, 0),
    P1080(R.string.scale_1080, 1080),
    P720(R.string.scale_720, 720),
    P50(R.string.scale_50, -1),
    P25(R.string.scale_25, -2)
}

/**
 * 帧率模式:fps=0 表示跟随源。
 * DNG 序列 zip 没有容器帧率,fps=0 不可用,UI 只从 [standard] 里剔除 ORIG 并提供默认 25;
 * 普通容器可选整个 [standard] 列表。新增常量追加到末尾,
 * 否则会打乱已有常量的序号(SegmentedRow 按下标取值)。
 */
enum class FpsMode(val labelRes: Int, val fps: Int) {
    ORIG(R.string.fps_orig, 0),
    F60(R.string.fps_60, 60),
    F30(R.string.fps_30, 30),
    F24(R.string.fps_24, 24),
    F25(R.string.fps_25, 25),
    F48(R.string.fps_48, 48),
    F50(R.string.fps_50, 50);

    companion object {
        /** 普通容器帧率档位。 */
        val standard: List<FpsMode> = listOf(ORIG, F60, F30, F25, F24)

        /**
         * DNG 序列 zip 的帧率档位(契约 §10.6:zip 没有容器帧率,ORIG 不可用)。
         * UI(HomeScreen zip 分支)与单测共享这一份,别再各写一份字面量(L-22)。
         */
        val zipOptions: List<FpsMode> = listOf(F24, F25, F30, F48, F50, F60)
    }
}

/**
 * DNG 解raw 档位(契约 §10.4):LibRaw user_qual / half_size。
 * FAST = half_size + PPG 之前的最快档(预览/试转);
 * BALANCED = PPG(默认);FINE = AHD(最慢、细节最好)。
 */
enum class DngQuality(val labelRes: Int) {
    FAST(R.string.dngq_fast),
    BALANCED(R.string.dngq_balanced),
    FINE(R.string.dngq_fine);

    /** spec 里的 dngq 取值(契约 §3)。 */
    fun specValue(): String = ordinal.toString()
}

/** DNG 白平衡策略(契约 §10.4)。d65 = 固定日光(1,1,1,1)。 */
enum class DngColorMode(val labelRes: Int) {
    CAMERA(R.string.dngcolor_camera),
    AUTO(R.string.dngcolor_auto),
    D65(R.string.dngcolor_d65);

    /** spec 里的 dngcolor 取值(契约 §3)。 */
    fun specValue(): String = when (this) {
        AUTO -> "auto"
        D65 -> "d65"
        else -> "camera"
    }
}

/** DNG 序列 zip 的内置色彩映射(普通容器忽略)。 */
enum class DngMap(val labelRes: Int) {
    NONE(R.string.dngmap_none),
    APPLE_LOG(R.string.dngmap_applelog);

    fun specValue(): String = when (this) {
        NONE -> "none"
        APPLE_LOG -> "applelog"
    }
}

/** 画质档位。 */
enum class QualityTier { HIGH, MEDIUM, LOW }

/**
 * 一个待转码任务。outputUri 必须已通过 SAF createDocument 建好再传入(契约 §8.1)。
 */
data class JobSpec(
    val inputUri: Uri,
    val inputName: String,
    val outputUri: Uri,
    val outputName: String,
    val format: OutFormat,
    val tier: QualityTier,
    val scale: ScaleMode,
    val fps: FpsMode,
    val lutPath: String?,
    val lutName: String?,
    val lutIntensity: Float,
    val audio: AudioMode,
    val tenBit: Boolean,
    val faststart: Boolean,
    /** DNG 序列 zip 专用:解raw 档位(普通容器忽略)。 */
    val dngQuality: DngQuality = DngQuality.BALANCED,
    /** DNG 序列 zip 专用:白平衡策略(普通容器忽略)。 */
    val dngColor: DngColorMode = DngColorMode.CAMERA,
    /** DNG 序列 zip 专用:色彩映射(普通容器忽略)。 */
    val dngMap: DngMap = DngMap.NONE
)

/** 任务状态。 */
enum class JobState { QUEUED, RUNNING, DONE, FAILED, CANCELLED }

/** 队列中的一条任务快照(JobBus 的状态载体,UI 直接展示)。 */
data class JobRecord(
    val id: String,
    val spec: JobSpec,
    val state: JobState,
    val pct: Int,
    val stage: String,
    val frame: Long,
    val totalFrames: Long,
    val error: String?,
    val startedAt: Long,
    val endedAt: Long,
    val outSize: Long,
    val inSize: Long,
    val durationMs: Long
)

/**
 * nativeInfo(§4)解析结果。字段名与 C 端返回的 key 集合一一对应:
 * codec/w/h/pixfmt/fpsnum/fpsden/duration_us/bitrate/nb_frames/rotation/container/
 * audio_codec/audio_ch/audio_rate/cspace/cprim/ctrc。
 * - audioCodec/cspace/cprim/ctrc 为 null 表示 C 端返回空串(=无)。
 * - durationUs/nbFrames 为 -1 表示未知。
 * - isHdr 由色彩标签推导(smpte2084 / arib-std-b67),供 UI 弹 HDR 提示。
 */
data class MediaFacts(
    val codec: String,
    val width: Int,
    val height: Int,
    val fpsNum: Int,
    val fpsDen: Int,
    val durationUs: Long,
    val bitrate: Long,
    val nbFrames: Long,
    val rotation: Int,
    val container: String,
    val pixFmt: String,
    val audioCodec: String?,
    val audioChannels: Int,
    val audioRate: Int,
    val cspace: String?,
    val cprim: String?,
    val ctrc: String?,
    val isHdr: Boolean,
    /**
     * DNG 序列 zip 的帧数(nativeInfo 的 frames 键,契约 §4);非 zip 一律 -1。
     * zip 探测时 nbFrames 也同步成同一个值。
     */
    val frames: Long = -1L
) {
    /** 是否 DNG 序列 zip(MotionCam):决定 zip 专属参数卡与 fps 档位(契约 §10.6)。 */
    val isDngZip: Boolean
        get() = codec.equals("dng", true) || container.equals("zip", true)
}

/**
 * 历史记录一条(JobStore 持久化,kotlinx.serialization)。
 * 注意:JobRecord 含 Uri 不可序列化,历史只存 outputUri 字符串。
 *
 * 序列化注意:format/state 两个枚举按 name 存。**枚举常量改名即旧文件反序列化失败**;
 * JobStore 加载失败会把坏档备份成 jobs.json.corrupt-<时间戳> 并打日志(S-3),然后从空
 * 历史开始。若要改枚举名,仍需配套 @SerialName 或写一次性迁移,别裸改常量名。
 */
@Serializable
data class HistoryItem(
    val id: String,
    val ts: Long,
    val inputName: String,
    val outputName: String,
    val format: OutFormat,
    val state: JobState,
    val error: String?,
    val lutName: String?,
    val lutIntensity: Float,
    val inSize: Long,
    val outSize: Long,
    val durationMs: Long,
    val outputUriString: String?,
    /** DNG zip 的总帧数(DNG 序列 zip 才有意义;普通容器 0)。 */
    val frames: Long = 0L,
    /** 任务使用的帧率(zip 时为用户显式选择;0 = 跟随源)。 */
    val fps: Int = 0
)
