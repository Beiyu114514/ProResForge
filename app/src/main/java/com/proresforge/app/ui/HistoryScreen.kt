// HistoryScreen.kt — 历史记录页:读 JobStore 历史(文件 → 输出名、格式、状态 chip、体积/耗时/LUT),
// DONE 项可打开(ACTION_VIEW)/分享(ACTION_SEND),FAILED/CANCELLED 展示错误,顶部可清空。
package com.proresforge.app.ui

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.Toast
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.proresforge.app.R
import com.proresforge.app.data.JobStore
import com.proresforge.app.data.HistoryItem
import com.proresforge.app.data.JobState
import com.proresforge.app.data.OutFormat
import com.proresforge.app.engine.AppLog
import com.proresforge.app.ui.FactRow
import com.proresforge.app.ui.formatDurationMs
import com.proresforge.app.ui.formatSize
import kotlinx.coroutines.launch

@Composable
fun HistoryScreen(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val jobStore = remember { JobStore.get(context) }
    val history by jobStore.history.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val ordered = remember(history) { history.sortedByDescending { it.ts } }
    // S-2:开源许可弹窗入口(文本在 assets/licenses/,随包分发)
    var showLicenses by remember { mutableStateOf(false) }
    // 清空是破坏性操作:先确认,且只删除历史记录,不删除已导出的文件。
    var showClearConfirmation by remember { mutableStateOf(false) }

    Box(modifier.fillMaxSize()) {
        if (showLicenses) {
            LicensesDialog(onDismiss = { showLicenses = false })
        }
        if (showClearConfirmation) {
            AlertDialog(
                onDismissRequest = { showClearConfirmation = false },
                title = { Text(stringResource(R.string.history_clear_title)) },
                text = { Text(stringResource(R.string.history_clear_message)) },
                confirmButton = {
                    TextButton(
                        onClick = {
                            showClearConfirmation = false
                            scope.launch { jobStore.clear() }
                        },
                    ) {
                        Text(stringResource(R.string.history_clear_confirm))
                    }
                },
                dismissButton = {
                    TextButton(onClick = { showClearConfirmation = false }) {
                        Text(stringResource(R.string.history_clear_cancel))
                    }
                },
            )
        }
        if (ordered.isEmpty()) {
            Text(
                text = stringResource(R.string.history_empty),
                modifier = modifier.align(Alignment.Center),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        } else {
            // U-18:标题与操作行放在列表外(原先作为第一个 item,内容一滚就跟着消失)
            Column(Modifier.fillMaxSize()) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(start = 12.dp, end = 12.dp, top = 12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    Text(
                        text = stringResource(R.string.history_title),
                        style = MaterialTheme.typography.titleMedium,
                    )
                    Row {
                        TextButton(onClick = { shareLogs(context) }) {
                            Text(stringResource(R.string.btn_logs))
                        }
                        TextButton(onClick = { showLicenses = true }) {
                            Text(stringResource(R.string.btn_licenses))
                        }
                        TextButton(
                            onClick = { showClearConfirmation = true },
                        ) {
                            Text(stringResource(R.string.btn_clear_history))
                        }
                    }
                }
                LazyColumn(
                    modifier = Modifier.weight(1f),
                    contentPadding = PaddingValues(
                        start = 12.dp,
                        end = 12.dp,
                        top = 4.dp,
                        bottom = 12.dp,
                    ),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    items(ordered, key = { it.id }) { item ->
                        HistoryCard(
                            item = item,
                            onOpen = {
                                openOutputUri(context, item.outputUriString, item.format.mime)
                            },
                            onShare = {
                                shareOutputUri(
                                    context,
                                    item.outputUriString,
                                    item.format.mime,
                                    item.outputName,
                                )
                            },
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun HistoryCard(item: HistoryItem, onOpen: () -> Unit, onShare: () -> Unit) {
    Card {
        Column(Modifier.padding(12.dp)) {
            Text(
                text = item.inputName,
                style = MaterialTheme.typography.titleSmall,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                text = "→ ${item.outputName}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(6.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = formatLabel(item.format),
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.primary,
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    text = historyStateLabel(item.state),
                    style = MaterialTheme.typography.labelMedium,
                    color = when (item.state) {
                        JobState.DONE -> MaterialTheme.colorScheme.primary
                        JobState.FAILED, JobState.CANCELLED -> MaterialTheme.colorScheme.error
                        else -> MaterialTheme.colorScheme.onSurfaceVariant
                    },
                )
            }
            Spacer(Modifier.height(6.dp))
            FactRow(
                stringResource(R.string.fact_size),
                "${formatSize(item.inSize)} → ${formatSize(item.outSize)}",
            )
            FactRow(stringResource(R.string.fact_duration), formatDurationMs(item.durationMs))
            if (!item.lutName.isNullOrEmpty()) {
                FactRow(
                    stringResource(R.string.fact_lut),
                    stringResource(
                        R.string.fact_lut_value,
                        item.lutName.orEmpty(),
                        (item.lutIntensity * 100).toInt(),
                    ),
                )
            }
            val error = item.error
            if ((item.state == JobState.FAILED || item.state == JobState.CANCELLED) &&
                !error.isNullOrEmpty()
            ) {
                Text(
                    text = error,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }
            if (item.state == JobState.DONE) {
                Spacer(Modifier.height(8.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = onOpen) {
                        Text(stringResource(R.string.open))
                    }
                    OutlinedButton(onClick = onShare) {
                        Text(stringResource(R.string.share))
                    }
                }
            }
        }
    }
}

@Composable
private fun formatLabel(format: OutFormat): String = stringResource(format.labelRes)

@Composable
private fun historyStateLabel(state: JobState): String = stringResource(
    when (state) {
        JobState.QUEUED -> R.string.job_state_queued
        JobState.RUNNING -> R.string.job_state_running
        JobState.DONE -> R.string.job_state_done
        JobState.FAILED -> R.string.job_state_failed
        JobState.CANCELLED -> R.string.job_state_cancelled
    },
)

// U-18:打开/分享的实现已抽到 OutputActions.kt(任务页也要用,避免两份 Intent 代码)

/** 通过系统分享面板发送日志文件(FileProvider,见 AndroidManifest / res/xml/file_paths.xml)。 */
private fun shareLogs(context: Context) {
    val uris = AppLog.logShareUris(context)
    if (uris.isEmpty()) {
        toast(context, context.getString(R.string.toast_no_logs))
        return
    }
    val send = Intent(Intent.ACTION_SEND_MULTIPLE).apply {
        type = "text/plain"
        putParcelableArrayListExtra(Intent.EXTRA_STREAM, ArrayList(uris))
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }
    runCatching {
        context.startActivity(Intent.createChooser(send, context.getString(R.string.btn_logs)))
    }.onFailure { toast(context, context.getString(R.string.toast_share_failed)) }
}

private fun toast(context: Context, message: String) {
    Toast.makeText(context, message, Toast.LENGTH_SHORT).show()
}
