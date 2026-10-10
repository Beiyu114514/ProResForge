package com.proresforge.app.data

import android.content.Context

/**
 * 转码参数持久化(契约 §8.2):SharedPreferences("proresforge")。
 *
 * - 全部字段都有默认值(冷启动/换机后 UI 直接可用);
 * - 枚举按 name 存取,读到非法值( downgrade/升级后)回落到默认值;
 * - 强度 lutIntensity 收敛到 0f..1f;
 * - lastLutUri/lastLutName 由 LutManager.stage 成功时写入(契约 §8.2)。
 *
 * 注意:输出目录键 output_tree_uri 由 OutputDir 独占管理,这里不碰。
 */
class SettingsStore(context: Context) {

    private val prefs = context.applicationContext
        .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    /** 输出容器/编码格式。 */
    var format: OutFormat
        get() = enumOr(prefs.getString(KEY_FORMAT, null), OutFormat.H264)
        set(value) = prefs.edit().putString(KEY_FORMAT, value.name).apply()

    /** 画质档位。 */
    var tier: QualityTier
        get() = enumOr(prefs.getString(KEY_TIER, null), QualityTier.MEDIUM)
        set(value) = prefs.edit().putString(KEY_TIER, value.name).apply()

    /** 缩放模式。 */
    var scale: ScaleMode
        get() = enumOr(prefs.getString(KEY_SCALE, null), ScaleMode.ORIG)
        set(value) = prefs.edit().putString(KEY_SCALE, value.name).apply()

    /** 帧率模式。 */
    var fps: FpsMode
        get() = enumOr(prefs.getString(KEY_FPS, null), FpsMode.ORIG)
        set(value) = prefs.edit().putString(KEY_FPS, value.name).apply()

    /** 音频策略。 */
    var audio: AudioMode
        get() = enumOr(prefs.getString(KEY_AUDIO, null), AudioMode.COPY)
        set(value) = prefs.edit().putString(KEY_AUDIO, value.name).apply()

    /** 10bit(仅 H265/ProRes 有意义,UI 负责按 FormatSpec.supports10Bit 置灰)。 */
    var tenBit: Boolean
        get() = prefs.getBoolean(KEY_TENBIT, false)
        set(value) = prefs.edit().putBoolean(KEY_TENBIT, value).apply()

    /** moov 前置(MP4/MOV 有效)。 */
    var faststart: Boolean
        get() = prefs.getBoolean(KEY_FASTSTART, true)
        set(value) = prefs.edit().putBoolean(KEY_FASTSTART, value).apply()

    /** LUT 强度 0f..1f。 */
    var lutIntensity: Float
        get() = prefs.getFloat(KEY_LUT_INT, 1f).coerceIn(0f, 1f)
        set(value) = prefs.edit().putFloat(KEY_LUT_INT, value.coerceIn(0f, 1f)).apply()

    /** DNG 序列 zip 解raw 档位(契约 §10.4;普通容器不适用)。 */
    var dngQuality: DngQuality
        get() = enumOr(prefs.getString(KEY_DNGQ, null), DngQuality.BALANCED)
        set(value) = prefs.edit().putString(KEY_DNGQ, value.name).apply()

    /** DNG 序列 zip 白平衡策略(契约 §10.4)。 */
    var dngColor: DngColorMode
        get() = enumOr(prefs.getString(KEY_DNGCOLOR, null), DngColorMode.CAMERA)
        set(value) = prefs.edit().putString(KEY_DNGCOLOR, value.name).apply()

    /** DNG 序列 zip 色彩映射。 */
    var dngMap: DngMap
        get() = enumOr(prefs.getString(KEY_DNGMAP, null), DngMap.NONE)
        set(value) = prefs.edit().putString(KEY_DNGMAP, value.name).apply()

    /** 最近一次成功暂存的 LUT uri。 */
    var lastLutUri: String?
        get() = prefs.getString(KEY_LUT_URI, null)
        set(value) = prefs.edit().putString(KEY_LUT_URI, value).apply()

    /** 最近一次成功暂存的 LUT 名称。 */
    var lastLutName: String?
        get() = prefs.getString(KEY_LUT_NAME, null)
        set(value) = prefs.edit().putString(KEY_LUT_NAME, value).apply()

    private companion object {
        const val PREFS_NAME = "proresforge"
        const val KEY_FORMAT = "format"
        const val KEY_TIER = "tier"
        const val KEY_SCALE = "scale"
        const val KEY_FPS = "fps"
        const val KEY_AUDIO = "audio"
        const val KEY_TENBIT = "tenbit"
        const val KEY_FASTSTART = "faststart"
        const val KEY_LUT_INT = "lut_intensity"
        const val KEY_LUT_URI = "lut_uri"
        const val KEY_LUT_NAME = "lut_name"
        const val KEY_DNGQ = "dng_quality"
        const val KEY_DNGCOLOR = "dng_color"
        const val KEY_DNGMAP = "dng_map"
    }
}

/** 按 name 解析枚举;null/非法值返回 [def]。 */
private inline fun <reified T : Enum<T>> enumOr(name: String?, def: T): T =
    if (name.isNullOrEmpty()) {
        def
    } else {
        try {
            enumValueOf<T>(name)
        } catch (e: IllegalArgumentException) {
            def
        }
    }
