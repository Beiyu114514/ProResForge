// AppNav.kt — 应用外壳:顶部标题栏+ 底部三 Tab(转码/任务/历史),
// 用普通 remember 状态切页,不引入 navigation-compose 依赖。
package com.proresforge.app.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.DateRange
import androidx.compose.material.icons.filled.List
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import com.proresforge.app.R

private data class NavItem(val labelRes: Int, val icon: ImageVector, val index: Int)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AppNav() {
    // rememberSaveable:旋转/进程死亡后还停在用户上次看的 Tab(remember 会弹回转码页)
    var tab by rememberSaveable { mutableStateOf(0) }
    // U-04:字体缩放 ≥1.3 倍时标题栏只显示应用名(见下方 TopAppBar)
    val fontScale = LocalDensity.current.fontScale
    // U-11:转码入队后 Snackbar 的「查看任务」动作 → 切到任务 Tab
    val openJobs: () -> Unit = { tab = 1 }
    val items = listOf(
        NavItem(R.string.tab_transcode, Icons.Default.PlayArrow, 0),
        NavItem(R.string.tab_jobs, Icons.Default.List, 1),
        NavItem(R.string.tab_history, Icons.Default.DateRange, 2),
    )

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    if (tab == 0) {
                        Column {
                            Text(
                                text = stringResource(R.string.app_name),
                                style = MaterialTheme.typography.titleLarge,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                            // U-04:字体放大后两行标题会撑破 TopAppBar(实测 2.0 倍时第二行被硬裁),
                            // 空间不够时只留应用名,避免出现被切掉一半的文字。
                            if (fontScale < 1.3f) {
                                Text(
                                    text = stringResource(R.string.app_tagline),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                )
                            }
                        }
                    } else {
                        Text(
                            text = stringResource(
                                if (tab == 1) R.string.tab_jobs else R.string.tab_history,
                            ),
                            style = MaterialTheme.typography.titleLarge,
                        )
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface,
                ),
            )
        },
        bottomBar = {
            NavigationBar {
                items.forEach { item ->
                    NavigationBarItem(
                        selected = tab == item.index,
                        onClick = { tab = item.index },
                        icon = {
                            Icon(
                                item.icon,
                                contentDescription = stringResource(item.labelRes),
                            )
                        },
                        label = { Text(stringResource(item.labelRes)) },
                    )
                }
            }
        },
    ) { innerPadding ->
        Box(Modifier.padding(innerPadding)) {
            when (tab) {
                0 -> HomeScreen(onOpenJobs = openJobs)
                1 -> JobsScreen()
                else -> HistoryScreen()
            }
        }
    }
}
