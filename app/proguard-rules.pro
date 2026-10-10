# ProResForge ProGuard / R8 规则。
# release 当前未开启混淆(isMinifyEnabled = false,见 app/build.gradle.kts),
# 此文件仅在手工开启 R8 后生效。

# 我们的 JNI 入口类必须原样保留(契约 §2):
# C 侧符号 Java_com_proresforge_app_engine_FFmpeg_nativeRun / _nativeCancel /
# _nativeInfo / _nativePreview / initNativeLogging / clearPendingCancel(共 6 个)
# 依赖该类的完整类名与方法名,一旦被混淆或内联,调用即 UnsatisfiedLinkError。
-keep class com.proresforge.app.engine.FFmpeg { *; }
