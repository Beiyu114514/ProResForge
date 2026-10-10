// native-lib.cpp —— JNI 入口(6 个函数)
//
// 对应契约 INTERFACES.md §2 的 C 侧符号(逐字一致,改一处两边同步):
//   Java_com_proresforge_app_engine_FFmpeg_nativeRun(JNIEnv*, jobject, jint fd, jlong length, jint outFd, jstring spec, jobject progress)
//   Java_com_proresforge_app_engine_FFmpeg_nativeCancel(JNIEnv*, jobject)
//   Java_com_proresforge_app_engine_FFmpeg_nativeInfo(JNIEnv*, jobject, jint fd, jlong length)
//   Java_com_proresforge_app_engine_FFmpeg_nativePreview(JNIEnv*, jobject, jint fd, jlong length, jlong atUs, jstring lutPath, jdouble lutIntensity, jboolean graded, jint maxW, jint dngQuality, jint dngColor, jint dngMap)
//   Java_com_proresforge_app_engine_FFmpeg_initNativeLogging(JNIEnv*, jobject, jstring logFilePath)
//   Java_com_proresforge_app_engine_FFmpeg_clearPendingCancel(JNIEnv*, jobject)
//
// 线程/引用规则:
//   * nativeRun 由 Kotlin 串行调用(TranscodeService 单线程消费队列),本函数是阻塞的,
//     转码/进度都发生在调用线程上,不另起线程(避免 JNI 线程管理复杂度)。
//   * 进度回调在同一线程上执行:AttachCurrentThread(PushLocalFrame 防止局部引用堆积),
//     结束不 DetachCurrentThread —— 该线程由 Java VM 管理,不能主动 detach。
//   * progress 对象在转码期间保存全局引用,结束后 DeleteGlobalRef。
//     method ID 依赖类不卸载:同时给类也加全局引用。
//   * fd 均为 Kotlin detachFd() 移交所有权,native 用完 close(见 transcode.cpp / preview.cpp)。

#include <jni.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

#include <android/log.h>
#include <errno.h>    // AVERROR(EBUSY)
#include <stdarg.h>
#include <sys/stat.h> // stat/fstat:核对日志句柄 inode(L-13 句柄缓存)
#include <ctime>
#include <unistd.h>   // close(fd)

#include "preview.h"
#include "strspec.h"
#include "transcode.h"
#include "dngzip.h"

extern "C" {
#include <libavcodec/avcodec.h>
// jni.h 没有 extern "C" 保护(设计上给 C 用户用),C++ 里必须包在 extern "C" 中,
// 否则 av_jni_set_java_vm 会被 C++ 名字改编,链接时找不到符号。
#include <libavcodec/jni.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
}

// 本文件在全局命名空间,契约类型都在 proresforge 里,统一 using 进来
using proresforge::FdIoCtx;
using proresforge::Spec;
using proresforge::TranscodeParams;
using proresforge::detect_rotation_deg;
using proresforge::request_cancel;
using proresforge::clear_cancel_pending;
using proresforge::run_transcode;
using proresforge::render_preview_jpeg;
using proresforge::DngZip;
using proresforge::dng_decode_to_rgb48;

namespace {

// ---------------------------------------------------------------------------
// FFmpeg 日志 → logcat + 文件(app.log,与 Kotlin 侧 AppLog 共写)
// ---------------------------------------------------------------------------
std::string g_log_path;   // AppLog.init 经 initNativeLogging 传入;空 = 不落盘
std::mutex g_log_mu;      // 与 JNI 传入路径的写入互斥

// L-13:日志句柄缓存。pf_av_log 原来每行 fopen+fseek+ftell+fclose —— 转码期
// av_log 是逐帧多条(还有 per-frame 的 fps/性能计数器),这波 syscall 风暴会把
// 解码线程拖慢。现在常驻一个 "a" 句柄(行缓冲,每条写完即刷),只在
// 「路径被换 / Kotlin AppLog 轮转改名 / 自己写爆 / 写失败」时重开;
// kLogCheckEvery 行才核对一次 inode 和大小,不是每行。
FILE *g_log_fp = nullptr;
long  g_log_budget = 0;          // 距离下次核对还剩多少行
const long kLogCheckEvery = 256; // 核对节流:256 行 ≈ 转码期每秒一次量级

// 重开到当前 g_log_path(持 g_log_mu 调用)。路径暂时不可写就丢下这条日志,
// 下一行会再试 —— 不为了写日志去阻塞转码线程。
void pf_log_reopen()
{
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = nullptr;
    }
    if (g_log_path.empty()) return;
    g_log_fp = fopen(g_log_path.c_str(), "a");
    if (!g_log_fp) return;
    setvbuf(g_log_fp, nullptr, _IOLBF, 0);   // 每条日志写完即刷,又不必每行 open/close
    g_log_budget = kLogCheckEvery;
}

void pf_av_log(void *avcl, int level, const char *fmt, va_list vl)
{
    if (level > av_log_get_level()) return;
    char line[1024];
    int prefix = 1;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &prefix);
    int prio;
    if (level <= AV_LOG_FATAL) prio = ANDROID_LOG_FATAL;
    else if (level <= AV_LOG_ERROR) prio = ANDROID_LOG_ERROR;
    else if (level <= AV_LOG_WARNING) prio = ANDROID_LOG_WARN;
    else if (level <= AV_LOG_INFO) prio = ANDROID_LOG_INFO;
    else prio = ANDROID_LOG_DEBUG;
    __android_log_print(prio, "ProResForge", "%s", line);

    // 落盘:与 Kotlin AppLog 共写同一个 filesDir/logs/app.log(见 L-13)。
    std::lock_guard<std::mutex> lk(g_log_mu);
    if (g_log_path.empty()) return;
    if (!g_log_fp) {
        g_log_fp = fopen(g_log_path.c_str(), "a");
        if (!g_log_fp) return;
        setvbuf(g_log_fp, nullptr, _IOLBF, 0);
        g_log_budget = kLogCheckEvery;
    }
    // 周期性核对(不是每行):
    //   ① 句柄 inode 是否还等于当前路径 —— Kotlin AppLog 轮转会 rename app.log,
    //      常驻句柄会继续写旧 inode,日志就裂成两个文件;
    //   ② 文件是否 > 4MB —— Kotlin 侧 2MB 轮转之外的兜底,单侧写爆时改名续写。
    if (--g_log_budget <= 0) {
        g_log_budget = kLogCheckEvery;
        struct stat pst, fst;
        bool have_p = (::stat(g_log_path.c_str(), &pst) == 0);
        bool have_f = (fstat(fileno(g_log_fp), &fst) == 0);
        if (have_p && have_f && pst.st_dev == fst.st_dev && pst.st_ino == fst.st_ino) {
            if (fst.st_size > (off_t)4 * 1024 * 1024) {
                fclose(g_log_fp);
                std::rename(g_log_path.c_str(), (g_log_path + ".1").c_str());
                g_log_fp = fopen(g_log_path.c_str(), "a");
                if (!g_log_fp) return;
                setvbuf(g_log_fp, nullptr, _IOLBF, 0);
            }
        } else {
            pf_log_reopen();   // 路径被轮转/替换/删掉:跟到当前路径再写
            if (!g_log_fp) return;
        }
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    char tsbuf[32];
    strftime(tsbuf, sizeof(tsbuf), "%m-%d %H:%M:%S", &tmv);
    const char lvl = (level <= AV_LOG_ERROR) ? 'E'
                   : ((level <= AV_LOG_WARNING) ? 'W'
                   : ((level <= AV_LOG_INFO) ? 'I' : 'D'));
    // av_log_format_line2 的结果带换行,去掉再统一加
    size_t n = std::strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    if (fprintf(g_log_fp, "%s.%03ld %c/FFmpeg: %s\n", tsbuf, ts.tv_nsec / 1000000L, lvl, line) < 0) {
        pf_log_reopen();   // 写失败(被外部截断/空间不足):丢掉句柄,下次重开再试
        return;
    }
}

// ---------------------------------------------------------------------------
// 进度回调桥
// ---------------------------------------------------------------------------
JavaVM *g_vm = nullptr;          // JNI_OnLoad 里缓存
jobject g_progress = nullptr;    // Progress 对象的全局引用
jclass g_progress_cls = nullptr; // 其类的全局引用(保 method ID 有效)
jmethodID g_progress_method = 0; // void onProgress(int, String, long, long)
std::mutex g_mu;
std::atomic<int> g_cb_inflight{0};        // 正在执行中的回调次数(锁外读写)
std::atomic<bool> g_cb_drop_pending{false}; // 有回调在飞时来不及删,交给最后一个回调

// 契约 §5 STAGES
const char *STAGE_INIT = "init";
const char *STAGE_DECODE = "decode";
const char *STAGE_AUDIO = "audio";
const char *STAGE_FINALIZE = "finalize";
const char *STAGE_DONE = "done";
const char *STAGE_ERROR = "error";

// ProgressFn 实现:在转码线程上调用,主动 attach + 局部帧管理。
//
// 关键约束:持 g_mu 期间绝不回调进 VM。旧实现整段回调都在锁内,一旦 Kotlin 侧
// onProgress 阻塞或再入 native(例如再调 nativeRun 想拿同一把锁),立刻死锁。
// 现在只在锁内拷贝「引用 + method id」,出锁后再 CallVoidMethod。
void cb_done(); // 定义在下面:结束一次回调,必要时执行被推迟的删除

void jni_progress(int pct, const char *stage, int64_t frame, int64_t total_frames, void *user)
{
    (void)user;
    // 先占位,再拷引用。「拷引用」与「占位」之间若被另一线程插进来删引用
    // (它看到 in-flight==0),我们手上这个 jobject 就悬空了。顺序反过来的话,
    // 删除方一定会看到 in-flight>0,只会置 pending。
    g_cb_inflight.fetch_add(1);
    jobject cb = nullptr;
    jmethodID mid = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        cb = g_progress;
        mid = g_progress_method;
    }
    if (!cb || !mid) { cb_done(); return; }
    JNIEnv *env = nullptr;
    if (!g_vm || g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) { cb_done(); return; }

    env->PushLocalFrame(1);
    jstring js = env->NewStringUTF(stage ? stage : "");
    if (js) {
        env->CallVoidMethod(cb, mid, (jint)pct, js, (jlong)frame, (jlong)total_frames);
        if (env->ExceptionCheck()) env->ExceptionClear();   // 不让 Kotlin 异常污染后续 JNI
    }
    env->PopLocalFrame(nullptr);

    cb_done();
}

// 结束一次回调(或一次未成功发起的尝试)。最后一个走的人要负责执行被推迟的删除:
// 没有 JNIEnv 时删不了全局引用,就留着 pending 等下一次回调( pending 会一直置位,
// 不会漏删也不会重复删)。
void cb_done()
{
    const int left = g_cb_inflight.fetch_sub(1) - 1;
    if (left == 0 && g_cb_drop_pending.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_cb_drop_pending.load()) return;
        JNIEnv *env = nullptr;
        if (g_vm && g_vm->GetEnv((void **)&env, JNI_VERSION_1_6) == JNI_OK) {
            if (g_progress) { env->DeleteGlobalRef(g_progress); g_progress = nullptr; }
            if (g_progress_cls) { env->DeleteGlobalRef(g_progress_cls); g_progress_cls = nullptr; }
            g_progress_method = 0;
            g_cb_drop_pending.store(false, std::memory_order_release);
        }
    }
}

// 释放进度回调用的全局引用(调用方持有 g_mu)。
// 有回调正在飞时不能删引用(回调还在用它),置 pending 让最后一个回调自己删。
void drop_progress_locked(JNIEnv *env)
{
    if (g_cb_inflight.load(std::memory_order_acquire) > 0) {
        g_cb_drop_pending.store(true, std::memory_order_release);
        return;
    }
    if (g_progress) { env->DeleteGlobalRef(g_progress); g_progress = nullptr; }
    if (g_progress_cls) { env->DeleteGlobalRef(g_progress_cls); g_progress_cls = nullptr; }
    g_progress_method = 0;
    g_cb_drop_pending.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// nativeInfo 数据收集(契约 §4)
// ---------------------------------------------------------------------------
std::string fourcc_str(uint32_t tag)
{
    // 容器 fourcc 按小端字节序解读(MOV/AVI 惯例:低字节在前);不可打印则返回空串
    char b[4];
    b[0] = (char)(tag & 0xFF);
    b[1] = (char)((tag >> 8) & 0xFF);
    b[2] = (char)((tag >> 16) & 0xFF);
    b[3] = (char)((tag >> 24) & 0xFF);
    for (int i = 0; i < 4; i++) {
        const unsigned char c = (unsigned char)b[i];
        if (c < 32 || c > 126) return std::string();
    }
    return std::string(b, 4);
}

// "k1=v1;k2=v2" 点号分隔;失败返回空串
std::string join_kv(const proresforge::Spec &kv)
{
    std::string s;
    const std::vector<std::pair<std::string, std::string>> &items = kv.items();
    for (size_t i = 0; i < items.size(); i++) {
        if (!s.empty()) s += ';';
        s += items[i].first;
        s += '=';
        s += items[i].second;
    }
    return s;
}

std::string collect_media_info(int fd, int64_t length)
{
    AVFormatContext *ic = nullptr;
    FdIoCtx *ioctx = nullptr;
    AVIOContext *pb = nullptr;
    uint8_t *buf = nullptr;

    // ---------- DNG 序列 zip(MotionCam,契约 §4.1) ----------
    // 先探 4 字节魔数 PK\x03\x04:命中就走 dngzip,不进 avformat。
    // 不用 tiff.c 解析 DNG:它只交 CFA Bayer(没有 demosaic / 白平衡 / 色彩矩阵),
    // 而且 DNG 的 tiled 路径还是 AVERROR_PATCHWELCOME(契约 §10.1)。
    {
        uint8_t magic[4] = { 0, 0, 0, 0 };
        // offset 0 不会截断,仍用 *64 变体保持与其余 IO 一致(见 dngzip.cpp 顶部)
        if (pread64(fd, magic, 4, 0) == 4 && DngZip::is_zip(magic, 4)) {
            std::string err;
            DngZip zip;
            if (zip.open(fd, length, err) != 0) {
                // zip 结构本身坏了(不是 zip / EOCD 没了 / CD 截断)。
                // 打日志:2026-10-04 真机上「无法解析」就是靠这些行定位的。
                av_log(nullptr, AV_LOG_WARNING,
                       "nativeInfo: 输入是 zip 但 open 失败:%s(len=%lld)\n", err.c_str(),
                       (long long)length);
                close(fd);
                return std::string();
            }
            const size_t nframes = zip.frame_count();
            if (nframes == 0) {
                av_log(nullptr, AV_LOG_WARNING,
                       "nativeInfo: zip 里没有 .dng 帧(共 0 帧),按不可解析处理\n");
                close(fd);
                return std::string();
            }
            // 拿宽高:解「第一帧能解开的帧」。首帧可能因为截断/半帧/个别坏帧
            // 解不动,不能因此把整包判死(真机反馈:整包只有首帧有问题时
            // App 直接报「无法解析」,但后面 800 帧都是好的)。
            std::vector<uint8_t> fbuf;
            std::vector<uint16_t> rgb;
            int w = 0, h = 0;
            int raw_w = 0, raw_h = 0;
            bool ok = false;
            const size_t probe_max = nframes < 8 ? nframes : 8;   // 试前 8 帧
            for (size_t i = 0; i < probe_max; i++) {
                std::string derr;
                if (zip.read_frame(i, fbuf, derr) != 0) {
                    av_log(nullptr, AV_LOG_WARNING, "nativeInfo: 探测第 %zu/%zu 帧:%s\n",
                           i + 1, nframes, derr.c_str());
                    continue;
                }
                if (dng_decode_to_rgb48(fbuf.data(), fbuf.size(), proresforge::DngQuality::FAST,
                                         proresforge::DngColorMode::CAMERA, rgb, w, h,
                                         derr, &raw_w, &raw_h, proresforge::DngMap::NONE,
                                         false) != 0) {
                    av_log(nullptr, AV_LOG_WARNING, "nativeInfo: 探测第 %zu/%zu 帧解码:%s\n",
                           i + 1, nframes, derr.c_str());
                    continue;
                }
                ok = true;   // 找到首帧能解的
                break;
            }
            if (!ok) {
                av_log(nullptr, AV_LOG_ERROR,
                       "nativeInfo: zip 前 %zu 帧全部解不开(DNG 内容损坏或不支持),按不可解析处理\n",
                       probe_max);
                close(fd);
                return std::string();
            }
            Spec kv;
            kv.set("container", "zip");
            kv.set_i("duration_us", 0);            // zip 没有容器时长
            kv.set_i("bitrate", 0);
            kv.set("codec", "dng");
            kv.set("codec_tag", "");
            kv.set("profile", "");
            // FAST 解码会输出 half_size RGB,但媒体尺寸必须使用解包前全尺寸。
            kv.set_i("w", raw_w > 0 ? raw_w : w);
            kv.set_i("h", raw_h > 0 ? raw_h : h);
            kv.set("pixfmt", "rgb48le");           // LibRaw output_bps=16 → sRGB
            kv.set_i("fpsnum", 0);                 // 帧率由用户选(zip 无 fps)
            kv.set_i("fpsden", 0);
            // nframes 上面探测段已声明(同一作用域),这里直接用
            kv.set_i("nb_frames", (long long)nframes);
            kv.set_i("frames", (long long)nframes);   // 契约 §4:zip 的帧数在 frames
            kv.set_i("rotation", 0);               // DNG 没有容器旋转
            kv.set("audio_codec", "");
            kv.set_i("audio_ch", 0);
            kv.set_i("audio_rate", 0);
            if (zip.has_audio()) {
                std::vector<uint8_t> audio_buf;
                std::string audio_err;
                AVFormatContext *audio_ic = nullptr;
                proresforge::MemIoCtx *audio_ctx = nullptr;
                AVIOContext *audio_pb = nullptr;
                uint8_t *audio_io_buf = nullptr;
                if (zip.read_audio(audio_buf, audio_err) != 0 ||
                    proresforge::mem_io_open_input(audio_buf.data(),
                                                   (int64_t)audio_buf.size(),
                                                   &audio_ic, &audio_ctx,
                                                   &audio_pb, &audio_io_buf) < 0 ||
                    avformat_find_stream_info(audio_ic, nullptr) < 0) {
                    av_log(nullptr, AV_LOG_WARNING,
                           "nativeInfo: ZIP audio.wav 无法解析: %s\n", audio_err.c_str());
                    proresforge::mem_io_free_input(&audio_ic, &audio_ctx,
                                                   &audio_pb, &audio_io_buf);
                } else {
                    const int ai = av_find_best_stream(audio_ic, AVMEDIA_TYPE_AUDIO,
                                                       -1, -1, nullptr, 0);
                    if (ai >= 0) {
                        const AVCodecParameters *acp = audio_ic->streams[ai]->codecpar;
                        const char *an = avcodec_get_name(acp->codec_id);
                        kv.set("audio_codec", an ? an : "");
                        kv.set_i("audio_ch", acp->ch_layout.nb_channels);
                        kv.set_i("audio_rate", acp->sample_rate);
                    }
                    proresforge::mem_io_free_input(&audio_ic, &audio_ctx,
                                                   &audio_pb, &audio_io_buf);
                }
            }
            // 输出按 bt709/sRGB 打标(见 §10.3 与转码的强制 cprim/ctrc 一致)
            kv.set("cspace", "bt709");
            kv.set("cprim", "bt709");
            kv.set("ctrc", "bt709");
            // nativeInfo 也拥有输入 fd(契约 §2)
            close(fd);
            return join_kv(kv);
        }
    }

    if (fd_io_open_input(fd, length, &ic, &ioctx, &pb, &buf) < 0) {
        close(fd);
        return std::string();
    }

    // 探测上限(只取头部参数;mov 的编解码/尺寸/旋转/色彩全在 stsd/colr/display matrix):
    // 默认 5MB/5s 会让 find_stream_info 对 MotionCam 4K 4444XQ(单帧 10+MB)反复试解码,
    // 手机上一次探测就是数秒,多文件排队时 UI 像卡死。收紧后拿到首帧即停,毫秒级返回。
    ic->probesize = 2 * 1024 * 1024;
    ic->max_analyze_duration = 1 * AV_TIME_BASE;

    Spec kv;
    const AVRational utb = { 1, AV_TIME_BASE };   // 微秒时基

    // ---------- 容器级 ----------
    int64_t duration_us = (ic->duration != AV_NOPTS_VALUE) ? ic->duration : -1;
    {
        // iformat->name 形如 "mov,mp4,m4a,3gp,3g2,mj2",取第一段作为简短容器名
        std::string cont;
        if (ic->iformat && ic->iformat->name) {
            cont = ic->iformat->name;
            const size_t comma = cont.find(',');
            if (comma != std::string::npos) cont = cont.substr(0, comma);
        }
        kv.set("container", cont);
        kv.set_i("duration_us", duration_us);
        kv.set_i("bitrate", ic->bit_rate > 0 ? (long long)ic->bit_rate : 0);
    }

    // ---------- 视频 ----------
    int64_t nb_frames = -1;
    {
        const int vi = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vi < 0) {
            kv.set("codec", "");
            kv.set("codec_tag", "");
            kv.set("profile", "");
            kv.set_i("w", 0);
            kv.set_i("h", 0);
            kv.set("pixfmt", "");
            kv.set_i("fpsnum", 0);
            kv.set_i("fpsden", 0);
            kv.set_i("nb_frames", -1);
            kv.set_i("rotation", 0);
        } else {
            AVStream *st = ic->streams[vi];
            const AVCodecParameters *cp = st->codecpar;

            const char *cname = avcodec_get_name(cp->codec_id);
            kv.set("codec", cname ? cname : "");
            kv.set("codec_tag", fourcc_str(cp->codec_tag));

            const char *pname = (cp->profile != AV_PROFILE_UNKNOWN)
                                    ? avcodec_profile_name(cp->codec_id, cp->profile)
                                    : nullptr;
            kv.set("profile", pname ? pname : "");

            kv.set_i("w", cp->width);
            kv.set_i("h", cp->height);
            const char *pix = av_get_pix_fmt_name((enum AVPixelFormat)cp->format);
            kv.set("pixfmt", pix ? pix : "");

            AVRational fr = st->avg_frame_rate;
            if (fr.num <= 0 || fr.den <= 0) fr = st->r_frame_rate;
            if (fr.num <= 0 || fr.den <= 0) { fr.num = 0; fr.den = 1; }
            kv.set_i("fpsnum", fr.num);
            kv.set_i("fpsden", fr.den);

            // 帧数在流上(AVCodecParameters 没有 nb_frames 字段)
            nb_frames = (st->nb_frames > 0) ? st->nb_frames : -1;
            kv.set_i("nb_frames", nb_frames);
            kv.set_i("rotation", detect_rotation_deg(st, "auto"));

            // 容器没给时长时,用视频流时长兜底
            if (duration_us < 0 && st->duration != AV_NOPTS_VALUE && st->time_base.num > 0) {
                duration_us = av_rescale_q(st->duration, st->time_base, utb);
                kv.set_i("duration_us", duration_us);
            }
        }
    }

    // ---------- 音频 ----------
    {
        const int ai = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (ai < 0) {
            kv.set("audio_codec", "");
            kv.set_i("audio_ch", 0);
            kv.set_i("audio_rate", 0);
        } else {
            const AVCodecParameters *cp = ic->streams[ai]->codecpar;
            const char *an = avcodec_get_name(cp->codec_id);
            kv.set("audio_codec", an ? an : "");
            kv.set_i("audio_ch", cp->ch_layout.nb_channels);
            kv.set_i("audio_rate", cp->sample_rate);
        }
    }

    // ---------- 色彩标签(与 spec 的 cspace/cprim/ctrc 同款小写标签) ----------
    {
        const AVCodecParameters *cp = nullptr;
        const int vi = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vi >= 0) cp = ic->streams[vi]->codecpar;

        std::string cs, cp2, trc;
        if (cp) {
            // ffmpeg 自带标签(av_color_space_name 之类);unspecified 统一给空串
            const char *s1 = av_color_space_name(cp->color_space);
            const char *s2 = av_color_primaries_name(cp->color_primaries);
            const char *s3 = av_color_transfer_name(cp->color_trc);
            if (s1 && std::strcmp(s1, "unknown") != 0) cs = s1;
            if (s2 && std::strcmp(s2, "unknown") != 0) cp2 = s2;
            if (s3 && std::strcmp(s3, "unknown") != 0) trc = s3;
        }
        kv.set("cspace", cs);
        kv.set("cprim", cp2);
        kv.set("ctrc", trc);
    }

    fd_io_free_input(&ic, &ioctx, &pb, &buf);
    // nativeInfo 也拥有输入 fd(契约 §2):用完关闭
    close(fd);
    return join_kv(kv);
}

} // namespace

// ---------------------------------------------------------------------------
// JNI 桥:cache JavaVM
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    g_vm = vm;
    av_log_set_level(AV_LOG_INFO);
    av_log_set_callback(pf_av_log);
    // FFmpeg 的 mediacodec 封装(编解码器 + hwcontext)要通过 av_jni_get_java_vm()
    // 拿 JNIEnv 去调 AMediaCodec;不注册的话 mediacodec 编码器一 init 就失败,
    // 整个 fallback 链会退化到 libx264(软件编码)。传 NULL log_ctx 即可。
    // 注意:FFmpeg 必须以 --enable-jni 编译(--enable-mediacodec 的前置条件),否则链接失败。
    av_jni_set_java_vm(vm, nullptr);
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_proresforge_app_engine_FFmpeg_nativeRun(JNIEnv *env, jobject thiz,
                                                jint fd, jlong length, jint outFd,
                                                jstring spec, jobject progress)
{
    (void)thiz;
    const bool owns_lifecycle = proresforge::begin_transcode();
    if (env->GetJavaVM(&g_vm) != JNI_OK) g_vm = nullptr;
    if (!owns_lifecycle) {
        // 并发重入防护:上一次 nativeRun 还没结束(现架构串行,不可达,防御性)。
        // 不动别人的生命周期边界;本调用的 fd 所有权在 Kotlin 侧已移交,这里负责关闭。
        if (fd >= 0) close(fd);
        if (outFd >= 0 && outFd != fd) close(outFd);
        return AVERROR(EBUSY);
    }

    // spec 原文(UTF-8)
    std::string spec_text;
    if (spec) {
        const char *chars = env->GetStringUTFChars(spec, nullptr);
        if (!chars) {
            if (owns_lifecycle) proresforge::end_transcode();
            if (fd >= 0) close(fd);
            if (outFd >= 0 && outFd != fd) close(outFd);
            return AVERROR(ENOMEM);
        }
        spec_text = chars;
        env->ReleaseStringUTFChars(spec, chars);
    }

    // 进度回调:全局引用 + method ID(失败则本次不报进度)
    bool have_progress = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        drop_progress_locked(env);
        if (progress) {
            jclass cls = env->GetObjectClass(progress);
            if (cls) {
                jmethodID m = env->GetMethodID(cls, "onProgress", "(ILjava/lang/String;JJ)V");
                if (m) {
                    jclass gref = (jclass)env->NewGlobalRef(cls);
                    jobject pref = env->NewGlobalRef(progress);
                    if (gref && pref) {
                        g_progress_cls = gref;
                        g_progress = pref;
                        g_progress_method = m;
                        have_progress = true;
                    } else {
                        if (gref) env->DeleteGlobalRef(gref);
                        if (pref) env->DeleteGlobalRef(pref);
                    }
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }
    }

    TranscodeParams p;
    p.fd_in = (int)fd;
    p.length = (int64_t)length;
    p.fd_out = (int)outFd;
    p.spec_text = spec_text;
    p.on_progress = have_progress ? jni_progress : nullptr;
    p.user = nullptr;

    // R10-8:run_transcode 内部会按条目声明分配(单帧上限 512MB,dngzip.cpp:91-94),
    // bad_alloc 若穿过 JNI 边界,Android 上直接 abort 进程(不是「下次调用 EBUSY」)。
    // 捕获后:进度引用照常释放(见下面公共块),生命周期由 owns_lifecycle 收尾,
    // 返回一个非 0 负码让 Kotlin 的 else 分支当失败(err_ffmpeg) —— 绝不能返回 0
    // 冒充成功。worker 路径(run_transcode 的调用方)本来就有 catch,主线程这条没有。
    jint result = -1;
    bool threw = false;
    try {
        result = (jint)run_transcode(p);
    } catch (const std::exception &e) {
        av_log(nullptr, AV_LOG_ERROR, "nativeRun: run_transcode 异常 \"%s\"\n", e.what());
        threw = true;
    } catch (...) {
        av_log(nullptr, AV_LOG_ERROR, "nativeRun: run_transcode 未知异常\n");
        threw = true;
    }

    {
        std::lock_guard<std::mutex> lk(g_mu);
        drop_progress_locked(env);
    }
    if (owns_lifecycle) proresforge::end_transcode();
    if (threw) return -12;   // ENOMEM:HIDL 无关,取一个稳定非 0 值供 UI 显示
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_proresforge_app_engine_FFmpeg_nativeCancel(JNIEnv *env, jobject thiz)
{
    (void)env;
    (void)thiz;
    request_cancel();
    return 0;
}

// 作废尚未被 begin_transcode 消费的 pending 取消。
// Kotlin 侧的取消动作可能在 nativeRun 之前完成(probe/fd 早退、或任务已被判
// CANCELLED 而跳过 nativeRun);此时 native 的 g_cancel_pending 若残留,会被
// 下一次 nativeRun 的 begin_transcode 当成「这个新任务也要取消」。
extern "C" JNIEXPORT void JNICALL
Java_com_proresforge_app_engine_FFmpeg_clearPendingCancel(JNIEnv *env, jobject thiz)
{
    (void)env;
    (void)thiz;
    clear_cancel_pending();
}

extern "C" JNIEXPORT void JNICALL
Java_com_proresforge_app_engine_FFmpeg_initNativeLogging(JNIEnv *env, jobject thiz, jstring path)
{
    (void)thiz;
    std::lock_guard<std::mutex> lk(g_log_mu);
    if (!path) {
        g_log_path.clear();
        pf_log_reopen();   // 关掉常驻句柄,别继续往旧文件里写
        return;
    }
    const char *p = env->GetStringUTFChars(path, nullptr);
    if (p) {
        g_log_path = p;
        env->ReleaseStringUTFChars(path, p);
        pf_log_reopen();   // 路径可能变了(AppLog 重指向):跟到新路径
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_proresforge_app_engine_FFmpeg_nativeInfo(JNIEnv *env, jobject thiz, jint fd, jlong length)
{
    (void)thiz;
    // R10-8:同样兜住 bad_alloc 等(collect_media_info 内部按容器/条目分配)。
    // 返回空串 —— 空串已经是现有失败语义(Kotlin 侧按「无法解析」处理),
    // 不为此新开一条 JNI 错误通道。
    std::string text;
    try {
        text = collect_media_info((int)fd, (int64_t)length);
    } catch (const std::exception &e) {
        av_log(nullptr, AV_LOG_ERROR, "nativeInfo: 异常 \"%s\"\n", e.what());
    } catch (...) {
        av_log(nullptr, AV_LOG_ERROR, "nativeInfo: 未知异常\n");
    }
    if (text.empty()) return env->NewStringUTF("");
    return env->NewStringUTF(text.c_str());
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_proresforge_app_engine_FFmpeg_nativePreview(JNIEnv *env, jobject thiz, jint fd, jlong length,
                                                     jlong atUs, jstring lutPath, jdouble lutIntensity,
                                                     jboolean graded, jint maxW,
                                                     jint dngQuality, jint dngColor, jint dngMap)
{
    (void)thiz;
    const char *lut = nullptr;
    if (lutPath) {
        // N-10:GetStringUTFChars 失败(OOM)会返回 null;静默把 lut 当 nullptr 传下去
        // 会按「无 LUT」出图 —— 用户以为调色生效了。明确失败返回 null(无预览)。
        lut = env->GetStringUTFChars(lutPath, nullptr);
        if (!lut) return nullptr;
    }

    // dngq/dngcolor/dngmap 之后必须和转码 spec 一致(N-7),越界由 preview.cpp 退回默认档。
    // N-03:恶意/异常输入可能让内部容器分配失败(bad_alloc 等);在 JNI 边界兜住,
    // 失败按「无预览」(null)处理,不让 C++ 异常穿透 JNI 边界崩溃进程。
    std::vector<uint8_t> jpeg;
    try {
        jpeg = render_preview_jpeg(
            (int)fd, (int64_t)length, (int64_t)atUs, lut,
            (double)lutIntensity, graded == JNI_TRUE, (int)maxW,
            (int)dngQuality, (int)dngColor, (int)dngMap);
    } catch (const std::exception &e) {
        av_log(nullptr, AV_LOG_ERROR, "nativePreview: 异常 %s\n", e.what());
    } catch (...) {
        av_log(nullptr, AV_LOG_ERROR, "nativePreview: 未知异常\n");
    }

    if (lut) env->ReleaseStringUTFChars(lutPath, lut);
    // N-04:跨语言契约 —— 失败(空 vector)返回 null,Kotlin 侧判 null 属正常路径。
    if (jpeg.empty()) return nullptr;
    // jsize 是 32 位:jint maxW 上限 640,正常不可达,但作为边界防御仍拒绝窄化溢出。
    if (jpeg.size() > (size_t)INT32_MAX) {
        av_log(nullptr, AV_LOG_ERROR, "nativePreview: 输出过大(%zu 字节),拒绝返回\n", jpeg.size());
        return nullptr;
    }

    jbyteArray out = env->NewByteArray((jsize)jpeg.size());
    if (!out) return nullptr;
    env->SetByteArrayRegion(out, 0, (jsize)jpeg.size(), (const jbyte *)jpeg.data());
    return out;
}
