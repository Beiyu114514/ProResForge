// ProResForgeApp.kt — Application:尽早初始化日志(其余组件零依赖可用)。
package com.proresforge.app

import android.app.Application
import com.proresforge.app.engine.AppLog

class ProResForgeApp : Application() {

    override fun onCreate() {
        super.onCreate()
        AppLog.init(this)
        val ver = runCatching {
            val info = packageManager.getPackageInfo(packageName, 0)
            "v${info.versionName} (${info.longVersionCode})"
        }.getOrDefault("?")
        AppLog.i(TAG, "app start $ver")
    }

    private companion object {
        const val TAG = "App"
    }
}
