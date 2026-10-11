package com.proresforge.app.engine

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import com.proresforge.app.R

/**
 * 转码通知(契约 §5 / §8.3):进度(前台)、完成、失败三种。
 *
 * - 渠道 id 固定 [CHANNEL_ID]("transcode")与 [DONE_CHANNEL_ID]("transcode_done"),
 *   名称/描述取 R.string.notif_channel_*;
 * - [progress] 产出的通知由 TranscodeService 用作 startForeground 的前台通知,
 *   带一个「取消」Action(由调用方通过 [PendingIntent] 传入);
 * - [done] 点击回到主界面,「打开文件」为 Action(有输出文件时);
 * - API 33+ 未授予 POST_NOTIFICATIONS 时 notify 静默失败(不抛异常)。
 */
object Notifications {

    /** 通知渠道 id(与 Manifest 无关联,仅本地约定)。 */
    const val CHANNEL_ID = "transcode"

    /** U-12:完成/失败通知单独一个渠道(进度渠道被静音,不能共用)。 */
    const val DONE_CHANNEL_ID = "transcode_done"

    private const val TAG = "Notifications"

    /** 建渠道(幂等);API < 26 无需渠道,直接返回。 */
    fun ensureChannel(context: Context) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val nm = context.getSystemService(NotificationManager::class.java) ?: return
        createChannel(
            context = context,
            nm = nm,
            id = CHANNEL_ID,
            nameRes = R.string.notif_channel_name,
            descRes = R.string.notif_channel_desc,
            importance = NotificationManager.IMPORTANCE_LOW,
            showBadge = false,
            vibrate = false,
        )
        // U-12:进度通知是 LOW 且不响不震;完成/失败共用同一渠道会被一并静音,
        // 所以「转码结果」单独一个渠道(用户也能在系统设置里单独调)。
        createChannel(
            context = context,
            nm = nm,
            id = DONE_CHANNEL_ID,
            nameRes = R.string.notif_channel_done_name,
            descRes = R.string.notif_channel_desc,
            importance = NotificationManager.IMPORTANCE_DEFAULT,
            showBadge = true,
            vibrate = true,
        )
    }

    /** 建单个渠道(已存在则跳过;ROM 拒绝建渠道时忽略——通知只是不显示)。 */
    private fun createChannel(
        context: Context,
        nm: NotificationManager,
        id: String,
        nameRes: Int,
        descRes: Int,
        importance: Int,
        showBadge: Boolean,
        vibrate: Boolean,
    ) {
        if (nm.getNotificationChannel(id) != null) return
        val channel = NotificationChannel(id, context.getString(nameRes), importance).apply {
            description = context.getString(descRes)
            setShowBadge(showBadge)
            enableVibration(vibrate)
            lockscreenVisibility = Notification.VISIBILITY_PUBLIC
        }
        try {
            nm.createNotificationChannel(channel)
        } catch (e: Exception) {
            // 某些定制 ROM 会拒绝建渠道;忽略即可(通知只是不显示)
        }
    }

    /**
     * 进度通知(前台通知,持续显示)。
     *
     * @param title 通知标题
     * @param text  通知正文(调用方已本地化,如 getString(R.string.notif_running, pct))
     * @param pct   0..100,用于进度条
     * @param cancelIntent 「取消」按钮的 PendingIntent(一般为 Service 的取消动作)
     * @return 已下发的通知本体(Service 可直接拿它 startForeground)
     */
    fun progress(
        context: Context,
        id: Int,
        title: String,
        text: String,
        pct: Int,
        cancelIntent: PendingIntent
    ): Notification {
        val n = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_transcode)
            .setContentTitle(title)
            .setContentText(text)
            .setProgress(100, pct.coerceIn(0, 100), false)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setCategory(NotificationCompat.CATEGORY_PROGRESS)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
            .addAction(0, context.getString(R.string.btn_cancel), cancelIntent)
            .build()
        notify(context, id, n)
        return n
    }

    /**
     * 完成通知;点击尝试打开输出文件([outputUri] 为 null 时不可点击)。
     *
     * @param title 任务名(用于填充 R.string.notif_done 的 %1$s)
     */
    fun done(context: Context, id: Int, title: String, outputUri: Uri?) {
        val builder = NotificationCompat.Builder(context, DONE_CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_transcode)
            .setContentTitle(context.getString(R.string.app_name))
            .setContentText(context.getString(R.string.notif_done, title))
            .setAutoCancel(true)
            .setOnlyAlertOnce(true)
            .setCategory(NotificationCompat.CATEGORY_STATUS)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
        val viewPi = if (outputUri != null) {
            val mime = try {
                context.contentResolver.getType(outputUri)
            } catch (e: Exception) {
                null
            } ?: "video/*"
            val viewIntent = Intent(Intent.ACTION_VIEW).apply {
                setDataAndType(outputUri, mime)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            PendingIntent.getActivity(
                context,
                id,
                viewIntent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )
        } else {
            null
        }
        // U-12:点击通知默认回到主界面;有输出文件时把「打开文件」保留为 Action
        // (原先点击直接 ACTION_VIEW,通知一划走、任务列表也不好进)。
        appPendingIntent(context, id)?.let { builder.setContentIntent(it) }
        if (viewPi != null) builder.addAction(0, context.getString(R.string.open), viewPi)
        notify(context, id, builder.build())
    }

    /** U-12:点通知回到主界面的 PendingIntent(取不到启动 Intent 时返回 null)。 */
    private fun appPendingIntent(context: Context, id: Int): PendingIntent? {
        val launch = context.packageManager.getLaunchIntentForPackage(context.packageName) ?: return null
        launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        return PendingIntent.getActivity(
            context,
            id + 1000,
            launch,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
    }

    /**
     * 失败通知。
     *
     * @param title 任务名(用于填充 R.string.notif_failed 的 %1$s)
     * @param error 失败原因(展示为副标题)
     */
    fun failed(context: Context, id: Int, title: String, error: String) {
        val builder = NotificationCompat.Builder(context, DONE_CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_transcode)
            .setContentTitle(context.getString(R.string.app_name))
            .setContentText(context.getString(R.string.notif_failed, title))
            .setAutoCancel(true)
            .setCategory(NotificationCompat.CATEGORY_ERROR)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
        if (error.isNotEmpty()) builder.setSubText(error)
        notify(context, id, builder.build())
    }

    /**
     * 统一下发。通知失败(API 33+ 未授予 POST_NOTIFICATIONS、ROM 限制…)一律不抛异常,
     * 但记一行日志:否则「通知怎么没出来」这类问题在 logcat / app.log 里毫无线索。
     */
    private fun notify(context: Context, id: Int, n: Notification) {
        try {
            NotificationManagerCompat.from(context).notify(id, n)
        } catch (e: SecurityException) {
            // API 33+ 未授予 POST_NOTIFICATIONS(用户拒绝了通知权限)
            AppLog.w(TAG, "notify($id) 缺通知权限: ${e.message}")
        } catch (e: Exception) {
            AppLog.w(TAG, "notify($id) 失败: ${e.javaClass.simpleName}: ${e.message}")
        }
    }
}
