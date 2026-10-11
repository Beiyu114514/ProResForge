# ProResForge ProGuard / R8 规则。
# release 当前未开启混淆(isMinifyEnabled = false,见 app/build.gradle.kts),
# 此文件仅在手工开启 R8 后生效。

# 我们的 JNI 入口类必须原样保留(契约 §2):
# C 侧符号 Java_com_proresforge_app_engine_FFmpeg_nativeRun / _nativeCancel /
# _nativeInfo / _nativePreview / initNativeLogging / clearPendingCancel(共 6 个)
# 依赖该类的完整类名与方法名,一旦被混淆或内联,调用即 UnsatisfiedLinkError。
-keep class com.proresforge.app.engine.FFmpeg { *; }

# ============================================================================
# JNI 相关(P1-01,2026-10-11)
# ============================================================================

# 所有 native 方法:连描述符一起保留(R8 内联/改名都会让 RegisterNatives 或
# 静态解析对不上)。FFmpeg 类上面已整类 keep,这里兜底覆盖未来新增的 JNI 类。
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}

# FFmpeg.Progress 回调接口(native 转码线程会反向调用 onProgress):
# 整类 keep 只保住了 FFmpeg 自己,这个内部接口若无规则,实现类方法名仍可能被
# 混淆 → native 侧按名字查找回调方法失败,进度静默丢失。
-keep interface com.proresforge.app.engine.FFmpeg$Progress { *; }

# ============================================================================
# kotlinx.serialization(P1-01,2026-10-11)
# ============================================================================

# @Serializable 类的字段必须保留:序列化器生成的 $serializer 按 field name
# 反射/合成访问,混淆改字段名会导致旧 jobs.json 反序列化失败(历史全部丢失)。
-keepclassmembers @kotlinx.serialization.Serializable class ** {
    <fields>;
}

# 序列化器(Companion.serializer() 与 XXX$$serializer 的名字/描述符不能动)
-keep,includedescriptorclasses class com.proresforge.app.data.**$$serializer { *; }
-keepclassmembers class com.proresforge.app.data.** {
    *** Companion;
}

# 具体的数据类(防御性保留;当前仅 HistoryItem 带 @Serializable,其余三类
# 暂未参与序列化,一并锁定以便后续改动时无需回想起本文件):
-keep class com.proresforge.app.data.HistoryItem { *; }
-keep class com.proresforge.app.data.JobSpec { *; }
-keep class com.proresforge.app.data.JobRecord { *; }
-keep class com.proresforge.app.data.MediaFacts { *; }

# ============================================================================
# 反射相关(P1-01,2026-10-11)
# ============================================================================

# DocumentFile 相关(SAF:androidx.documentfile 内部经 ContentProvider 反射查询)
-keep class androidx.documentfile.provider.** { *; }

# ParcelFileDescriptor(fd 操作:TranscodeService.closeDetachedFd 经 adoptFd 包装)
-keep class android.os.ParcelFileDescriptor { *; }

# ============================================================================
# Compose & ViewModel(P1-01,2026-10-11)
# ============================================================================

# ViewModel 保留(lifecycle 库自带规则,这里显式声明更安全:反射创建依赖构造器)
-keep class * extends androidx.lifecycle.ViewModel {
    <init>(...);
}

# Compose:compose-compiler 已自带规则,无需重复 keep。

# ============================================================================
# 枚举(P1-01,2026-10-11)
# ============================================================================

# values()/valueOf() 依赖 $VALUES 数组字段;JobStore 按 name 存枚举
# (OutFormat/JobState),枚举常量名混淆即旧档反序列化失败。
-keepclassmembers enum com.proresforge.app.data.** {
    **[] $VALUES;
    public *;
}
