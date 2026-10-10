// components.kt — 转码/任务/历史三页共享的通用 UI 组件(SectionCard/LabeledSlider/
// SegmentedRow/FactRow/ProgressRow)与本地化格式工具(体积、时长、时钟)。
package com.proresforge.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.proresforge.app.R
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** 一个带标题/副标题的卡片区块,content 置于卡片内部纵列。 */
@Composable
fun SectionCard(
    title: String,
    subtitle: String? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 6.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
    ) {
        Column(Modifier.padding(16.dp)) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onSurface,
            )
            if (subtitle != null) {
                Text(
                    text = subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Spacer(Modifier.height(12.dp))
            content()
        }
    }
}

/** 滑块 + 左标签右数值:LUT 强度等处使用。steps 为档位数(0 = 连续)。 */
@Composable
fun LabeledSlider(
    label: String,
    value: Float,
    valueRange: ClosedFloatingPointRange<Float>,
    steps: Int = 0,
    onChange: (Float) -> Unit,
    valueText: String,
) {
    Column(Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = label,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurface,
            )
            Text(
                text = valueText,
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.primary,
            )
        }
        Slider(
            value = value,
            onValueChange = onChange,
            valueRange = valueRange,
            steps = steps,
        )
    }
}

/** 单选 chip 行:格式/质量/缩放/帧率/音频等枚举选择。FlowRow 允许换行,避免横排溢出。 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun SegmentedRow(
    options: List<String>,
    selectedIndex: Int,
    onSelect: (Int) -> Unit,
) {
    FlowRow(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        options.forEachIndexed { index, option ->
            FilterChip(
                selected = index == selectedIndex,
                onClick = { onSelect(index) },
                label = { Text(option) },
            )
        }
    }
}

/** 两列文本行:左标签(定宽小号灰字),右值(正文色,可多行截断)。 */
@Composable
fun FactRow(label: String, value: String, warn: Boolean = false) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 2.dp),
        verticalAlignment = Alignment.Top,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.widthIn(min = 84.dp),
        )
        // U-16:标签与值之间留 8dp(原先只有 widthIn 的下限,大字体下会贴在一起)
        Spacer(Modifier.width(8.dp))
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium,
            color = if (warn) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface,
            maxLines = 3,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

/** 进度行:pct >= 0 显示确定进度条,pct == -1 显示不确定进度条;detail 为阶段/帧数文字。 */
@Composable
fun ProgressRow(pct: Int, detail: String) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 6.dp),
    ) {
        if (pct in 0..100) {
            LinearProgressIndicator(
                progress = { pct / 100f },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(6.dp),
                strokeCap = StrokeCap.Round,
            )
        } else {
            LinearProgressIndicator(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(6.dp),
                strokeCap = StrokeCap.Round,
            )
        }
        Spacer(Modifier.height(4.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(
                text = detail,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                text = if (pct in 0..100) "$pct%" else stringResource(R.string.progress_indeterminate),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
            )
        }
    }
}

/** 字节数 → "12.3 KB / 4.5 MB / 1.02 GB";未知(<=0)显示 "-"。 */
fun formatSize(bytes: Long): String = when {
    bytes <= 0L -> "-"
    bytes < 1024L * 1024L -> "%.1f KB".format(Locale.US, bytes / 1024.0)
    bytes < 1024L * 1024L * 1024L -> "%.1f MB".format(Locale.US, bytes / 1024.0 / 1024.0)
    else -> "%.2f GB".format(Locale.US, bytes / 1024.0 / 1024.0 / 1024.0)
}
/** 毫秒 → "m:ss" 或 "h:mm:ss"。 */
fun formatDurationMs(ms: Long): String {
    if (ms <= 0) return "-"
    val totalSec = ms / 1000
    val h = totalSec / 3600
    val m = totalSec % 3600 / 60
    val s = totalSec % 60
    return if (h > 0) {
        "%d:%02d:%02d".format(Locale.US, h, m, s)
    } else {
        "%d:%02d".format(Locale.US, m, s)
    }
}

/** 微秒(MediaFacts.durationUs)→ 时长文本。 */
fun formatDurationUs(us: Long): String = if (us > 0) formatDurationMs(us / 1000) else "-"

/** epoch 毫秒 → 本地 "HH:mm:ss"。 */
fun formatClock(epochMs: Long): String =
    if (epochMs <= 0) "-"
    else SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(Date(epochMs))
