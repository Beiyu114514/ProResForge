package com.proresforge.app

import android.net.Uri
import android.net.TestUris

/**
 * 纯 JVM 单测用的 Uri 占位:实现见 android.net.TestUris(同包借用 Uri 的
 * 包私有构造器;mockable android.jar 下 StringUri 反射不可得,见该类注释)。
 */
fun fakeUri(): Uri = TestUris.fake()
