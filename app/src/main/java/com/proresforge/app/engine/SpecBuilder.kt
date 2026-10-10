// SpecBuilder.kt — JobSpec + MediaFacts → k=v\n spec 串(契约 §3)。
// 2026-10-07 从 FFmpeg.buildSpec 原样迁出:FFmpeg object 的 <clinit> 会
// System.loadLibrary,纯 JVM 单测(§7 测试缺口)一碰就 UnsatisfiedLinkError;
// 本对象零 Android 依赖,buildSpec 规则可被 app/src/test 直接覆盖。
package com.proresforge.app.engine

import com.proresforge.app.data.AudioMode
import com.proresforge.app.data.FormatCatalog
import com.proresforge.app.data.FormatSpec
import com.proresforge.app.data.FpsMode
import com.proresforge.app.data.JobSpec
import com.proresforge.app.data.MediaFacts
import com.proresforge.app.data.OutFormat
import com.proresforge.app.data.QualityTier
import com.proresforge.app.data.ScaleMode
import kotlin.math.floor
import java.util.Locale

object SpecBuilder {

    /**
     * 把 [JobSpec] + 探测出的 [MediaFacts] 序列化成 §3 的 k=v\n spec 串。
     *
     * 关键规则(与 cpp/ 侧 parser 一一对应):
     * - w=h=0 表示源尺寸;输出宽高一律偶数;P1080/P720 恒定把长边对齐到 1920/1280
     *   (源更小时会上采样,与「1080p / 720p」的字面语义一致)
     * - ScaleMode.ORIG 的 0,0 交给 native 做 autorotate:旋转 90/270 时 native 看到的是
     *   交换后的源宽高,因此这里按旋转后的源宽高计算倍率,再让 native 回写实际尺寸
     * - fps:ORIG → facts.fpsNum/fpsDen(为 0 则 0,0 表示源帧率)
     * - rc:H264/H265 → crf(字段按 tier 取 FormatSpec.crf*);VP9 → q(字段填 vp9Cq* 档位);
     *   ProRes → bitrate(按目标面积给足够大的 vbr 上限,§3.2)
     * - gop 固定 2(秒);profile 不输出(由 native 按 pixfmt 推导,如 yuv420p10le→main10)
     * - lut/lutint 两行仅在 lutPath 非空时写(B11);lutint 最多 3 位小数;scale=lanczos;rotate=auto
     * - cspace/cprim/ctrc:H264/H265/VP9=bt709;ProRes 省略三个颜色键(§3.1 空值语义,
     *   ProRes 的色域由 profile 承载)
     * - audio:copy/aac/drop;ZIP 内存在 audio.wav 时同样沿用用户选择,没有音频时 native 自动无轨;
     *   faststart:VP9 恒 0(容器不支持),其余取 JobSpec.faststart
     * - title 行省略(容器元数据由 native 自行决定)
     */
    fun buildSpec(spec: JobSpec, facts: MediaFacts): String {
        val fmt = FormatCatalog.spec(spec.format)
        val isVp9 = spec.format == OutFormat.VP9
        val isProres = spec.format == OutFormat.PRORES

        // ---- 输出尺寸(偶数;0 = 源尺寸)---- 旋转 90/270 时按交换后的源宽高计算
        val rotated = facts.rotation == 90 || facts.rotation == 270
        val srcW = if (rotated) facts.height else facts.width
        val srcH = if (rotated) facts.width else facts.height
        val outWH: Pair<Int, Int> = when (spec.scale) {
            ScaleMode.ORIG -> 0 to 0
            ScaleMode.P1080 -> fitLongEdge(srcW, srcH, 1920)
            ScaleMode.P720 -> fitLongEdge(srcW, srcH, 1280)
            ScaleMode.P50 -> even(srcW / 2).coerceAtLeast(2) to even(srcH / 2).coerceAtLeast(2)
            ScaleMode.P25 -> even(srcW / 4).coerceAtLeast(2) to even(srcH / 4).coerceAtLeast(2)
        }

        // ---- 帧率 ----
        // DNG zip 没有容器帧率(fpsnum=fpsden=0),ORIG 不可用 → 固定 25(可被用户改)
        val fps: Pair<Int, Int> = when {
            spec.fps != FpsMode.ORIG -> spec.fps.fps to 1
            facts.fpsNum > 0 && facts.fpsDen > 0 -> facts.fpsNum to facts.fpsDen
            facts.isDngZip -> 25 to 1
            else -> 0 to 0 // 源帧率未知:交给 native 用容器默认值
        }

        // ---- 码率控制 ----
        val rc: String
        val crf: Int
        val bitrate: Long
        when {
            isVp9 -> {
                rc = "q"      // §3.2:libvpx-vp9 用 cq-level,档位放进 crf 字段
                crf = vp9Cq(fmt, spec.tier)
                bitrate = 0L
            }

            isProres -> {
                rc = "bitrate"
                crf = 0
                // 目标面积(ORIG 未指定缩放时用源面积)× 400Mbps@4K 的比例,夹在 50M~1G
                val ow = if (outWH.first > 0) outWH.first else srcW
                val oh = if (outWH.second > 0) outWH.second else srcH
                val area = ow.toLong().coerceAtLeast(1L) * oh.toLong().coerceAtLeast(1L)
                bitrate = (400_000_000.0 * area / (3840.0 * 2160.0)).toLong()
                    .coerceIn(50_000_000L, 1_000_000_000L)
            }

            else -> {
                rc = "crf"    // §3.2:mediacodec/libx264/libx265 按 crf 线性插值出 bitrate
                crf = crfValue(fmt, spec.tier)
                bitrate = 0L
            }
        }

        // ---- GOP:2 秒(§3.2);profile 由 native 按 pixfmt 推导,不输出 ----
        val sb = StringBuilder(512)
        // tenBit 仅对 H265 实际生效(H264 不支持,ProRes 恒 10bit 由 pixfmt 承载)
        val pixFmt = if (spec.format == OutFormat.H265 && spec.tenBit) "yuv420p10le" else fmt.pixFmt
        sb.append("vcodec=").append(fmt.vcodec).append('\n')
        sb.append("pixfmt=").append(pixFmt).append('\n')
        sb.append("w=").append(outWH.first).append('\n')
        sb.append("h=").append(outWH.second).append('\n')
        sb.append("fpsnum=").append(fps.first).append('\n')
        sb.append("fpsden=").append(fps.second).append('\n')
        sb.append("rc=").append(rc).append('\n')
        sb.append("crf=").append(crf).append('\n')
        sb.append("bitrate=").append(bitrate).append('\n')
        sb.append("gop=").append(GOP_SEC).append('\n')
        sb.append("scale=lanczos\n")
        sb.append("rotate=auto\n")
        if (!isProres) {
            // H.264/H.265/VP9 一律打 bt709 标签(§3.1);ProRes 省略(空值 = 不写标签)
            sb.append("cspace=bt709\n")
            sb.append("cprim=bt709\n")
            sb.append("ctrc=bt709\n")
        }
        // audio=copy/aac/drop. ZIP 内存在 audio.wav 时沿用用户选择;没有音频时
        // native 会自然跳过音频流,不需要 Kotlin 侧提前改写成 drop。
        val audioMode = when (spec.audio) {
            AudioMode.COPY -> "copy"
            AudioMode.AAC -> "aac"
            AudioMode.DROP -> "drop"
        }
        sb.append("audio=").append(audioMode).append('\n')
        sb.append("faststart=").append(if (isVp9) 0 else if (spec.faststart) 1 else 0).append('\n')
        val lutPath = spec.lutPath
        if (!lutPath.isNullOrEmpty()) {
            sb.append("lut=").append(lutPath).append('\n')
            sb.append("lutint=").append(formatLutIntensity(spec.lutIntensity)).append('\n')
        }
        if (facts.isDngZip) {
            // DNG 序列 zip: native 跳过视频 avformat,走 dngzip + LibRaw;
            // 音频若存在则由同一个 DngZip 成员 audio.wav 提供。
            sb.append("src=dngzip\n")
            sb.append("dngq=").append(spec.dngQuality.specValue()).append('\n')
            sb.append("dngcolor=").append(spec.dngColor.specValue()).append('\n')
            sb.append("dngmap=").append(spec.dngMap.specValue()).append('\n')
        }
        // title 行省略(§3 未要求写入容器标题)
        return sb.toString()
    }

    // ------------------------------------------------------------------ 内部实现

    private fun crfValue(f: FormatSpec, tier: QualityTier): Int = when (tier) {
        QualityTier.HIGH -> f.crfHigh
        QualityTier.MEDIUM -> f.crfMedium
        QualityTier.LOW -> f.crfLow
    }

    private fun vp9Cq(f: FormatSpec, tier: QualityTier): Int = when (tier) {
        QualityTier.HIGH -> f.vp9CqHigh
        QualityTier.MEDIUM -> f.vp9CqMedium
        QualityTier.LOW -> f.vp9CqLow
    }

    /**
     * 长边对齐到 [longEdge],短边等比向下取偶数(源更小时会上采样到 [longEdge])。
     *
     * 两个方向都要夹到 ≥2:encoder 的最小尺寸是 2,只保 height 会让
     * portrait 视频(w < h)在极端比例下算出 w=0,把「0 = 用源尺寸」的语义抢走。
     */
    private fun fitLongEdge(w: Int, h: Int, longEdge: Int): Pair<Int, Int> {
        if (w <= 0 || h <= 0) return 0 to 0
        val long = maxOf(w, h)
        val scale = longEdge.toDouble() / long
        return even(floor(w * scale).toInt()).coerceAtLeast(2) to
            even(floor(h * scale).toInt()).coerceAtLeast(2)
    }

    /** 向下取偶数;0 保 0(调用方视 0 为「用源尺寸」,不做夹取)。 */
    private fun even(v: Int): Int = if (v <= 0) 0 else v and 1.inv()

    /** 强度最多 3 位小数(§3:lutint 0..1);用 Locale.US 避免小数逗号。 */
    private fun formatLutIntensity(v: Float): String {
        val s = String.format(Locale.US, "%.3f", v.coerceIn(0f, 1f))
        return s.trimEnd('0').trimEnd('.').ifEmpty { "0" }
    }

    /** GOP(秒)= 2,§3.2。 */
    private const val GOP_SEC = 2
}
