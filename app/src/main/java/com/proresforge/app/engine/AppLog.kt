// AppLog.kt — 全 App 统一日志门面:logcat 镜像 + 文件落盘(filesDir/logs/)。
//
// - 文件:logs/app.log(当前)+ logs/app.log.1(上一份);单文件超 2MB 轮转,
//   总占用上限约 4MB;
// - 写盘在专用 HandlerThread 上串行执行,调用方零阻塞;
// - native(FFmpeg av_log)通过 nativeInitLogging 拿到同一个 app.log 路径,以 O_APPEND
//   共写;两侧行都是单次 write,基本不会交错;
// - 分享:logShareUris() 给历史页「日志」按钮用(FileProvider,见 AndroidManifest)。
//
// 用法:Application.onCreate 里 AppLog.init(this);之后任意处 AppLog.i(TAG, "...")。
package com.proresforge.app.engine

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import androidx.core.content.FileProvider
import java.io.File
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

object AppLog {

    /** 单文件上限;超过即 app.log → app.log.1 轮转。 */
    private const val MAX_BYTES = 2L * 1024 * 1024

    private const val TAG = "AppLog"

    @Volatile
    private var logFile: File? = null

    @Volatile
    private var handler: Handler? = null

    private val ts = SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US)

    /** 初始化(幂等)。同时把日志路径交给 native,让 FFmpeg 的 av_log 共写同一文件。 */
    fun init(context: Context) {
        if (handler != null) return
        val dir = File(context.applicationContext.filesDir, "logs").apply { mkdirs() }
        val f = File(dir, "app.log")
        logFile = f
        handler = Handler(HandlerThread("app-log").apply { start() }.looper)
        i(TAG, "init: ${f.absolutePath}")
        // native 侧与文件日志共用一路;库未就绪时只影响 ffmpeg 行的落盘,logcat 照常
        runCatching { FFmpeg.initNativeLogging(f.absolutePath) }
            .onFailure { w(TAG, "initNativeLogging 失败(native 行不落盘)", it) }
    }

    // ------------------------------------------------------------------ 对外接口

    fun v(tag: String, msg: String, tr: Throwable? = null) = dispatch(Log.VERBOSE, tag, msg, tr)

    fun i(tag: String, msg: String, tr: Throwable? = null) = dispatch(Log.INFO, tag, msg, tr)

    fun w(tag: String, msg: String, tr: Throwable? = null) = dispatch(Log.WARN, tag, msg, tr)

    fun e(tag: String, msg: String, tr: Throwable? = null) = dispatch(Log.ERROR, tag, msg, tr)

    // ------------------------------------------------------------------ 内部实现

    private fun dispatch(level: Int, tag: String, msg: String, tr: Throwable?) {
        // logcat 即时镜像(不做任何 IO)
        if (tr != null) {
            when (level) {
                Log.VERBOSE -> Log.v(tag, msg, tr)
                Log.INFO -> Log.i(tag, msg, tr)
                Log.WARN -> Log.w(tag, msg, tr)
                Log.ERROR -> Log.e(tag, msg, tr)
                else -> Log.d(tag, msg, tr)
            }
        } else {
            Log.println(level, tag, msg)
        }

        val h = handler ?: return
        val now = System.currentTimeMillis()
        // 格式化挪进 handler 线程执行:ts(SimpleDateFormat)非线程安全,在调用方
        // 线程(主线程/IO 协程并发)跑 format() 可能抛异常直接打崩调用方;handler
        // 线程天然串行,顺带免去调用方的格式化开销。
        h.post { appendFile(format(level, tag, msg, tr, now)) }
    }

    private fun format(level: Int, tag: String, msg: String, tr: Throwable?, now: Long): String {
        val head = "${ts.format(Date(now))} ${levelChar(level)}/$tag: $msg"
        return if (tr != null) "$head\n${Log.getStackTraceString(tr)}" else head
    }

    private fun levelChar(level: Int): Char = when (level) {
        Log.VERBOSE -> 'V'
        Log.INFO -> 'I'
        Log.WARN -> 'W'
        Log.ERROR -> 'E'
        else -> 'D'
    }

    private fun appendFile(line: String) {
        val f = logFile ?: return
        try {
            if (f.isFile && f.length() > MAX_BYTES) rotate(f)
            FileOutputStream(f, true).use { it.write((line + "\n").toByteArray(Charsets.UTF_8)) }
        } catch (e: Exception) {
            Log.w(TAG, "写日志文件失败: ${e.message}")
        }
    }

    /** app.log → app.log.1(覆盖上一份);native 侧的行可能随改名落进旧文件,可接受。 */
    private fun rotate(f: File) {
        val old = File(f.parentFile, "${f.nameWithoutExtension}.1")
        if (old.isFile) old.delete()
        if (!f.renameTo(old)) Log.w(TAG, "日志轮转失败")
    }

    // ------------------------------------------------------------------ 分享

    /** 可分享的日志文件(FileProvider uri);没有内容时返回空列表。 */
    fun logShareUris(context: Context): List<Uri> {
        val f = logFile ?: return emptyList()
        val files = listOfNotNull(
            f.takeIf { it.isFile && it.length() > 0L },
            File(f.parentFile, "app.log.1").takeIf { it.isFile && it.length() > 0L },
        )
        if (files.isEmpty()) return emptyList()
        val authority = "${context.packageName}.fileprovider"
        return files.map { FileProvider.getUriForFile(context, authority, it) }
    }
}
