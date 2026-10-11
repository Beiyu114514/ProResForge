// LicensesScreen.kt — 开源许可弹窗(S-2):从 assets/licenses/ 读取 NOTICE.md 与
// 各第三方许可证全文,随包分发(README §6 的自述承诺:「发布二进制时应随附
// 许可证文本、版权声明和源码获取方式」)。
//
// 文件清单与顺序:NOTICE.md(总述 + 源码 offer)固定在最前,其余按文件名排序
// (Apache-2.0 / BSD-3-Clause / CDDL-1.0 / GPL-2.0 / GPL-3.0 / LGPL-2.1 / Zlib)。
// 加新组件时:文本丢进 assets/licenses/ 并在 NOTICE.md 里补一节,这里无需改代码。
package com.proresforge.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import com.proresforge.app.R
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private const val LICENSES_ASSETS_DIR = "licenses"

/** 一个许可证文本文件(文件名去掉扩展名作标题)。 */
private data class LicenseDoc(val title: String, val body: String)
/** U-17:LazyColumn 的扁平行(文档标题 / 正文一行)。 */
private sealed interface LicenseRow {
    data class Title(val text: String) : LicenseRow
    data class Line(val text: String) : LicenseRow
}

/** 文档 → 扁平行;跳过空行与分隔线,并去掉 Markdown 记号(标题 # 号、列表符号、引用 > 号、表格竖线,
 *  以及行内记号:**粗体**、`代码`、[文字](链接) —— U-17 遗留:行内记号在纯文本渲染里只剩噪音)。 */
private fun stripInlineMarkdown(s: String): String = s
    .replace(Regex("\\[([^\\]]+)]\\(([^)]+)\\)")) { m -> "${m.groupValues[1]} (${m.groupValues[2]})" }
    .replace("**", "")
    .replace("__", "")
    .replace("`", "")

private fun List<LicenseDoc>.toRows(): List<LicenseRow> = flatMap { doc ->
    buildList {
        add(LicenseRow.Title(doc.title))
        doc.body.lineSequence().forEach { raw ->
            val line = raw.trimEnd()
            val text = line.trim()
            if (text.isEmpty()) return@forEach
            when {
                text.all { ch -> ch == '-' || ch == '*' || ch == '_' } -> Unit
                text.startsWith("|") || text.endsWith("|") ->
                    if (text.trim('|', '-', ':', ' ').isEmpty()) Unit
                    else add(LicenseRow.Line(stripInlineMarkdown(text.trim('|').split('|').joinToString("   ") { cell -> cell.trim() })))
                text.startsWith("#") -> add(LicenseRow.Title(stripInlineMarkdown(text.trimStart('#').trim())))
                text.startsWith(">") -> add(LicenseRow.Line(stripInlineMarkdown(text.trimStart('>').trim())))
                else -> add(LicenseRow.Line(stripInlineMarkdown(line.removePrefix("- ").removePrefix("* ").removePrefix("+ "))))
            }
        }
    }
}

@Composable
fun LicensesDialog(onDismiss: () -> Unit) {
    val context = LocalContext.current
    var docs by remember { mutableStateOf<List<LicenseDoc>?>(null) }
    // U-17:正文按行拆成扁平行;原先「一个 item 里两个 Text」会让整份许可证文本
    // (GPL 全文上万行)一次测量,弹窗打开明显卡顿。
    val rows = remember(docs) { docs?.toRows() }

    // assets 读取是 IO;弹窗打开时读一次,失败给可操作的错误文案(不静默白屏)
    LaunchedEffect(Unit) {
        docs = withContext(Dispatchers.IO) {
            runCatching {
                val names = context.assets.list(LICENSES_ASSETS_DIR).orEmpty()
                    .filter { it.endsWith(".txt") || it.endsWith(".md") }
                    .sortedWith(
                        compareByDescending<String> { it.equals("NOTICE.md", ignoreCase = true) }
                            .thenBy { it }
                    )
                names.map { name ->
                    val body = context.assets.open("$LICENSES_ASSETS_DIR/$name")
                        .use { it.readBytes().toString(Charsets.UTF_8) }
                    LicenseDoc(title = name.substringBeforeLast('.'), body = body)
                }
            }.getOrNull()
        }
    }

    Dialog(onDismissRequest = onDismiss) {
        Surface(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp)
                .widthIn(max = 720.dp),
            shape = MaterialTheme.shapes.large,
            color = MaterialTheme.colorScheme.surface,
            tonalElevation = 6.dp,
        ) {
            Column(Modifier.fillMaxWidth()) {
                Text(
                    text = stringResource(R.string.licenses_title),
                    style = MaterialTheme.typography.titleLarge,
                    modifier = Modifier.padding(start = 20.dp, top = 20.dp, end = 20.dp),
                )
                val list = docs
                when {
                    list == null ->
                        Text(
                            text = stringResource(R.string.progress_indeterminate),
                            modifier = Modifier.padding(20.dp),
                            style = MaterialTheme.typography.bodyMedium,
                        )

                    list.isEmpty() ->
                        Text(
                            text = stringResource(R.string.licenses_load_failed),
                            modifier = Modifier.padding(20.dp),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.error,
                        )

                    else -> LazyColumn(
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(max = 420.dp),
                        contentPadding = PaddingValues(20.dp),
                        verticalArrangement = Arrangement.spacedBy(2.dp),
                    ) {
                        val flat = rows.orEmpty()
                        items(flat.size) { i ->
                            when (val row = flat[i]) {
                                is LicenseRow.Title -> Text(
                                    text = row.text,
                                    style = MaterialTheme.typography.titleMedium,
                                    color = MaterialTheme.colorScheme.primary,
                                    modifier = Modifier.padding(top = 12.dp),
                                )

                                is LicenseRow.Line -> Text(
                                    text = row.text,
                                    style = MaterialTheme.typography.bodySmall,
                                    fontFamily = FontFamily.Monospace,
                                )
                            }
                        }
                    }
                }
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(12.dp),
                    horizontalArrangement = Arrangement.End,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Button(onClick = onDismiss) {
                        Text(stringResource(R.string.licenses_close))
                    }
                }
            }
        }
    }
}
