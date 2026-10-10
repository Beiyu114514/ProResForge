// JobsScreen.kt — 任务队列页:观察 JobBus.jobs / currentId,展示每个 JobRecord 的进度条、
// 阶段中文映射、时间与错误;QUEUED/RUNNING 可取消,底部一键清除已完成。
package com.proresforge.app.ui

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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.proresforge.app.R
import com.proresforge.app.data.JobRecord
import com.proresforge.app.data.JobState
import com.proresforge.app.engine.JobBus
import com.proresforge.app.ui.FactRow
import com.proresforge.app.ui.ProgressRow
import com.proresforge.app.ui.formatClock
import com.proresforge.app.ui.formatDurationMs
import com.proresforge.app.ui.formatSize

@Composable
fun JobsScreen(modifier: Modifier = Modifier) {
    val jobs by JobBus.jobs.collectAsStateWithLifecycle()
    val currentId by JobBus.currentId.collectAsStateWithLifecycle()
    val ordered = remember(jobs) {
        jobs.values.sortedWith(compareBy({ stateOrder(it.state) }, { it.startedAt }))
    }
    val hasFinished = jobs.values.any {
        it.state == JobState.DONE || it.state == JobState.FAILED || it.state == JobState.CANCELLED
    }

    Box(modifier.fillMaxSize()) {
        if (jobs.isEmpty()) {
            Text(
                text = stringResource(R.string.jobs_empty),
                modifier = modifier.align(Alignment.Center),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        } else {
            LazyColumn(
                contentPadding = PaddingValues(12.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                items(ordered, key = { it.id }) { job ->
                    JobCard(job = job, isCurrent = job.id == currentId)
                }
                if (hasFinished) {
                    item {
                        Button(
                            onClick = { JobBus.clearFinished() },
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            Text(stringResource(R.string.btn_clear_finished))
                        }
                    }
                }
            }
        }
    }
}

/** 排序权重:进行中 → 排队 → 失败/取消 → 完成,组内按开始时间。 */
private fun stateOrder(state: JobState): Int = when (state) {
    JobState.RUNNING -> 0
    JobState.QUEUED -> 1
    JobState.FAILED -> 2
    JobState.CANCELLED -> 3
    JobState.DONE -> 4
}

@Composable
private fun JobCard(job: JobRecord, isCurrent: Boolean) {
    // U-11:打开/分享输出文件需要 Context
    val context = LocalContext.current
    Card {
        Column(Modifier.padding(12.dp)) {
            // 行 1:文件名 + 格式/输出名 + 取消
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(
                        text = job.spec.inputName,
                        style = MaterialTheme.typography.titleSmall,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Text(
                        text = "${stringResource(job.spec.format.labelRes)} · ${job.spec.outputName}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
                if (job.state == JobState.QUEUED || job.state == JobState.RUNNING) {
                    TextButton(onClick = { JobBus.cancel(job.id) }) {
                        Text(stringResource(R.string.btn_cancel))
                    }
                }
            }
            Spacer(Modifier.height(6.dp))
            // 行 2:状态(含当前任务标记)
            val stateText = if (isCurrent) {
                stringResource(R.string.job_state_current, jobStateLabel(job.state))
            } else {
                jobStateLabel(job.state)
            }
            Text(
                text = stateText,
                style = MaterialTheme.typography.labelLarge,
                color = when (job.state) {
                    JobState.RUNNING -> MaterialTheme.colorScheme.primary
                    JobState.FAILED, JobState.CANCELLED -> MaterialTheme.colorScheme.error
                    else -> MaterialTheme.colorScheme.onSurfaceVariant
                },
            )
            // 行 3:进行中的任务显示进度;终态改用状态与错误文案,避免取消任务显示 100%。
            if (job.state == JobState.QUEUED || job.state == JobState.RUNNING) {
                ProgressRow(
                    pct = job.pct,
                    detail = "${stageLabel(job.stage)} · ${frameText(job.frame, job.totalFrames)}",
                )
            }
            FactRow(stringResource(R.string.fact_started_at), formatClock(job.startedAt))
            if (job.state == JobState.DONE) {
                FactRow(stringResource(R.string.fact_duration), formatDurationMs(job.durationMs))
                FactRow(
                    stringResource(R.string.fact_size),
                    "${formatSize(job.inSize)} → ${formatSize(job.outSize)}",
                )
                // U-11:完成后就地打开/分享,不必再切到历史页找同一条记录
                Spacer(Modifier.height(8.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = {
                        openOutputUri(context, job.spec.outputUri.toString(), job.spec.format.mime)
                    }) {
                        Text(stringResource(R.string.open))
                    }
                    OutlinedButton(onClick = {
                        shareOutputUri(
                            context,
                            job.spec.outputUri.toString(),
                            job.spec.format.mime,
                            job.spec.outputName,
                        )
                    }) {
                        Text(stringResource(R.string.share))
                    }
                }
            }
            val error = job.error
            if ((job.state == JobState.FAILED || job.state == JobState.CANCELLED) &&
                !error.isNullOrEmpty()
            ) {
                Text(
                    text = error,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }
        }
    }
}

@Composable
private fun jobStateLabel(state: JobState): String = stringResource(
    when (state) {
        JobState.QUEUED -> R.string.job_state_queued
        JobState.RUNNING -> R.string.job_state_running
        JobState.DONE -> R.string.job_state_done
        JobState.FAILED -> R.string.job_state_failed
        JobState.CANCELLED -> R.string.job_state_cancelled
    },
)

/** 契约 §5 STAGES 的本地化映射(UI 侧负责);queued 为 JobBus 未开始任务的初始 stage。 */
@Composable
private fun stageLabel(stage: String): String = when (stage) {
    "queued" -> stringResource(R.string.stage_queued)
    "init" -> stringResource(R.string.stage_init)
    "decode" -> stringResource(R.string.stage_decode)
    "audio" -> stringResource(R.string.stage_audio)
    "finalize" -> stringResource(R.string.stage_finalize)
    "done" -> stringResource(R.string.stage_done)
    "error" -> stringResource(R.string.stage_error)
    // JobBus 取消终态的 stage;没有分支时会直接显示英文原名
    "cancelled" -> stringResource(R.string.job_state_cancelled)
    else -> stage
}

private fun frameText(frame: Long, totalFrames: Long): String =
    if (totalFrames > 0) "$frame/$totalFrames" else "-"
