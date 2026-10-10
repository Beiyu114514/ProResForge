package com.proresforge.app.engine

import android.content.ContentResolver
import android.net.Uri
import android.os.ParcelFileDescriptor
import com.proresforge.app.data.JobSpec
import com.proresforge.app.data.MediaFacts

/**
 * FFmpeg JNI 门面(契约 §2 逐字一致)+ spec 串序列化(契约 §3)。
 *
 * ## 与 native 的约定(必须与 cpp/ 侧符号严格匹配)
 * - native 方法见下方 external 声明;C 侧符号形如
 *   `Java_com_proresforge_app_engine_FFmpeg_nativeRun(JNIEnv*, jobject, ...)`
 *   (第二个参数是 jobject:Kotlin object 的方法仍是实例方法)。
 * - fd 语义:fd 全部由这里的 detachFd() 得到后交给 native,native 内部用
 *   pread/pwrite 自维护偏移、用完自己 close。**Kotlin 侧绝不能再 close 这些 fd。**
 * - LUT 不通过 fd 传递,而是 cacheDir 下的绝对路径(native 用 stdio 读)。
 *
 * ## 异常安全
 * native 符号与 Kotlin 声明不匹配、或 .so 缺失时,external 调用抛
 * UnsatisfiedLinkError(Throwable 的子类)。调用方应在 try/catch(Throwable) 里调用
 * 本对象,避免 so 未就绪时 App 崩溃。
 */
object FFmpeg {

    /** nativeRun 返回值:成功。 */
    const val RC_OK = 0

    /** nativeRun 返回值:用户取消。 */
    const val RC_CANCELLED = 1

    /** nativeRun 返回值:输出格式或编码器不支持。 */
    const val RC_UNSUPPORTED = 2

    /** nativeRun 返回值:输入文件损坏或无法解码。 */
    const val RC_INVALID_INPUT = 3

    init {
        // 库名必须与 cpp/CMakeLists.txt 的 target 名一致
        System.loadLibrary("ffmpeg-transcode")
    }

    /**
     * 转码进度回调(§2)。native 线程调用,实现方需自行切线程。
     */
    interface Progress {
        fun onProgress(pct: Int, stage: String, frame: Long, totalFrames: Long)
    }

    /**
     * 执行转码。
     *
     * @param fd      输入 fd(已 detachFd,native 用完会 close)
     * @param length  输入字节数(seeking/判断文件大小用)
     * @param outFd   输出 fd(已 detachFd,native 用完会 close)
     * @param spec    buildSpec 生成的 k=v\n 串
     * @param progress 进度回调
     * @return 0 成功;1 取消;2 输出格式/编码器不支持;3 输入文件损坏或无法解码;
     *         <0 = FFmpeg 错误码的相反数
     */
    external fun nativeRun(
        fd: Int,
        length: Long,
        outFd: Int,
        spec: String,
        progress: Progress
    ): Int

    /** 请求取消当前 nativeRun(异步,下一次进度回调后返回)。 */
    external fun nativeCancel(): Int

    /**
     * 作废尚未被 nativeRun 消费的 pending 取消(cancelPendingCancel 后本调用注销前生效)。
     *
     * 取消可能发生在 nativeRun 之前(probe/fd 早退,或 Kotlin 已判定任务 CANCELLED 而
     * 跳过 nativeRun);若不清掉,native 侧残留的 pending 会让下一次 nativeRun 开局即取消。
     * 只对「转码未在跑」的情况有效,正在进行中的取消不受影响。
     */
    external fun clearPendingCancel()

    /**
     * 探测输入信息(§4)。
     *
     * @return "k=v;k=v" 点分串;失败返回空串或 null(调用方需兼容两者)
     */
    external fun nativeInfo(fd: Int, length: Long): String

    /**
     * 生成预览 JPEG:解码 [atUs] 处一帧,可选套 LUT,最长边缩到 [maxW](§6)。
     *
     * @param dngQuality DngQuality.ordinal(0/1/2)
     * @param dngColor   DngColorMode.ordinal(0/1/2)
     * @param dngMap     DngMap.ordinal(0/1)
     *   DNG 序列 zip 的解码档位必须与转码 spec 的 dngq/dngcolor/dngmap 一致,
     *   否则预览画面和成品画质/白平衡对不上(N-7);普通容器忽略这三个参数。
     *   三者在 native 侧越界会退回默认档(FAST/CAMERA/NONE)。
     * @return JPEG 字节;失败返回 null
     */
    external fun nativePreview(
        fd: Int,
        length: Long,
        atUs: Long,
        lutPath: String?,
        lutIntensity: Double,
        graded: Boolean,
        maxW: Int,
        dngQuality: Int,
        dngColor: Int,
        dngMap: Int
    ): ByteArray?

    /**
     * 初始化 native 日志文件:把 app.log 的绝对路径交给 native,FFmpeg 的 av_log 输出
     * (编码器失败原因、滤镜报错等)以 O_APPEND 共写同一文件(AppLog 负责轮转)。
     * 失败静默 —— 只影响 native 行落盘,logcat 照常。
     */
    external fun initNativeLogging(logFilePath: String)

    /**
     * 把 [JobSpec] + 探测出的 [MediaFacts] 序列化成 §3 的 k=v\n spec 串。
     *
     * 实现已迁到 [SpecBuilder.buildSpec](2026-10-07):FFmpeg object 的 <clinit> 会
     * System.loadLibrary,纯 JVM 单测无法触碰;规则注释与测试见 SpecBuilder。
     */
    fun buildSpec(spec: JobSpec, facts: MediaFacts): String = SpecBuilder.buildSpec(spec, facts)

    /**
     * 以只读方式打开输入并 detach 出裸 fd(§2)。
     *
     * @return (fd, length);打开失败/取不到长度时第二个元素为 -1;整体失败返回 null
     */
    fun writeFd(uri: Uri, resolver: ContentResolver): Pair<Int, Long>? = try {
        val pfd: ParcelFileDescriptor =
            resolver.openFileDescriptor(uri, "r") ?: return null
        val size = try {
            pfd.statSize
        } catch (e: Exception) {
            -1L
        }
        Pair(pfd.detachFd(), size)
    } catch (e: Exception) {
        AppLog.w(TAG, "writeFd 失败: ${e.message}")
        null
    }

    /**
     * 以「读/写/截断」方式创建输出并 detach 出裸 fd(§2)。
     * 调用方需先用 OutputDir.outputUriFor 建好文档(或自行 createDocument)。
     *
     * 必须带读权限("rwt" 而非 "wt"):faststart(moov 前移)会用 pread 读回
     * 输出 fd 的顶层 box,O_WRONLY fd 上 pread 一律 EBADF —— 自 P1-03 起
     * faststart 失败即任务失败,写-only fd 会让所有 mp4/mov 任务必挂。
     */
    fun createOutFd(uri: Uri, resolver: ContentResolver): Int? = try {
        val pfd: ParcelFileDescriptor =
            resolver.openFileDescriptor(uri, "rwt") ?: return null
        pfd.detachFd()
    } catch (e: Exception) {
        AppLog.w(TAG, "createOutFd 失败: ${e.message}")
        null
    }

    // ------------------------------------------------------------------ 内部实现

    private const val TAG = "FFmpeg"
}
