package com.proresforge.app.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * FormatCatalog 参数表回归(契约 §8.1 / §3.2)。
 * 纯 JVM 单元测试:不碰 Android、不碰 native,改表立刻红。
 */
class FormatCatalogTest {

    /** 表中必须恰好覆盖四个输出格式,且顺序稳定(UI 选项顺序即此顺序)。 */
    @Test
    fun allCoversExactlyFourFormatsInOrder() {
        assertEquals(
            listOf(OutFormat.H264, OutFormat.H265, OutFormat.VP9, OutFormat.PRORES),
            FormatCatalog.all().map { it.format },
        )
    }

    @Test
    fun specMatchesEnumKey() {
        for (format in OutFormat.entries) {
            assertEquals(format, FormatCatalog.spec(format).format)
        }
    }

    @Test
    fun preferredVcodecPerContract() {
        assertEquals("h264_mediacodec", FormatCatalog.spec(OutFormat.H264).vcodec)
        assertEquals("hevc_mediacodec", FormatCatalog.spec(OutFormat.H265).vcodec)
        assertEquals("libvpx-vp9", FormatCatalog.spec(OutFormat.VP9).vcodec)
        assertEquals("prores_ks", FormatCatalog.spec(OutFormat.PRORES).vcodec)
    }

    @Test
    fun pixFmtPerContract() {
        assertEquals("yuv420p", FormatCatalog.spec(OutFormat.H264).pixFmt)
        assertEquals("yuv420p", FormatCatalog.spec(OutFormat.H265).pixFmt)
        assertEquals("yuv420p", FormatCatalog.spec(OutFormat.VP9).pixFmt)
        // ProRes 输出恒为 10bit 4:2:2
        assertEquals("yuv422p10le", FormatCatalog.spec(OutFormat.PRORES).pixFmt)
    }

    @Test
    fun tenBitOnlySelectableForH265() {
        assertFalse(FormatCatalog.spec(OutFormat.H264).supports10Bit)
        assertTrue(FormatCatalog.spec(OutFormat.H265).supports10Bit)
        assertFalse(FormatCatalog.spec(OutFormat.VP9).supports10Bit)
        // ProRes 输出恒为 10bit(pixfmt 承载),但 UI 上没有可切开关
        assertFalse(FormatCatalog.spec(OutFormat.PRORES).supports10Bit)
    }

    /** CRF 三档必须单调:HIGH(最好)数值最小 → LOW(最差)数值最大。 */
    @Test
    fun crfTiersAreOrderedHighBelowMediumBelowLow() {
        for (format in listOf(OutFormat.H264, OutFormat.H265)) {
            val spec = FormatCatalog.spec(format)
            assertTrue("$format crfHigh<crfMedium", spec.crfHigh < spec.crfMedium)
            assertTrue("$format crfMedium<crfLow", spec.crfMedium < spec.crfLow)
        }
    }

    /** VP9 走 CQ 档而不是 CRF,同样要求 high < medium < low。 */
    @Test
    fun vp9CqTiersAreOrderedHighBelowMediumBelowLow() {
        val spec = FormatCatalog.spec(OutFormat.VP9)
        assertTrue("vp9CqHigh<vp9CqMedium", spec.vp9CqHigh < spec.vp9CqMedium)
        assertTrue("vp9CqMedium<vp9CqLow", spec.vp9CqMedium < spec.vp9CqLow)
    }

    /** ProRes(prores_ks)没有 CRF/CQ,三档字段必须全 0(否则引擎会按 0 误设参数)。 */
    @Test
    fun proresHasNoRateQualityTiers() {
        val spec = FormatCatalog.spec(OutFormat.PRORES)
        assertEquals(0, spec.crfHigh)
        assertEquals(0, spec.crfMedium)
        assertEquals(0, spec.crfLow)
    }

    /** 用错字段是大坑:H264/H265 有 CRF 就不该有 CQ,VP9 反之。 */
    @Test
    fun exactlyOneQualitySchemePerFormat() {
        fun hasCrf(format: OutFormat): Boolean {
            val s = FormatCatalog.spec(format)
            return s.crfHigh > 0 && s.crfMedium > 0 && s.crfLow > 0
        }

        fun hasCq(format: OutFormat): Boolean {
            val s = FormatCatalog.spec(format)
            return s.vp9CqHigh > 0 && s.vp9CqMedium > 0 && s.vp9CqLow > 0
        }

        assertTrue(hasCrf(OutFormat.H264))
        assertFalse(hasCq(OutFormat.H264))

        assertTrue(hasCrf(OutFormat.H265))
        assertFalse(hasCq(OutFormat.H265))

        assertFalse(hasCrf(OutFormat.VP9))
        assertTrue(hasCq(OutFormat.VP9))

        assertFalse(hasCrf(OutFormat.PRORES))
        assertFalse(hasCq(OutFormat.PRORES))
    }

    /** 每个格式都要能 copy/aac 音频;DROP 由引擎自行降级,不进表。 */
    @Test
    fun audioModesAlwaysContainCopyAndAac() {
        for (spec in FormatCatalog.all()) {
            assertTrue("${spec.format} copy", spec.audioModes.contains(AudioMode.COPY))
            assertTrue("${spec.format} aac", spec.audioModes.contains(AudioMode.AAC))
            assertFalse("${spec.format} drop", spec.audioModes.contains(AudioMode.DROP))
        }
    }

    /**
     * labelRes 是 strings 资源 id,枚举常量顺序改了就可能串显示。
     * 这里只锁住「四个格式互不相同」这一条(具体文案由 UI/资源审查覆盖)。
     */
    @Test
    fun labelResAreDistinct() {
        val ids = OutFormat.entries.map { it.labelRes }
        assertEquals(ids.size, ids.distinct().size)
    }

    /** 容器/扩展名/MIME 必须自洽:VP9→webm,ProRes→mov,H265 仍走 mp4。 */
    @Test
    fun extAndMimeMatchContainerSemantics() {
        assertEquals("mp4", OutFormat.H264.ext)
        assertEquals("video/mp4", OutFormat.H264.mime)
        assertEquals("mp4", OutFormat.H265.ext)
        assertEquals("video/mp4", OutFormat.H265.mime)
        assertEquals("webm", OutFormat.VP9.ext)
        assertEquals("video/webm", OutFormat.VP9.mime)
        assertEquals("mov", OutFormat.PRORES.ext)
        assertEquals("video/quicktime", OutFormat.PRORES.mime)
    }
}
