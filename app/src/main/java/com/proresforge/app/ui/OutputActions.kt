// OutputActions.kt — 输出文件的通用动作(打开 / 分享),任务页与历史页共用。
// U-11:完成的任务卡片原先没有打开入口,必须切到历史页才能看结果;与其把同样的
// Intent 代码抄一份,不如抽到一处,两页行为保持一致(权限标志也只需维护一次)。
package com.proresforge.app.ui

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.Toast
import com.proresforge.app.R

/** 用外部应用打开输出(ACTION_VIEW + 本次读权限);uri 无效或没有应用可打开时 Toast 提示。 */
fun openOutputUri(context: Context, uriString: String?, mime: String) {
    val uri = parseOrNull(uriString)
    if (uri == null) {
        toast(context, context.getString(R.string.toast_bad_out_uri))
        return
    }
    val intent = Intent(Intent.ACTION_VIEW).apply {
        setDataAndType(uri, mime)
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }
    runCatching { context.startActivity(intent) }
        .onFailure { toast(context, context.getString(R.string.toast_no_app_to_open)) }
}

/** 通过系统分享面板发送输出文件。 */
fun shareOutputUri(context: Context, uriString: String?, mime: String, title: String) {
    val uri = parseOrNull(uriString)
    if (uri == null) {
        toast(context, context.getString(R.string.toast_bad_out_uri))
        return
    }
    val send = Intent(Intent.ACTION_SEND).apply {
        type = mime
        putExtra(Intent.EXTRA_STREAM, uri)
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }
    runCatching { context.startActivity(Intent.createChooser(send, title)) }
        .onFailure { toast(context, context.getString(R.string.toast_share_failed)) }
}

/** uri 字符串 → Uri;空串或解析结果为空返回 null(与历史页原实现的判定一致)。 */
private fun parseOrNull(uriString: String?): Uri? {
    if (uriString.isNullOrEmpty()) return null
    val uri = runCatching { Uri.parse(uriString) }.getOrNull() ?: return null
    return uri.takeIf { it.toString().isNotEmpty() }
}

private fun toast(context: Context, message: String) {
    Toast.makeText(context, message, Toast.LENGTH_SHORT).show()
}
