package com.proresforge.app.engine

import com.proresforge.app.fakeUri
import com.proresforge.app.data.AudioMode
import com.proresforge.app.data.DngColorMode
import com.proresforge.app.data.DngMap
import com.proresforge.app.data.DngQuality
import com.proresforge.app.data.FpsMode
import com.proresforge.app.data.JobSpec
import com.proresforge.app.data.MediaFacts
import com.proresforge.app.data.OutFormat
import com.proresforge.app.data.QualityTier
import com.proresforge.app.data.ScaleMode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * buildSpec 参数表驱动回归(第 9 轮审查 §7 测试缺口①)。
 *
 * spec 串是 Kotlin↔native 的字符串契约(strspec.h SpecKeys):键名/取值一变,
 * native 侧解析就漂。这里锁定 §3 的关键规则;实现位于 [SpecBuilder]
 * (2026-10-07 从 FFmpeg object 迁出,后者 <clinit> 里 System.loadLibrary,
 * 纯 JVM 测试碰不得)。
 *
 * Uri 字段只是透传存储:用 android.jar 桩子类占位(build.gradle 已开
 * isReturnDefaultValues,桩方法返回默认值),SpecBuilder 从不解引用它们。
 */
class FFmpegBuildSpecTest {

    private fun spec(
        format: OutFormat = OutFormat.H264,
        tier: QualityTier = QualityTier.MEDIUM,
        scale: ScaleMode = ScaleMode.ORIG,
        fps: FpsMode = FpsMode.ORIG,
        audio: AudioMode = AudioMode.COPY,
        tenBit: Boolean = false,
        faststart: Boolean = true,
        lutPath: String? = null,
        lutIntensity: Float = 1f,
        dngQuality: DngQuality = DngQuality.BALANCED,
        dngColor: DngColorMode = DngColorMode.CAMERA,
        dngMap: DngMap = DngMap.NONE,
    ) = JobSpec(
        inputUri = fakeUri(), inputName = "in.mov",
        outputUri = fakeUri(), outputName = "out.mp4",
        format = format, tier = tier, scale = scale, fps = fps,
        lutPath = lutPath, lutName = lutPath?.let { "L" }, lutIntensity = lutIntensity,
        audio = audio, tenBit = tenBit, faststart = faststart,
        dngQuality = dngQuality, dngColor = dngColor, dngMap = dngMap,
    )

    private fun facts(
        w: Int = 1920, h: Int = 1080, rot: Int = 0,
        fpsNum: Int = 0, fpsDen: Int = 0,
        codec: String = "h264", container: String = "mov",
    ) = MediaFacts(
        codec = codec, width = w, height = h,
        fpsNum = fpsNum, fpsDen = fpsDen,
        durationUs = 1_000_000L, bitrate = 0L, nbFrames = -1L,
        rotation = rot, container = container, pixFmt = "yuv420p",
        audioCodec = null, audioChannels = 0, audioRate = 0,
        cspace = null, cprim = null, ctrc = null, isHdr = false,
    )

    /** spec 串 → 有序 Map(按第一个 '=' 切,契约语义)。 */
    private fun Map<String, String>.geti(key: String): Int = getValue(key).toInt()
    private fun Map<String, String>.getl(key: String): Long = getValue(key).toLong()

    private fun parse(specStr: String): Map<String, String> =
        specStr.split('\n').filter { it.isNotEmpty() }.associate {
            val eq = it.indexOf('=')
            require(eq > 0) { "非法 spec 行: $it" }
            it.substring(0, eq) to it.substring(eq + 1)
        }

    @Test
    fun h264MediumDefaults() {
        val s = parse(SpecBuilder.buildSpec(spec(), facts()))
        assertEquals("h264_mediacodec", s["vcodec"])
        assertEquals("yuv420p", s["pixfmt"])
        assertEquals(0, s.geti("w"))          // ORIG → 0,0 = 源尺寸
        assertEquals(0, s.geti("h"))
        assertEquals("crf", s["rc"])
        assertEquals(23, s.geti("crf"))       // H264 MEDIUM
        assertEquals(0L, s.getl("bitrate"))
        assertEquals(2, s.geti("gop"))
        assertEquals("lanczos", s["scale"])
        assertEquals("auto", s["rotate"])
        assertEquals("copy", s["audio"])
        assertEquals(1, s.geti("faststart"))
        assertEquals("bt709", s["cspace"])    // 非 ProRes 打 bt709
        assertEquals("bt709", s["cprim"])
        assertEquals("bt709", s["ctrc"])
        assertFalse(s.containsKey("src"))     // 非 zip
        assertFalse(s.containsKey("lut"))     // 无 LUT(B11:lut/lutint 成对省略)
        assertFalse(s.containsKey("lutint"))
    }

    @Test
    fun h265TenBitUsesMain10PixFmt() {
        val s = parse(SpecBuilder.buildSpec(spec(format = OutFormat.H265, tenBit = true), facts()))
        assertEquals("yuv420p10le", s["pixfmt"])
        assertEquals(25, s.geti("crf"))       // H265 MEDIUM
    }

    @Test
    fun h265WithoutTenBitStays8Bit() {
        val s = parse(SpecBuilder.buildSpec(spec(format = OutFormat.H265), facts()))
        assertEquals("yuv420p", s["pixfmt"])
    }

    @Test
    fun vp9UsesCqTierAndForbidsFaststart() {
        val s = parse(SpecBuilder.buildSpec(spec(format = OutFormat.VP9, tier = QualityTier.HIGH), facts()))
        assertEquals("libvpx-vp9", s["vcodec"])
        assertEquals("q", s["rc"])
        assertEquals(20, s.geti("crf"))       // VP9 CQ HIGH
        assertEquals(0, s.geti("faststart"))  // webm 容器不支持
    }

    @Test
    fun proresUsesBitrateAndOmitsColorTags() {
        val s = parse(
            SpecBuilder.buildSpec(spec(format = OutFormat.PRORES, tier = QualityTier.LOW), facts())
        )
        assertEquals("prores_ks", s["vcodec"])
        assertEquals("yuv422p10le", s["pixfmt"])
        assertEquals("bitrate", s["rc"])
        assertEquals(0, s.geti("crf"))
        // 400Mbps@4K 按面积折算:1920x1080 = 4K 的 1/4 → 100Mbps
        assertEquals(100_000_000L, s.getl("bitrate"))
        assertFalse(s.containsKey("cspace"))  // ProRes 省略三色键(§3.1)
        assertFalse(s.containsKey("cprim"))
        assertFalse(s.containsKey("ctrc"))
    }

    @Test
    fun proresBitrateClampedToFloor() {
        // 320x180 的面积折算远低于 50M 下限 → 夹到 50M
        val s = parse(
            SpecBuilder.buildSpec(spec(format = OutFormat.PRORES), facts(w = 320, h = 180))
        )
        assertEquals(50_000_000L, s.getl("bitrate"))
    }

    @Test
    fun p1080FitsLongEdgeAndUpsamplesSmallSource() {
        assertEquals(1920 to 1080, Pair(1920, 1080).let {
            parse(SpecBuilder.buildSpec(spec(scale = ScaleMode.P1080), facts(w = 3840, h = 2160)))
                .let { it.geti("w") to it.geti("h") }
        })
        // 小源上采样:640x360 → 1920x1080(「1080p」的字面语义)
        val up = parse(SpecBuilder.buildSpec(spec(scale = ScaleMode.P1080), facts(w = 640, h = 360)))
        assertEquals(1920, up.geti("w"))
        assertEquals(1080, up.geti("h"))
    }

    @Test
    fun rotationSwapsSourceSizeBeforeScaling() {
        // 竖拍 1080x1920 + rot=90:native 看到的是交换后的源(1920x1080)
        val s = parse(
            SpecBuilder.buildSpec(spec(scale = ScaleMode.P720), facts(w = 1080, h = 1920, rot = 90))
        )
        assertEquals(1280, s.geti("w"))
        assertEquals(720, s.geti("h"))
    }

    @Test
    fun percentScalesHalveAndRoundToEven() {
        // 1083/2=541 → 向下取偶 540;1921/2=960 已是偶数
        val s = parse(SpecBuilder.buildSpec(spec(scale = ScaleMode.P50), facts(w = 1083, h = 1921)))
        assertEquals(540, s.geti("w"))
        assertEquals(960, s.geti("h"))
    }

    @Test
    fun fpsExplicitOverridesFactsAndZipDefault() {
        assertEquals(25 to 1, parse(SpecBuilder.buildSpec(spec(fps = FpsMode.F25), facts()))
            .let { it.geti("fpsnum") to it.geti("fpsden") })
        // ORIG + facts 有帧率 → 跟随源
        val orig = parse(
            SpecBuilder.buildSpec(spec(), facts(fpsNum = 30000, fpsDen = 1001))
        )
        assertEquals(30000, orig.geti("fpsnum"))
        assertEquals(1001, orig.geti("fpsden"))
        // ORIG + zip 无容器帧率 → 固定 25
        val zip = parse(SpecBuilder.buildSpec(spec(), facts(codec = "dng", container = "zip")))
        assertEquals(25, zip.geti("fpsnum"))
        assertEquals(1, zip.geti("fpsden"))
    }

    @Test
    fun dngZipPreservesAudioModeAndAppendsZipKeys() {
        val copy = parse(
            SpecBuilder.buildSpec(
                spec(audio = AudioMode.COPY, dngQuality = DngQuality.FINE,
                    dngColor = DngColorMode.AUTO, dngMap = DngMap.APPLE_LOG),
                facts(codec = "dng", container = "zip"),
            )
        )
        assertEquals("copy", copy["audio"])
        assertEquals("dngzip", copy["src"])
        assertEquals("2", copy["dngq"])
        assertEquals("auto", copy["dngcolor"])
        assertEquals("applelog", copy["dngmap"])

        val aac = parse(
            SpecBuilder.buildSpec(
                spec(audio = AudioMode.AAC),
                facts(codec = "dng", container = "zip"),
            )
        )
        assertEquals("aac", aac["audio"])
    }

    @Test
    fun lutLinesWrittenOnlyWithLutPath() {
        val with = parse(SpecBuilder.buildSpec(spec(lutPath = "/data/luts/a.cube", lutIntensity = 0.5f), facts()))
        assertEquals("/data/luts/a.cube", with["lut"])
        assertEquals("0.5", with["lutint"])
        // 1.0 → "1"(去尾零);0.25 → "0.25"
        assertEquals("1", parse(SpecBuilder.buildSpec(spec(lutPath = "/a.cube"), facts()))["lutint"])
        assertEquals("0.25", parse(SpecBuilder.buildSpec(spec(lutPath = "/a.cube", lutIntensity = 0.25f), facts()))["lutint"])
    }

    @Test
    fun faststartFlagFollowsSpecExceptVp9() {
        assertEquals(0, parse(SpecBuilder.buildSpec(spec(faststart = false), facts())).geti("faststart"))
        assertEquals(1, parse(SpecBuilder.buildSpec(spec(faststart = true), facts())).geti("faststart"))
        assertEquals(0, parse(SpecBuilder.buildSpec(spec(format = OutFormat.VP9), facts())).geti("faststart"))
    }
}
