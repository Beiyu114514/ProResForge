package com.proresforge.app.data

/**
 * 输出格式参数表(契约 §8.1 / §3.2)。
 * vcodec 为「首选编码器名」,C 端按 §3.2 fallback 链逐个尝试:
 *   h264_mediacodec → libx264,hevc_mediacodec → libx265,libvpx-vp9 / prores_ks 无回退。
 */
object FormatCatalog {

    private val specs: List<FormatSpec> = listOf(
        FormatSpec(
            format = OutFormat.H264,
            vcodec = "h264_mediacodec",
            pixFmt = "yuv420p",
            supports10Bit = false,
            audioModes = listOf(AudioMode.COPY, AudioMode.AAC),
            crfHigh = 18, crfMedium = 23, crfLow = 28,
            vp9CqHigh = 0, vp9CqMedium = 0, vp9CqLow = 0
        ),
        FormatSpec(
            format = OutFormat.H265,
            vcodec = "hevc_mediacodec",
            pixFmt = "yuv420p",
            supports10Bit = true,
            audioModes = listOf(AudioMode.COPY, AudioMode.AAC),
            crfHigh = 20, crfMedium = 25, crfLow = 30,
            vp9CqHigh = 0, vp9CqMedium = 0, vp9CqLow = 0
        ),
        FormatSpec(
            format = OutFormat.VP9,
            vcodec = "libvpx-vp9",
            pixFmt = "yuv420p",
            supports10Bit = false,
            audioModes = listOf(AudioMode.COPY, AudioMode.AAC),
            crfHigh = 0, crfMedium = 0, crfLow = 0,
            vp9CqHigh = 20, vp9CqMedium = 32, vp9CqLow = 40
        ),
        FormatSpec(
            format = OutFormat.PRORES,
            vcodec = "prores_ks",
            pixFmt = "yuv422p10le",
            // ProRes 输出恒为 10bit(由 pixfmt 承载),没有可选开关
            supports10Bit = false,
            // quicktime 支持 aac/pcm;copy 仅在源音频被 mov 容器接受时有效,引擎侧自动降级
            audioModes = listOf(AudioMode.COPY, AudioMode.AAC),
            // prores_ks 无 CRF,三档字段为 0、被忽略
            crfHigh = 0, crfMedium = 0, crfLow = 0,
            vp9CqHigh = 0, vp9CqMedium = 0, vp9CqLow = 0
        )
    )

    /** 取某个格式的参数画像;不存在时抛 IllegalArgumentException(枚举即键,不会发生)。 */
    fun spec(format: OutFormat): FormatSpec = specs.first { it.format == format }

    /** 四个格式的完整列表(UI 用来铺选项)。 */
    fun all(): List<FormatSpec> = specs
}
