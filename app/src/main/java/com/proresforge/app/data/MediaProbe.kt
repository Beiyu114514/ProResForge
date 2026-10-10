package com.proresforge.app.data

import android.content.ContentResolver
import android.content.Context
import android.net.Uri
import com.proresforge.app.engine.AppLog
import com.proresforge.app.engine.FFmpeg
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * 媒体信息探测(契约 §4):把 nativeInfo 返回的 "k=v;k=v" 点分串解析成 [MediaFacts]。
 * 抽帧预览:直接转调 FFmpeg.nativePreview。
 *
 * fd 语义(契约 §2):所有 fd 都是 detachFd 得到的原始 fd,native 拥有并用完 close(fd),
 * 因此这里拿到 fd 后**不**再关闭;只负责打开(以及失败时的关闭)。
 * native 库缺失(.so 未就绪)时 nativeInfo/nativePreview 会抛 UnsatisfiedLinkError,
 * 统一兜底成 null,避免把 UI/Service 带崩。
 */
class MediaProbe(private val context: Context) {

    private val resolver: ContentResolver get() = context.applicationContext.contentResolver

    /** IO 线程探测输入;解析失败/无法打开/无视频流返回 null。 */
    suspend fun probe(uri: Uri): MediaFacts? = withContext(Dispatchers.IO) {
        val opened = FFmpeg.writeFd(uri, resolver) ?: run {
            AppLog.w(TAG, "probe 打不开 fd: $uri")
            return@withContext null
        }
        val info = try {
            FFmpeg.nativeInfo(opened.first, opened.second)
        } catch (t: Throwable) {
            // native 没接手 fd(如 .so 未就绪),这里自关防泄漏;正常路径由 native close
            runCatching { android.os.ParcelFileDescriptor.adoptFd(opened.first).close() }
            AppLog.e(TAG, "probe nativeInfo 异常", t)
            ""
        }
        val facts = parseInfo(info)
        if (facts != null) {
            AppLog.i(
                TAG,
                "probe ok: ${facts.codec} ${facts.width}x${facts.height} " +
                    "fps=${facts.fpsNum}/${facts.fpsDen} rot=${facts.rotation} dur=${facts.durationUs}us hdr=${facts.isHdr}"
            )
        } else {
            AppLog.w(TAG, "probe 失败: $uri info=${info.take(200)}")
        }
        facts
    }

    /**
     * 抽 atUs 处一帧 JPEG。graded=false 时忽略 lutPath/intensity(原画面),
     * graded=true 时用 cacheDir 下的 LUT 应用滤镜(强度 intensity)。
     *
     * [dngQuality]/[dngColor]/[dngMap] 是 DngQuality/DngColorMode/DngMap 的 ordinal,
     * 必须和转码 spec 的 dngq/dngcolor/dngmap 一致(N-7);普通容器忽略。
     */
    suspend fun previewJpeg(
        uri: Uri,
        atUs: Long,
        lutPath: String?,
        intensity: Double,
        graded: Boolean,
        maxW: Int,
        dngQuality: Int = 0,
        dngColor: Int = 0,
        dngMap: Int = 0
    ): ByteArray? = withContext(Dispatchers.IO) {
        val opened = FFmpeg.writeFd(uri, resolver) ?: return@withContext null
        try {
            FFmpeg.nativePreview(
                opened.first, opened.second, atUs,
                if (graded) lutPath else null, intensity.coerceIn(0.0, 1.0), graded, maxW,
                dngQuality, dngColor, dngMap
            )
        } catch (t: Throwable) {
            runCatching { android.os.ParcelFileDescriptor.adoptFd(opened.first).close() }
            null
        }
    }

    /** "k=v;k=v" 点分串 → MediaFacts;缺关键信息返回 null。 */
    private fun parseInfo(info: String): MediaFacts? {
        if (info.isBlank()) return null
        val map = HashMap<String, String>()
        info.split(';').forEach { seg ->
            val eq = seg.indexOf('=')
            if (eq <= 0) return@forEach
            map[seg.substring(0, eq).trim()] = seg.substring(eq + 1).trim()
        }
        val codec = map["codec"].orEmpty()
        val width = map["w"]?.trim()?.toIntOrNull() ?: 0
        val height = map["h"]?.trim()?.toIntOrNull() ?: 0
        // 没有视频流(或探测失败)视为失败
        if (codec.isEmpty() || width <= 0 || height <= 0) return null

        val cspace = map["cspace"]?.takeIf { it.isNotEmpty() }
        val cprim = map["cprim"]?.takeIf { it.isNotEmpty() }
        val ctrc = map["ctrc"]?.takeIf { it.isNotEmpty() }
        val isHdr = listOfNotNull(cspace, cprim, ctrc).any {
            it.equals("smpte2084", true) || it.equals("arib-std-b67", true)
        }

        return MediaFacts(
            codec = codec,
            width = width,
            height = height,
            fpsNum = map["fpsnum"]?.toIntOrNull() ?: 0,
            fpsDen = map["fpsden"]?.toIntOrNull() ?: 0,
            durationUs = map["duration_us"]?.toLongOrNull() ?: -1L,
            bitrate = map["bitrate"]?.toLongOrNull() ?: 0L,
            nbFrames = map["nb_frames"]?.toLongOrNull() ?: -1L,
            // zip 帧数在 frames 键上(契约 §4.1);普通容器没有这个键 → -1
            frames = map["frames"]?.toLongOrNull() ?: -1L,
            rotation = map["rotation"]?.toIntOrNull() ?: 0,
            container = map["container"].orEmpty(),
            pixFmt = map["pixfmt"].orEmpty(),
            audioCodec = map["audio_codec"]?.takeIf { it.isNotEmpty() },
            audioChannels = map["audio_ch"]?.toIntOrNull() ?: 0,
            audioRate = map["audio_rate"]?.toIntOrNull() ?: 0,
            cspace = cspace,
            cprim = cprim,
            ctrc = ctrc,
            isHdr = isHdr
        )
    }

    private companion object {
        const val TAG = "MediaProbe"
    }
}
