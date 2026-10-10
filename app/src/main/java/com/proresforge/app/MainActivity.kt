// MainActivity.kt — 应用唯一入口 Activity:挂载 ProResForgeTheme{AppNav()}。
package com.proresforge.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.proresforge.app.ui.AppNav
import com.proresforge.app.ui.theme.ProResForgeTheme

class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        // U-01:显式声明 edge-to-edge(Android 15 / targetSdk 35 下系统本就强制),
        // 系统栏图标明暗交给 enableEdgeToEdge 按系统深浅色决定;此前 XML 主题把
        // windowLightStatusBar 写死 false,浅色模式下白图标叠白底 = 状态栏完全看不见。
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        setContent {
            ProResForgeTheme {
                AppNav()
            }
        }
    }
}
