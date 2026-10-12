// preview.cpp —— 单帧预览:seek → 解码一帧 → 可选 LUT → 缩放 → MJPEG(quality 85)
//
// 契约 §2 nativePreview / §7 要点:
//   * fd 为 Kotlin detachFd() 移交的输入文件(内部 close)
//   * 用 av_seek_frame(AVSEEK_FLAG_BACKWARD) 定位到 atUs 之前的最近关键帧,
//     再解码到"第一帧 pts >= atUs"的那一帧;若一帧都没解出来则返回空 vector(失败)
//   * 可选 filtergraph:buffer → [transpose(autorotate) → format=gbrp → lut3d → blend]
//     → scale → format=yuvj420p → buffersink
//     (n6.1 的 lut3d 只吃 RGB planar,先 format 成 gbrp;8 位预览用 gbrp)
//   * graded=false 或 lut 路径为空时完全不过 LUT
//   * n6.1 mjpegenc.c 只支持 8 位(YUVJ420P/YUVJ422P/YUVJ444P + 非 full-range 变体),
//     统一编码成 yuvj420p
//   * 质量控制见 jpeg_q_from_quality 注释(6.1 里唯一可靠的控制点是 qmin/qmax)
//   * DNG 序列 zip(MotionCam,契约 §10.5):输入是 PK\x03\x04 时 atUs 被当作「帧号」,
//     走 dngzip/LibRaw 单帧解码,不进 avformat;JNI 签名不变(§2 是契约)

#include "preview.h"

#include "strspec.h"
#include "transcode.h"
#include "dngzip.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// zip 分支的诊断日志(与 dngzip.cpp 同一出口:native-lib 的 pf_av_log)。
// libavutil/log.h 没有 extern "C" 保护,必须自己包(否则 C++ 符号链接不上)。
extern "C" {
#include <libavutil/log.h>
}

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
}

#include <unistd.h>

namespace proresforge {
namespace {

// 契约 §2 规定预览 JPEG 质量为 85(0..100 量表)。
const int kPreviewQuality = 85;

// 内存 AVIO 的累计写入上限(N-03):预览正常只有几百 KB(带 LUT/高分辨率也就几 MB);
// 超限说明输入异常(或逻辑 bug),通过返回负值让 avio 置 error,out_pb->error 检查处判失败,
// 防止恶意/损坏输入把内存吃穿。上限取 64 MB,远大于任何合法预览。
const size_t kPreviewMaxBytes = 64u * 1024 * 1024;

// JPEG 质量(0..100,越大越好)→ MPEG 系 qscale(1..31,越小越精细),等价于 ffmpeg CLI 的 -q:v。
// 100→1, 85→5, 50→16, 0→31。
//
// 为什么必须同时给 qmin/qmax:
//   FFmpeg 6.1 的 mjpegenc.c 没有任何 quality/qscale AVOption,质量完全走 MPEG 系
//   mpegvideo_enc.c 路径:quant 表 = 默认矩阵 * s->qscale / 8,而 s->qscale 由
//   update_qscale() 生成并被 av_clip 到 [qmin, qmax]。global_quality / frame->quality
//   只是 lambda(码控估计)的输入,单帧 MJPEG 下不可靠。
//   置 QSCALE flag 时 ff_rate_estimate_qscale 被完全跳过(fixed_qscale),lambda 保持 0,
//   qscale 被 clip 到 qmin==qmax,即精确等于目标 qscale。
int jpeg_q_from_quality(int quality)
{
    int q = quality;
    if (q < 0) q = 0;
    if (q > 100) q = 100;
    const int qs = ((100 - q) * 30) / 100 + 1;
    if (qs < 1) return 1;
    if (qs > 31) return 31;
    return qs;
}

// 转义 filter 选项字符串里的特殊字符(LUT 路径可能带 ':' 等)
std::string esc_path(const char *p)
{
    std::string s;
    if (!p) return s;
    for (const char *q = p; *q; ++q) {
        if (*q == ':' || *q == ';' || *q == ',' || *q == '\'' || *q == '\\')
            s.push_back('\\');
        s.push_back(*q);
    }
    return s;
}

// ---------------------------------------------------------------------------
// 内存 AVIO(mjpeg 输出):顺序追加写,只响应 AVSEEK_SIZE
// ---------------------------------------------------------------------------
struct PreviewMemIoCtx {
    std::vector<uint8_t> *sink = nullptr;
};

int mem_write(void *opaque, uint8_t *buf, int buf_size)
{
    PreviewMemIoCtx *c = static_cast<PreviewMemIoCtx *>(opaque);
    if (buf_size <= 0) return 0;
    // N-03:累计上限(见 kPreviewMaxBytes 注释)。返回负值会让 AVIOContext 置 error。
    if (c->sink->size() + (size_t)buf_size > kPreviewMaxBytes) return AVERROR(EFBIG);
    c->sink->insert(c->sink->end(), buf, buf + buf_size);
    return buf_size;
}

int64_t mem_seek(void *opaque, int64_t offset, int whence)
{
    PreviewMemIoCtx *c = static_cast<PreviewMemIoCtx *>(opaque);
    if (whence == AVSEEK_SIZE) return (int64_t)c->sink->size();
    (void)offset;
    // 单帧 JPEG 只顺序写,不实现真正的 seek
    return AVERROR(ENOSYS);
}

// buffersrc 参数字符串(宽高/像素格式/时基/采样宽高比)
void buffersrc_args(char *dst, size_t n, const AVFrame *f, const AVRational &tb)
{
    AVRational sar = f->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) { sar.num = 1; sar.den = 1; }
    std::snprintf(dst, n,
                  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                  f->width, f->height, (int)f->format,
                  tb.num, tb.den, sar.num, sar.den);
}

} // namespace

std::vector<uint8_t> render_preview_jpeg(int fd, int64_t length, int64_t at_us,
                                        const char *lut_path, double lut_intensity,
                                        bool graded, int max_w,
                                        int dng_quality, int dng_color, int dng_map)
{
    std::vector<uint8_t> result;    // 返回值:空 = 失败

    AVFormatContext *ic = nullptr;
    FdIoCtx *ioctx = nullptr;
    AVIOContext *in_pb = nullptr;
    uint8_t *in_buf = nullptr;
    AVCodecContext *dec = nullptr;
    AVFilterGraph *graph = nullptr;
    AVFilterContext *fsrc = nullptr;
    AVFilterContext *fsink = nullptr;
    AVFrame *recv = nullptr;
    AVFrame *cand = nullptr;
    AVFrame *filt = nullptr;
    AVPacket *pkt = nullptr;
    AVCodecContext *enc = nullptr;
    AVPacket *opkt = nullptr;
    AVIOContext *out_pb = nullptr;
    uint8_t *out_buf = nullptr;
    PreviewMemIoCtx mem;

    if (fd < 0) return result;

    // R10-7:fd 契约 §2 —— native 拥有它,用完 close。正常路径在 cleanup 尾部
    // close;但 mem_write 的 vector::insert / LibRaw 解码分配会抛 bad_alloc,
    // 异常跳过 goto cleanup(native-lib.cpp 的 catch 把异常收成「返回 null」,
    // Kotlin 的 MediaProbe 只在 Java 异常时 close —— 这条路 Java 侧看到的是
    // 正常 null,fd 就漏了)。用栈展开保证 close:goto/return 之前 close 过的
    // fd 在这里记 -1,析构只 close 真正还开着的那个(绝不 double-close)。
    // (本地 struct 已抽到 transcode.h 的 proresforge::FdGuard,三处共用。)
    int fd_owned = fd;
    FdGuard fd_releaser{ &fd_owned };

    // ------------------------- 输入 / 解码 -------------------------
    int rot_deg = 0;                // 几何旋转角(zip 源没有容器旋转信息,恒 0)
    AVRational src_tb = { 1, 25 };  // buffersrc 时基(zip 源没有容器时基)

    // DNG 序列 zip(MotionCam,契约 §10.5):先探 4 字节魔数。
    // 命中时 atUs 的语义是「帧号」而不是微秒 —— Kotlin 侧预览 zip 传的是 frame index。
    bool zip_src = false;
    {
        uint8_t magic[4] = { 0, 0, 0, 0 };
        // offset 0 不会截断,用 *64 变体仅为一致(见 dngzip.cpp 顶部)
        if (pread64(fd, magic, 4, 0) == 4 && DngZip::is_zip(magic, 4)) zip_src = true;
    }

    DngZip dngzip;   // 只有 zip_src 时有效
    if (zip_src) {
        // 只解目标帧:解压 → LibRaw(快速档,半尺寸 demosaic 足够做预览)。
        // 复用 cand 之后的所有流程(尺寸/LUT/缩放/mjpeg),零分支重复。
        // 失败要打日志(真机反馈「无法解析」时无迹可寻);目标帧解不动时
        // 从目标帧向后最多 8 帧找第一张能解的 —— 首帧被截断/半帧的 zip 也要有预览。
        // R10-6:必须封顶。批次 6 之前预览不做半尺寸降采样,每一帧都是全幅
        // read_frame + LibRaw;无上限时目标帧坏掉会把整包(几百帧)扫一遍,
        // 预览跑在 Dispatchers.IO 上,取消协程也进不了 JNI 内部。
        // 探测路径(native-lib.cpp 的 am 探测)同样最多 8 帧,这里对齐。
        const int kPreviewBackscan = 8;
        // N-7:预览按 JNI 传入的 dngq/dngcolor/dngmap 解,与转码 spec 一致;
        // 不再写死 FAST/CAMERA(否则预览画面和成品画质/白平衡对不上)。
        // 档位越界退回默认,防 JNI 侧传错。
        if (dng_quality < (int)DngQuality::FAST || dng_quality > (int)DngQuality::FINE) {
            dng_quality = (int)DngQuality::FAST;
        }
        if (dng_color < (int)DngColorMode::CAMERA || dng_color > (int)DngColorMode::D65) {
            dng_color = (int)DngColorMode::CAMERA;
        }
        if (dng_map != (int)DngMap::APPLE_LOG) dng_map = (int)DngMap::NONE;
        const DngQuality dq = (DngQuality)dng_quality;
        const DngColorMode dcm = (DngColorMode)dng_color;
        std::string err;
        if (dngzip.open(fd, length, err) != 0) {
            av_log(nullptr, AV_LOG_WARNING, "preview: zip open 失败:%s\n", err.c_str());
            goto cleanup;
        }
        if (dngzip.frame_count() == 0) {
            av_log(nullptr, AV_LOG_WARNING, "preview: zip 里没有 .dng 帧,无预览\n");
            goto cleanup;                     // 非 DNG zip / 空 zip → 空(不可解析)
        }
        size_t idx = (size_t)(at_us < 0 ? 0 : at_us);
        if (idx >= dngzip.frame_count()) idx = dngzip.frame_count() - 1;
        std::vector<uint8_t> buf;
        std::vector<uint16_t> rgb;
        int w = 0, h = 0;
        bool got = false;
        for (size_t i = idx; i < dngzip.frame_count() && i - idx < (size_t)kPreviewBackscan; i++) {
            std::string derr;
            if (dngzip.read_frame(i, buf, derr) != 0) {
                av_log(nullptr, AV_LOG_WARNING, "preview: 第 %zu 帧解压:%s\n", i + 1,
                       derr.c_str());
                continue;
            }
            // N-7:预览按 JNI 传入的 dngq/dngcolor/dngmap 解,和转码 spec 一致;
            // 不再写死 FAST/CAMERA(否则预览和成品画质/白平衡不一致)。
            // N-08:want_half_size=true —— 预览只要「看得见的图」。4096x3072
            // 全幅 RGB48 ≈ 72MB 解码占用,而预览输出上限 kPreviewMaxBytes
            // 只有 64MB,全幅预览在编码前就撑爆;半尺寸输出 ≈18MB,长边缩放
            // (见下面目标尺寸段)再把 640 边的 mjpeg 压到几十 KB。
            // 注意这是**显式的降采样解码参数**(默认关),不是偷偷把档位换成
            // FAST:demosaic 画质档 q 原样透传。
            if (dng_decode_to_rgb48(buf.data(), buf.size(), dq, dcm, rgb, w, h, derr,
                                    nullptr, nullptr, (DngMap)dng_map, true) != 0) {
                av_log(nullptr, AV_LOG_WARNING, "preview: 第 %zu 帧解码:%s\n", i + 1,
                       derr.c_str());
                continue;
            }
            idx = i;
            got = true;
            break;
        }
        if (!got) {
            av_log(nullptr, AV_LOG_ERROR, "preview: 目标帧起全部解不开,无预览\n");
            goto cleanup;
        }
        cand = av_frame_alloc();
        if (!cand) goto cleanup;
        cand->format = AV_PIX_FMT_RGB48LE;
        cand->width = w;
        cand->height = h;
        cand->pts = 0;
        cand->sample_aspect_ratio = (AVRational){ 1, 1 };
        if (av_frame_get_buffer(cand, 32) < 0) goto cleanup;
        for (int y = 0; y < h; y++) {
            uint8_t *dst = cand->data[0] + (size_t)y * (size_t)cand->linesize[0];
            const uint16_t *src = rgb.data() + (size_t)y * (size_t)w * 3;
            memcpy(dst, (const void *)src, (size_t)w * 3 * sizeof(uint16_t));
        }
        buf.clear();
        buf.shrink_to_fit();
        rgb.clear();
        rgb.shrink_to_fit();
        av_log(nullptr, AV_LOG_INFO, "preview: zip 帧 %zu/%zu 预览成功(%dx%d)\n", idx + 1,
               dngzip.frame_count(), w, h);
        goto zip_ready;
    }

    if (fd_io_open_input(fd, length, &ic, &ioctx, &in_pb, &in_buf) < 0)
        goto cleanup;

    {
        const int vi = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vi < 0) goto cleanup;
        AVStream *vst = ic->streams[vi];

        const AVCodec *codec = avcodec_find_decoder(vst->codecpar->codec_id);
        if (!codec) goto cleanup;
        dec = avcodec_alloc_context3(codec);
        if (!dec) goto cleanup;
        if (avcodec_parameters_to_context(dec, vst->codecpar) < 0) goto cleanup;
        // 预览只解一两帧,单线程即可(帧线程反而多一帧延迟)。
        // 注意 FFmpeg 语义:thread_count == 0 是「自动(可能多线程)」,1 才是真正的单线程。
        dec->thread_count = 1;
        if (avcodec_open2(dec, codec, nullptr) < 0) goto cleanup;

        recv = av_frame_alloc();
        cand = av_frame_alloc();
        pkt = av_packet_alloc();
        if (!recv || !cand || !pkt) goto cleanup;

        // seek 到目标时间点(BACKWARD:落在 atUs 之前最近的关键帧上)
        {
            const AVRational utb = { 1, AV_TIME_BASE };   // 微秒时基
            const int64_t target = av_rescale_q(at_us, utb, vst->time_base);
            av_seek_frame(ic, vi, target, AVSEEK_FLAG_BACKWARD);
            // seek 之后必须冲刷解码器:里面还残留着上一段的参考帧/状态,
            // 不 flush 会先吐出 seek 点之前的画面(预览图与目标时间点对不上)。
            avcodec_flush_buffers(dec);
        }

        // 解码到"第一帧 pts >= atUs";一帧都没解出来则失败。
        // N-01:send/receive 分层 —— EAGAIN(解码器输出满)必须先排空再重送同一个
        // packet(不丢包),真错误才丢包继续;flush 包的返回值也分层处理。旧实现
        // 把 EAGAIN 与真错误同路「丢包 continue」,且 flush 不查返回值。
        // R10-1:两种「零进展」必须封顶,否则 JNI 永不返回:
        //   (a) have_packet 为真、send_packet 收 EAGAIN、紧接着 receive 也是
        //       EAGAIN —— 包不丢、flushed 仍为 false,:338 的出口条件不成立;
        //   (b) av_read_frame 已 EOF,flush(send_packet(nullptr))收 EAGAIN,
        //       receive 也 EAGAIN —— :303-305 只对非 EAGAIN 才 break,
        //       下一轮再读一次 EOF,原地打转。
        // 连续零进展满 kDecodeStallLimit 轮就退出循环:手上有帧就用,
        // 没有帧则走下面既有的空预览失败路径。
        const int kDecodeStallLimit = 8;
        int stall = 0;
        {
            const AVRational utb = { 1, AV_TIME_BASE };
            const int64_t want = av_rescale_q(at_us, utb, vst->time_base);
            bool have = false;
            bool have_packet = false;   // pkt 里有一包待送(送成功才清)
            bool flushed = false;       // 已给解码器发过 flush 包
            while (!have) {
                bool progressed = false;    // 本轮是否真收到帧/送出包
                if (!have_packet && !flushed) {
                    const int r = av_read_frame(ic, pkt);
                    if (r < 0) {
                        if (r == AVERROR_EOF) {
                            // EOF:冲刷解码器里可能残留的帧(有些解码器有延迟)
                            const int fr = avcodec_send_packet(dec, nullptr);
                            if (fr >= 0 || fr == AVERROR_EOF) {
                                flushed = true;
                                progressed = true;
                            } else if (fr != AVERROR(EAGAIN)) break;  // flush 被拒:退回已有帧
                            // EAGAIN:先排空,下一轮再发 flush
                        } else {
                            break;                      // 真读错误:退回已拿到的最后一帧
                        }
                    } else if (pkt->stream_index != vi) {
                        av_packet_unref(pkt);
                        progressed = true;              // 跳过别的流:读操作在推进
                    } else {
                        have_packet = true;
                        progressed = true;
                    }
                }
                if (have_packet) {
                    const int sr = avcodec_send_packet(dec, pkt);
                    if (sr >= 0) {
                        av_packet_unref(pkt);
                        have_packet = false;
                        progressed = true;
                    } else if (sr == AVERROR(EAGAIN)) {
                        // 输出满:不丢包,进下面的 receive 排空后重送同一个 packet
                    } else {
                        av_packet_unref(pkt);           // 真错误:丢包继续(原语义)
                        have_packet = false;
                        progressed = true;
                    }
                }
                for (;;) {
                    const int r2 = avcodec_receive_frame(dec, recv);
                    if (r2 < 0) break;                  // EAGAIN / EOF / 错误
                    progressed = true;
                    av_frame_unref(cand);
                    // R10-1:查 av_frame_ref 返回值 —— 失败时 cand 已被 unref
                    // (缓冲释放、字段失效),不能把「空的 cand」当成本轮的候选帧。
                    if (av_frame_ref(cand, recv) < 0) {
                        av_frame_unref(recv);
                        continue;                       // 丢这一帧,继续收后续帧
                    }
                    av_frame_unref(recv);
                    if (cand->width <= 0) continue;
                    const int64_t ts = (cand->best_effort_timestamp != AV_NOPTS_VALUE)
                                           ? cand->best_effort_timestamp : 0;
                    if (ts >= want) { have = true; break; }
                }
                if (flushed && !have_packet) break;     // flush 已发且排空完,收工
                if (!progressed) {
                    if (++stall > kDecodeStallLimit) {
                        av_log(nullptr, AV_LOG_WARNING,
                               "preview: 解码连续 %d 轮零进展(解码器/解复用器同时 EAGAIN),"
                               "退出解码循环\n",
                               stall);
                        break;
                    }
                } else {
                    stall = 0;                          // 有进展:抖动重新计数
                }
            }
            if (cand->width <= 0) goto cleanup;
            // 预览同样要做 autorotate:竖拍素材的 display matrix 若不应用,缩略图就是横的,
            // 与转码(rotate=auto)结果不一致。
            rot_deg = detect_rotation_deg(vst, "auto");
        }
        src_tb = vst->time_base;
    }

zip_ready:
    // ------------------------- 目标尺寸 -------------------------
    {
        // N-07:预览按**长边**缩,max_w 的 Kotlin 语义(PREVIEW_MAX_W)就是
        // 「预览图最大边」。旧代码只比宽度:竖拍素材(宽 ≤ 640、高几千)不触发缩小,
        // 缩放后宽仍是 640、高不变 → mjpeg 4:2:0 顶到 64MB(kPreviewMaxBytes)
        // 上限,被判失败,用户看到「无法解析」。旋转会交换宽高,所以先按「旋转后」
        // 的尺寸去比 max_w(deg 180 不交换,已在调用方体现为不交换)。
        int src_w = cand->width;
        int src_h = cand->height;
        if (rot_deg == 90 || rot_deg == 270) { const int t = src_w; src_w = src_h; src_h = t; }
        int dw = src_w;
        int dh = src_h;
        // 长边超过 max_w 才等比缩小;宽高仍取偶数(mjpeg 8 位 4:2:0 的要求)。
        if (max_w > 0 && (dw > max_w || dh > max_w)) {
            if (dw >= dh) {
                dw = max_w;
                dh = (int)((int64_t)src_h * max_w / src_w);
            } else {
                dh = max_w;
                dw = (int)((int64_t)src_w * max_w / src_h);
            }
        }
        if (dw > 3) dw &= ~1;
        if (dh > 3) dh &= ~1;
        if (dw < 2) dw = 2;
        if (dh < 2) dh = 2;

        double intensity = lut_intensity;
        if (intensity < 0.0) intensity = 0.0;
        if (intensity > 1.0) intensity = 1.0;
        const bool use_lut = graded && lut_path && lut_path[0] && intensity > 0.0;

        // ------------------------- 拼接 filtergraph 描述串 -------------------------
        // [in]/[out] 连到下面手动创建的 buffersrc("in")/buffersink("out");
        // buffer 的参数由 avfilter_graph_create_filter 传入,不进描述串
        // (描述串以裸参数开头会被当成滤镜名解析,报 "No option name near")。
        std::string descr = "[in]";

        // 先做几何旋转(与转码 rotate=auto 同序:rotate → scale → LUT)
        if (rot_deg == 90) descr += "transpose=clock,";
        else if (rot_deg == 270) descr += "transpose=cclock,";
        else if (rot_deg == 180) descr += "hflip,vflip,";

        if (use_lut) {
            const std::string lut = esc_path(lut_path);
            // n6.1 lut3d 只支持 RGB planar(ARGB/…/GBRP/…/GBRP16),8 位预览转 gbrp
            descr += "format=gbrp,";
            if (intensity >= 0.999) {
                // 全强度:直接过 LUT
                descr += "lut3d=file=" + lut + ":interp=tetrahedral,";
            } else {
                // 部分强度:split 出原图 + LUT 图,按 all_opacity 混合
                char op[32];
                std::snprintf(op, sizeof(op), "%.4f", intensity);
                descr += "split=2[a][b];";
                descr += "[a]lut3d=file=" + lut + ":interp=tetrahedral[ga];";
                descr += "[b][ga]blend=all_mode=normal:all_opacity=" + std::string(op) + ",";
            }
        }

        {
            char tail[64];
            std::snprintf(tail, sizeof(tail), "scale=%d:%d:flags=lanczos,format=yuvj420p", dw, dh);
            descr += tail;
        }
        descr += "[out]";

        // ------------------------- 手工构建 graph -------------------------
        const AVFilter *fbuf = avfilter_get_by_name("buffer");
        const AVFilter *fsnk = avfilter_get_by_name("buffersink");
        if (!fbuf || !fsnk) goto cleanup;

        graph = avfilter_graph_alloc();
        if (!graph) goto cleanup;

        char args[512];
        buffersrc_args(args, sizeof(args), cand, src_tb);
        if (avfilter_graph_create_filter(&fsrc, fbuf, "in", args, nullptr, graph) < 0)
            goto cleanup;
        if (avfilter_graph_create_filter(&fsink, fsnk, "out", nullptr, nullptr, graph) < 0)
            goto cleanup;

        {
            AVFilterInOut *outputs = avfilter_inout_alloc();
            AVFilterInOut *inputs = avfilter_inout_alloc();
            bool ok = (outputs && inputs);
            if (ok) {
                outputs->name = av_strdup("in");
                outputs->filter_ctx = fsrc;
                outputs->pad_idx = 0;
                outputs->next = nullptr;
                inputs->name = av_strdup("out");
                inputs->filter_ctx = fsink;
                inputs->pad_idx = 0;
                inputs->next = nullptr;
                if (avfilter_graph_parse_ptr(graph, descr.c_str(), &inputs, &outputs, nullptr) < 0)
                    ok = false;
            }
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            if (!ok) goto cleanup;
        }

        if (avfilter_graph_config(graph, nullptr) < 0) goto cleanup;

        // 送一帧进滤镜图;单帧预览不 push nullptr(那是 EOF 刷新)
        if (av_buffersrc_add_frame(fsrc, cand) < 0) goto cleanup;

        filt = av_frame_alloc();
        if (!filt) goto cleanup;
        if (av_buffersink_get_frame(fsink, filt) < 0) goto cleanup;
    }

    // ------------------------- mjpeg 编码 -------------------------
    {
        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!codec) goto cleanup;
        enc = avcodec_alloc_context3(codec);
        if (!enc) goto cleanup;
        enc->width = filt->width;
        enc->height = filt->height;
        enc->pix_fmt = AV_PIX_FMT_YUVJ420P;         // 8 位 full-range,JPEG 语义正确
        enc->time_base = AVRational{ 1, 25 };
        enc->framerate = AVRational{ 25, 1 };
        enc->gop_size = 12;                          // 已 intra only,仅避免 init 改写 gop
        const int q = jpeg_q_from_quality(kPreviewQuality);
        enc->qmin = q;
        enc->qmax = q;
        enc->global_quality = q * FF_QP2LAMBDA;
        enc->flags |= AV_CODEC_FLAG_QSCALE;          // 关码控,由 qmin/qmax 定 qscale
        if (avcodec_open2(enc, codec, nullptr) < 0) goto cleanup;

        // 内存 AVIO:avio_alloc_context 内部的 buffer 归调用者(见 fd_io_free_avio 同思路)
        mem.sink = &result;
        const int kBufSize = 32768;
        out_buf = static_cast<uint8_t *>(av_malloc(kBufSize));
        if (!out_buf) goto cleanup;
        out_pb = avio_alloc_context(out_buf, kBufSize, 1, &mem, nullptr, mem_write, mem_seek);
        if (!out_pb) { av_freep(&out_buf); out_buf = nullptr; goto cleanup; }

        filt->pts = 0;                               // mjpeg 不参与 GOP 时间,给个合法值
        filt->quality = enc->global_quality;
        const int sfr = avcodec_send_frame(enc, filt);
        if (sfr < 0 && sfr != AVERROR(EAGAIN)) goto cleanup;
        // EAGAIN:编码器还没做好准备(理论上 mjpeg 首帧不会 EAGAIN),不去
        // goto cleanup —— 异常路径丢掉的是收包循环之外的全部上下文;
        // 当作「先收包再继续」处理,让同一套收包逻辑判决成/败。
        opkt = av_packet_alloc();
        if (!opkt) goto cleanup;
        // N-02:send/receive/flush 分层。EAGAIN = 先收包再继续(flush 已发但输出
        // 未尽时继续收),EOF = 正常收工,其它 <0 = 真错误 → 整体判失败。旧实现
        // 把 receive 的「EOF/错误」与 flush 的 EAGAIN 混为一谈且 flush 不查返回值,
        // 编码器中途出错时会输出截断的 JPEG 且被判成功。
        // R10-2:flush 自己 EAGAIN 时旧代码 continue 回收包循环,若 receive 也
        // 一直 EAGAIN,则 :511 的 enc_ok=false 永远到不了 —— 死循环。
        // 连续「flush EAGAIN 且没有收到任何包」计一次零进展,
        // 满 kEncFlushStallLimit 轮即按失败收场(:516 result.clear()
        // 丢掉截断的 JPEG)。
        const int kEncFlushStallLimit = 8;
        int flush_stall = 0;
        {
            bool flushed = false;
            bool enc_ok = true;
            while (enc_ok) {
                const int rr = avcodec_receive_packet(enc, opkt);
                if (rr >= 0) {
                    avio_write(out_pb, opkt->data, opkt->size);   // n6.1 avio_write 返回 void
                    av_packet_unref(opkt);
                    flush_stall = 0;                        // 有产出:抖动重新计数
                    continue;
                }
                if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) {
                    if (!flushed) {
                        const int fr = avcodec_send_frame(enc, nullptr);   // 刷新
                        if (fr >= 0 || fr == AVERROR_EOF) {
                            flushed = true;
                            flush_stall = 0;
                        } else if (fr != AVERROR(EAGAIN)) {
                            enc_ok = false;               // flush 被拒:真错误
                        } else if (++flush_stall > kEncFlushStallLimit) {
                            enc_ok = false;               // flush 空转封顶(R10-2)
                        }
                        // flush EAGAIN:回循环继续收(输出未尽)
                        continue;
                    }
                    if (rr == AVERROR_EOF) break;         // flush 已发且收尽:正常收工
                    enc_ok = false;                       // flush 后还 EAGAIN:异常,防死循环
                    continue;
                }
                enc_ok = false;                           // 真错误
            }
            if (!enc_ok || !flushed) result.clear();      // 截断/失败 → 空(= 无预览)
        }
        avio_flush(out_pb);
        if (out_pb->error < 0) result.clear();      // 写出失败 → 视为失败
    }

cleanup:
    // ------------------------- 收尾 -------------------------
    if (out_pb) {
        // 释放「当前」buffer(探测/写路径可能已被 libavformat 换掉,见 fd_io_free_avio)
        av_freep(&out_pb->buffer);
        avio_context_free(&out_pb);
        out_buf = nullptr;               // 旧指针已覆盖/已失效,不再单独 free
        out_pb = nullptr;
    } else if (out_buf) {
        av_freep(&out_buf);              // pb 没建起来:buffer 还是当初那块
        out_buf = nullptr;
    }
    if (opkt) av_packet_free(&opkt);
    if (enc) avcodec_free_context(&enc);
    if (filt) av_frame_free(&filt);
    if (graph) avfilter_graph_free(&graph);
    if (cand) av_frame_free(&cand);
    if (recv) av_frame_free(&recv);
    if (pkt) av_packet_free(&pkt);
    if (dec) avcodec_free_context(&dec);
    fd_io_free_input(&ic, &ioctx, &in_pb, &in_buf);

            // fd 契约 §2:native 拥有,用完 close —— 这里显式 close 并解除守卫,
            // goto/return 之外的异常出口(fd_io_open mem_write / vector::insert
            // 抛 bad_alloc、skip 0xff 0xff 长度过滤、fd_io_open fd 检查)由
            // fd_releaser 的析构兜底(R10-7)。两者互斥,不会 double-close。
            if (fd_owned >= 0) { close(fd_owned); fd_owned = -1; }
            return result;
}

} // namespace proresforge
