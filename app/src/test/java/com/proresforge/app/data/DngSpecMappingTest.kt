package com.proresforge.app.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * DNG 序列 zip 的 Kotlin → native spec 映射回归(契约 §10.3/§10.4/§10.6)。
 *
 * 这些映射是字符串契约:[com.proresforge.app.engine.FFmpeg.buildSpec] 把
 * `dngq=<档位序号>`、`dngcolor=<camera|auto|d65>` 塞进 spec,dngzip.cpp
 * 侧按同样的取值解析。谁改了枚举顺序/取值,这里先红。
 */
class DngSpecMappingTest {

    /** dngq 是档位序号 0/1/2(契约 §3),不能换成 label 文案。 */
    @Test
    fun dngQualitySpecValuesAreOrdinals() {
        assertEquals("0", DngQuality.FAST.specValue())
        assertEquals("1", DngQuality.BALANCED.specValue())
        assertEquals("2", DngQuality.FINE.specValue())
    }

    /** dngcolor 是固定小写字符串;新增档位要同步契约 §3 与 dngzip.cpp。 */
    @Test
    fun dngColorSpecValuesAreContractStrings() {
        assertEquals("camera", DngColorMode.CAMERA.specValue())
        assertEquals("auto", DngColorMode.AUTO.specValue())
        assertEquals("d65", DngColorMode.D65.specValue())
    }

    @Test
    fun dngMapSpecValuesAreContractStrings() {
        assertEquals("none", DngMap.NONE.specValue())
        assertEquals("applelog", DngMap.APPLE_LOG.specValue())
    }

    @Test
    fun dngMapOrderAndLabelsAreStable() {
        assertEquals(listOf(DngMap.NONE, DngMap.APPLE_LOG), DngMap.values().toList())
        val ids = DngMap.values().map { it.labelRes }
        assertEquals(ids.size, ids.distinct().size)
    }

    /**
     * 普通容器的帧率档位必须含「原始」(fps=0 跟随源),
     * 这是 HomeScreen 非 zip 分支 [FpsMode.standard] 的取值来源;
     * 新档位追加到末尾,不能打乱已有顺序(SegmentedRow 按下标取值)。
     */
    @Test
    fun standardFpsListKeepsOrigFirstAndNewModesLast() {
        assertEquals(FpsMode.ORIG, FpsMode.standard.first())
        assertTrue(FpsMode.standard.contains(FpsMode.F60))
        assertTrue(FpsMode.standard.contains(FpsMode.F30))
        assertTrue(FpsMode.standard.contains(FpsMode.F24))
        // labelRes 互不相同,否则 SegmentedRow 里两行字一样
        val ids = FpsMode.standard.map { it.labelRes }
        assertEquals(ids.size, ids.distinct().size)
    }

    /**
     * zip 帧率档位 = [FpsMode.zipOptions](UI 与测试共享同一份,L-22)。
     * 这里锁定「不变量」而不是具体顺序。
     */
    @Test
    fun zipFpsOptionsExcludeOrigAndInclude25() {
        val zipOptions = FpsMode.zipOptions
        assertFalse("zip 源不能选「原始」帧率", zipOptions.contains(FpsMode.ORIG))
        assertTrue(zipOptions.contains(FpsMode.F25))
        val ids = zipOptions.map { it.labelRes }
        assertEquals(ids.size, ids.distinct().size)
    }
}
