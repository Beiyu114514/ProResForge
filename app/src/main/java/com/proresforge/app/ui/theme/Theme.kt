// Theme.kt — 应用 Compose 主题:ProResForgeTheme 跟随系统深色模式,
// 深色为主:深蓝黑底 + 橙色点缀;typography 用 Material3 默认;系统栏由 MainActivity
// 的 enableEdgeToEdge() 处理(U-01),这里同时补全 M3 色角色(U-02/U-03/U-13)。
package com.proresforge.app.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

// 品牌色:深蓝黑(底)+ 橙(点缀),与 values/colors.xml 的 XML 主题同源
private val BrandOrange = Color(0xFFFF8A3D)
private val BrandOrangePressed = Color(0xFFE0742A)
private val BrandOnOrange = Color(0xFF24150A)
private val BrandInk = Color(0xFF0B1220) // 深蓝黑背景
private val BrandSurface = Color(0xFF121B2E)
private val BrandSurfaceVariant = Color(0xFF1C2942)
private val BrandOnSurface = Color(0xFFE7ECF5)
private val BrandOnSurfaceVariant = Color(0xFFB7C1D6)
private val BrandAccentBlue = Color(0xFF7FA6FF)
private val BrandInkLight = Color(0xFFF5F7FC)
private val BrandSurfaceLight = Color(0xFFFFFFFF)
private val BrandSurfaceVariantLight = Color(0xFFE7EAF3)
// U-02/U-13:浅色强调色加深到白字 ≥4.5:1(原 #E0742A 仅 3.13:1);
// 描边色两态都提到 ≥3:1(原浅 #C6CBD8=1.62:1、深 #33425F=1.71:1)。
private val BrandOrangeDeep = Color(0xFFB4531A)
private val BrandOutlineLight = Color(0xFF7C879C)
private val BrandOutlineDark = Color(0xFF5F7092)
// U-03:补齐 M3 基线里没被覆盖的角色(inverse* 供 Snackbar 等组件使用)
private val DarkInverseSurface = Color(0xFFE7ECF5)
private val DarkInverseOnSurface = Color(0xFF1A2233)
private val DarkInversePrimary = Color(0xFF8A4A16)
private val LightInverseSurface = Color(0xFF2B3140)
private val LightInverseOnSurface = Color(0xFFF1F3F8)
private val LightInversePrimary = Color(0xFFFFB68A)

private val DarkColors = darkColorScheme(
    primary = BrandOrange,
    onPrimary = BrandOnOrange,
    primaryContainer = BrandOrangePressed,
    onPrimaryContainer = Color.White,
    secondary = BrandAccentBlue,
    onSecondary = BrandOnOrange,
    background = BrandInk,
    onBackground = BrandOnSurface,
    surface = BrandSurface,
    onSurface = BrandOnSurface,
    surfaceVariant = BrandSurfaceVariant,
    onSurfaceVariant = BrandOnSurfaceVariant,
    error = Color(0xFFFF6B6B),
    onError = Color(0xFF3B0A0A),
    outline = BrandOutlineDark,
    // U-03:未覆盖的 M3 角色会落回基线配色(选中 FilterChip 的淡紫 #E8DEF8 即由此来)
    secondaryContainer = Color(0xFF24365C),
    onSecondaryContainer = Color(0xFFD9E4FF),
    inverseSurface = DarkInverseSurface,
    inverseOnSurface = DarkInverseOnSurface,
    inversePrimary = DarkInversePrimary,
    surfaceContainerLowest = Color(0xFF0B1220),
    surfaceContainerLow = Color(0xFF101828),
    surfaceContainer = Color(0xFF162034),
    surfaceContainerHigh = BrandSurfaceVariant,
    surfaceContainerHighest = Color(0xFF22304D),
)

private val LightColors = lightColorScheme(
    // U-02:#E0742A 作白字背景只有 3.13:1,加深到 #B4531A(白字 ≈4.9:1,达 AA)
    primary = BrandOrangeDeep,
    onPrimary = Color.White,
    primaryContainer = Color(0xFFFFDCC6),
    onPrimaryContainer = Color(0xFF3A1E04),
    secondary = Color(0xFF2C5FD1),
    onSecondary = Color.White,
    background = BrandInkLight,
    onBackground = Color(0xFF151A26),
    surface = BrandSurfaceLight,
    onSurface = Color(0xFF151A26),
    surfaceVariant = BrandSurfaceVariantLight,
    onSurfaceVariant = Color(0xFF474F63),
    error = Color(0xFFB3261E),
    onError = Color.White,
    outline = BrandOutlineLight,
    // U-03:同深色,补齐会被基线配色顶替的角色(否则选中态落回淡紫)
    secondaryContainer = Color(0xFFFFDCC6),
    onSecondaryContainer = Color(0xFF3A1E04),
    inverseSurface = LightInverseSurface,
    inverseOnSurface = LightInverseOnSurface,
    inversePrimary = LightInversePrimary,
    surfaceContainerLowest = Color(0xFFFFFFFF),
    surfaceContainerLow = Color(0xFFFAFBFE),
    surfaceContainer = Color(0xFFF4F6FB),
    surfaceContainerHigh = Color(0xFFEEF1F8),
    surfaceContainerHighest = BrandSurfaceVariantLight,
)

/**
 * 应用主题。darkTheme 默认跟随系统;系统栏交给 edge-to-edge(MainActivity 的
 * enableEdgeToEdge(),见 U-01),Compose 侧不再自行绘制系统栏背景。
 */
@Composable
fun ProResForgeTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit,
) {
    MaterialTheme(
        colorScheme = if (darkTheme) DarkColors else LightColors,
        typography = MaterialTheme.typography,
        content = content,
    )
}
