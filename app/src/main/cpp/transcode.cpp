// transcode.cpp
//
// 转码管线主体:
//   输入(fd) → 自定义 AVIO → avformat 解码 → filtergraph → 编码 → 自定义 AVIO → 输出(fd)
//   音频:copy 直通 / aac 重采样转码 / drop 丢弃
//   faststart(mp4/mov):结束后把 moov box 挪到 ftyp 之后
//
// 设计要点(与契约 INTERFACES.md 对齐):
//   * 自定义 AVIOContext 由本文件负责释放:avformat_open_input / avformat_close_input /
//     avformat_free_context 都不会关闭 CUSTOM_IO 的 pb(n6.1 源码实测);
//     buffer 的释放见 fd_io_free_avio(必须 free pb->buffer,不是留存指针)。
//   * 取消:全局 std::atomic<bool> g_cancel,主循环轮询;收到取消后立即收尾清理,
//     不写 trailer(残file由 Kotlin 删除)。
//   * 编码器按契约 fallback chain 用 avcodec_find_encoder_by_name 逐个尝试。
//   * 进度:reporter(pct, stage) 在转码线程直接回调 Kotlin(attach 后的同步调用)。

// prores_ks 帧级线程数:3(实测拐点=2,方案"从 3 或 4 中选",0=auto 禁止)。
// 需要 A/B 时用 -DPRF_PRORES_THREADS=1/2/4 覆盖。
#ifndef PRF_PRORES_THREADS
#define PRF_PRORES_THREADS 3
#endif

#include "transcode.h"
#include "dngzip.h"
#include "dngframeq.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <utility>
#include <limits>
#include <thread>
#include <mutex>
#include <new>

#include <unistd.h>
#include <errno.h>
#if defined(__ANDROID__)
#include <sched.h>
#endif

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libavutil/imgutils.h>
#include <libavutil/display.h>
#include <libavutil/channel_layout.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <libavcodec/bsf.h>
#include <libswresample/swresample.h>
}

namespace proresforge {

// ---------------------------------------------------------------------------
// 取消标志
// ---------------------------------------------------------------------------
std::atomic<bool> g_cancel{false};
std::mutex g_cancel_mu;
bool g_transcode_active = false;
bool g_cancel_pending = false;

bool begin_transcode()
{
    std::lock_guard<std::mutex> lk(g_cancel_mu);
    if (g_transcode_active) return false;
    // 消费 nativeRun 启动前已经到达的取消请求。任务结束时会清掉 pending,
    // 因而这里不会把上一任务的旧取消带进新任务。
    g_cancel.store(g_cancel_pending, std::memory_order_relaxed);
    g_cancel_pending = false;
    g_transcode_active = true;
    return true;
}

void end_transcode()
{
    std::lock_guard<std::mutex> lk(g_cancel_mu);
    g_transcode_active = false;
    g_cancel.store(false, std::memory_order_relaxed);
    g_cancel_pending = false;
}

void request_cancel()
{
    std::lock_guard<std::mutex> lk(g_cancel_mu);
    if (g_transcode_active) {
        g_cancel.store(true, std::memory_order_relaxed);
    } else {
        // JobBus 已将任务标成当前任务,但 nativeRun 尚未完成 JNI 参数准备时,
        // 取消先放在 pending,由 begin_transcode 消费为启动即取消。
        g_cancel_pending = true;
    }
}

// 让「未消费的 pending 取消」显式过期:
// 转码根本没起来(probe/fd 早退、nativeRun 前就被判 CANCELLED 跳过)时,残留的
// pending 会被下一次 begin_transcode 消费,把无关的新任务误判成取消。
// 只清 pending:任务已经开跑(g_transcode_active)时取消的是当前转码,不能抹。
void clear_cancel_pending()
{
    std::lock_guard<std::mutex> lk(g_cancel_mu);
    if (!g_transcode_active) g_cancel_pending = false;
}

bool is_cancelled() { return g_cancel.load(std::memory_order_relaxed); }

// R10-5:转码收尾 flush 的连续「零进展」轮数上限。零进展 = send_frame(NULL)
// 返回 EAGAIN、紧接着 receive_packet 也是 EAGAIN、且这一个包都没写出去。
// 符合约定的编码器在 send EAGAIN 时 receive 应吐包;硬件编码器异步取缓冲时
// 两边同时 EAGAIN 是真实可能的状态,但把两级 EAGAIN 各自吸收掉才是正确行为 ——
// 停在「两边同时 EAGAIN 且没有任何包写出」= 零进展,继续等只会把取消拖住
// (取消点在主循环,flush 期间不再有人看 g_cancel),最终是转码关不掉。
// 上限吸收合法抖动,触顶则以真实负错误走 done,不把截断文件报成成功。
constexpr int kFlushStallLimit = 8;

// ---------------------------------------------------------------------------
// DNG 序列 zip 的并行解码定参(dngthreads/dngdepth)
// ---------------------------------------------------------------------------
// 为什么不是「有几个核就开几个线程」:LibRaw 解一帧的内存峰值与分辨率成正比,
// 实测(4096x3072 rgb48le=72MB)VmHWM 213MB,即 ≈ rgb 字节数 × 2.95 + 16MB
// (DNG 压缩态 15.7MB + unpack raw 25MB + imgdata.image 100MB + 输出 rgb 72MB,
// 其中大部分 malloc 块 recycle 后不还给内核)。N 路并行就要 N 份,所以线程数
// 必须由内存预算反推,否则 4K DNG 序列会直接把进程推到 LMK 枪口上。
// 预算默认 1GB(= kBudget,见下):dngthreads>0 可强制指定(调参/绕过低内存机型)。
// 同时给主线程(滤镜+编码+写盘)留一个核:线程数最多 cores-1。
// 预取深度默认 2(depth=1 会退化成串行 —— 帧 i+1 的解码必须等帧 i 被取走才开始,
// 实测 433ms→424ms 没有重叠;depth=3/4 相比 2 几乎没有额外收益却多占 72MB/帧)。
// P1-1(2026-10-06)复核「默认 depth 是否随 dngthreads 联动」:host 60 帧矩阵
// (interleaved 4 reps,T=2/T=4 × d2/d3/d4)显示 d 越大墙钟与 user CPU 一致地升
// (瓶颈在 prores_ks 编码线程,不在解码 worker),且每个深度槽位 +130-160MB RSS;
// 唯二看似「d3 更快」的单次跑与受控重复方向相反,判为负载噪声,不满足 ≥10% 门槛。
// 结论:不联动,默认保持 2;spec>0 仍强制优先(语义 0=自动不变)。
// 实测明细与真机复测协议见 docs/INTERFACES.md §10.7.2。
struct DngParallel {
    int nthreads = 1;
    int depth = 2;
};

DngParallel dng_parallel_params(int spec_threads, int spec_depth, int w, int h)
{
    DngParallel p;

    // 可用核数:Android 上优先 sched_getaffinity(它反映 cpuset/大核限制),
    // 失败再退回 sysconf,最后退回 4。
    int cores = 0;
#if defined(__ANDROID__)
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        cores = (int)CPU_COUNT(&set);
    }
#endif
    if (cores <= 0) {
        long sc = sysconf(_SC_NPROCESSORS_ONLN);
        cores = (sc > 0) ? (int)sc : 4;
    }
    if (cores < 1) cores = 1;

    // 单帧峰值预算(bytes)
    const double rgb_bytes = (double)(w > 0 ? w : 4096) * (double)(h > 0 ? h : 3072) *
                             3.0 * 2.0;
    const double peak_per_frame = rgb_bytes * 2.95 + 16.0 * 1048576.0;
    // 1GB:4096x3072 实测 4 线程 VmHWM 才 ~500MB(bionic/scudo 的 malloc 复用率比 glibc
    // 还高),留一倍余量给编码器/IO buffer。
    const double kBudget = 1024.0 * 1048576.0;  // 整个解码池的 native 峰值预算

    int by_mem = (int)(kBudget / (peak_per_frame > 1.0 ? peak_per_frame : 1.0));
    if (by_mem < 1) by_mem = 1;
    if (by_mem > 4) by_mem = 4;
    if (by_mem > cores - 1) by_mem = cores - 1;   // 给编码留一个核
    if (by_mem < 1) by_mem = 1;                   // 单核设备:仍然正确,只是没并行

    p.nthreads = (spec_threads > 0) ? spec_threads : by_mem;
    if (p.nthreads > cores) p.nthreads = cores;
    if (p.nthreads < 1) p.nthreads = 1;
    if (p.nthreads > 16) p.nthreads = 16;

    p.depth = (spec_depth > 0) ? spec_depth : 2;
    if (p.depth < 2) p.depth = 2;      // 深度 1 没有重叠,等于串行
    if (p.depth > 8) p.depth = 8;
    return p;
}

// ---------------------------------------------------------------------------
// AVFrame 小池(P1-3 第一步,2026-10-06,PRF_AVFRAME_POOL)
// ---------------------------------------------------------------------------
// 动机:zip 路径每帧都 av_frame_get_buffer(dframe, 32)。4096x3072 rgb48le 的
// buf[0] 是 75,497,600 B(= 4*32 对齐 padding + linesize[0] 24576 * FFALIGN(h,32)
// 3072,见 av_image_get_buffer_size),av_buffer_alloc 内部清零,即每帧一次 72MB
// 的 mmap + memset(free 时 munmap)。N 帧序列就是 N 次这样的抖动,且换进来的
// 新页在 sws/缩放链里还要再被 fault 读一次。
// 池化后壳和 buffer 都跨帧复用,memcpy 目标页常驻,分配次数只剩首帧 + 回退。
//
// 所有权与释放时机(buffersrc.h 的文档 + n6.1 libavfilter/buffersrc.c
// av_buffersrc_add_frame_flags 实测):
//   * flags=0 且帧有引用计数: buffersrc 自己 av_frame_alloc 一个壳,
//     av_frame_move_ref(copy, frame) 把我们的 AVBufferRef 整体搬进滤镜图,
//     再 get_frame_defaults(frame) 把我们的壳重置(data/buf/linesize 全 NULL)。
//     所以 flags=0 之后 av_frame_unref(dframe) 是 no-op(move 已拿走引用),
//     留着只是错误路径兜底(帧不被触碰);也所以 flags=0 下 buffer 归滤镜图,
//     壳还能复用,72MB 的分配省不掉。
//   * flags=AV_BUFFERSRC_FLAG_KEEP_REF(8): buffersrc 走 av_frame_clone →
//     av_frame_ref 新建一份引用(计数 +1),滤镜图吃 clone,我们的壳继续持有
//     buffer —— 这才有池化的意义。
// 安全复用条件:buffer 引用计数回到 1(= 只有池持有)时才能覆盖写。推帧后计数
// 是 2(池 + 滤镜图的 clone);drain_graph 逐帧把快推到 sink,滤镜链消费完后
// 释放 clone 回到 1。N ≥ depth+2 给「某帧在链里多待一会儿」留余量;万一环回
// 时计数仍 >1,该壳退化为 av_frame_unref + av_frame_get_buffer 重新分配
// (正确回退,绝不去写滤镜图还在引用的 buffer)。
// 注意 av_frame_get_buffer 不检查壳上已有 buffer(直接 get_video_buffer 覆盖),
// 所以复用前必须由本结构先判引用计数,不能盲目接着写。
// 「第二步 worker 直写目标 buffer 的零拷贝」不在此列(生命周期风险最高,等真机
// soak,见 INTERFACES.md §10.7)。
//
// 发布默认值 = 0(P1-02,2026-10-06):host A/B 实测池化墙钟慢 1.4–1.7 倍、
// RSS 增约 220MB,而 arm64 真机证据缺失。开启(=1)的真机条件:相同设备与负载
// 下比 nopool 有 ≥10% 吞吐提升且 VmHWM 不增,interleaved 4 轮方向一致 ——
// 当前未验证,因此不作为发布默认。A/B 实验构建用 -DPRF_AVFRAME_POOL=1。
// #if/#else 两条实现路径都必须保持可编译(见下方使用点)。
#ifndef PRF_AVFRAME_POOL
#define PRF_AVFRAME_POOL 0
#endif

struct AvFramePool {
    std::vector<AVFrame *> fr;
    size_t next = 0;
    int64_t hits = 0;     // 原地复用(省掉 72MB 分配清零)
    int64_t misses = 0;   // 回退:重新分配(图谱还持引用 / 尺寸变化)

    // RAII:取消/失败路径是 goto 跳出作用域,靠析构兜底释放(否则每个壳连它
    // 72MB 的 buffer 一起泄漏,而转码进程取消后进程还活着,泄漏会累加)。
    ~AvFramePool() { stop(); }

    // count 个壳,壳上的 buffer 首帧才分配
    bool start(int count)
    {
        stop();
        if (count < 1) count = 1;
        fr.assign((size_t)count, nullptr);
        for (auto &f : fr) {
            f = av_frame_alloc();
            if (!f) {
                stop();
                return false;
            }
        }
        return true;
    }

    void stop()
    {
        for (auto &f : fr) av_frame_free(&f);
        fr.clear();
        next = 0;
    }

    size_t size() const { return fr.size(); }

    // 取一个壳并填好本帧参数。能原地复用就复用(count==1),否则重新分配。
    // 成功时 *out 的 data[0]/linesize[0] 已有效,调用方直接逐行 memcpy。
    bool acquire(int w, int h, int64_t pts, AVFrame **out)
    {
        if (fr.empty() || next >= fr.size()) return false;   // 未 start / 已 stop
        AVFrame *f = fr[next];
        next = (next + 1) % fr.size();
        if (f->buf[0] && f->width == w && f->height == h &&
            f->format == AV_PIX_FMT_RGB48LE &&
            av_buffer_get_ref_count(f->buf[0]) == 1) {
            f->pts = pts;   // data/linesize 沿用一个字节都不动
            hits++;
            *out = f;
            return true;
        }
        av_frame_unref(f);
        f->format = AV_PIX_FMT_RGB48LE;
        f->width = w;
        f->height = h;
        f->pts = pts;
        if (av_frame_get_buffer(f, 32) < 0) {
            *out = nullptr;
            return false;
        }
        misses++;
        *out = f;
        return true;
    }
};

// 池大小:N = depth+2(≥2)。N 不是正确性条件(引用计数判据兜底),只是余量 ——
// 给「fps 滤镜等会内部扣帧的链」多一点环回空间。上限按同一预算口径封顶:
// 池的常驻 = N × rgb 字节数(4096x3072 ≈ 72MB/槽),取 kBudget 的 1/4 封顶,
// 4096x3072 下 N≤3;1080p(12.4MB/槽)不受限。
int avframe_pool_count(int depth, int w, int h)
{
    const double rgb_bytes = (double)(w > 0 ? w : 4096) * (double)(h > 0 ? h : 3072) *
                             3.0 * 2.0;
    const double kBudget = 1024.0 * 1048576.0;   // 与 dng_parallel_params 同口径
    const double cap = kBudget * 0.25 / (rgb_bytes > 1.0 ? rgb_bytes : 1.0);
    int n = depth + 2;
    if (n < 2) n = 2;
    if ((double)n > cap) n = (int)cap;
    if (n < 1) n = 1;
    return n;
}

// ---------------------------------------------------------------------------
// fd IO 回调(pread/pwrite + 自维护偏移)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 大文件偏移:本文件 IO 一律 pread64/pwrite64/lseek64(详见 dngzip.cpp 顶部注释)。
// _FILE_OFFSET_BITS=64 不能全文件定义:它会经 FFmpeg 头改变
// ff_AMediaCodec_queueInputBuffer 的 off_t 形参 ABI,并与 libraw.a(以 NDK 默认
// 32 位 off_t 编译)不一致。
// 大文件偏移策略(P2-01,2026-10-11):
//   - 内部(FdIoCtx.pos/size、AVIO 回调形参)统一用 int64_t(与 AVIO 回调一致);
//   - pread64/pwrite64/lseek64 参数处显式转成 off64_t(POSIX 形参要求);
//   - Android LP64: off64_t ≡ int64_t ≡ long(无损);
//   - Android LP32: off64_t ≡ int64_t ≡ long long(无损,host harness 已验证 >4GB)。
// ---------------------------------------------------------------------------
int fd_io_read(void *opaque, uint8_t *buf, int buf_size)
{
    FdIoCtx *c = static_cast<FdIoCtx *>(opaque);
    if (buf_size <= 0) return AVERROR(EINVAL);
    int64_t total = 0;
    while (total < buf_size) {
        ssize_t n = pread64(c->fd, buf + total, (size_t)(buf_size - total),
                            (off64_t)(c->pos + total));
        if (n < 0) {
            if (errno == EINTR) continue;
            return AVERROR(errno);
        }
        if (n == 0) break; // 到文件尾
        total += n;
    }
    c->pos += total;
    return total > 0 ? (int)total : AVERROR_EOF;
}

int fd_io_write(void *opaque, uint8_t *buf, int buf_size)
{
    FdIoCtx *c = static_cast<FdIoCtx *>(opaque);
    if (buf_size <= 0) return AVERROR(EINVAL);
    int64_t total = 0;
    while (total < buf_size) {
        ssize_t n = pwrite64(c->fd, buf + total, (size_t)(buf_size - total),
                             (off64_t)(c->pos + total));
        if (n < 0) {
            if (errno == EINTR) continue;
            return AVERROR(errno);
        }
        // A4:pwrite64 返回 0 表示毫无进展,继续循环只会原地打转(阻塞整条管线),break。
        if (n == 0) break;
        total += n;
    }
    c->pos += total;
    if (c->pos > c->size) c->size = c->pos;
    return (int)total;
}

int64_t fd_io_seek(void *opaque, int64_t offset, int whence)
{
    FdIoCtx *c = static_cast<FdIoCtx *>(opaque);
    // 兼容旧调用方:掩掉 FORCE 位(avio_seek 自身也会掩)
    if (whence & AVSEEK_FORCE) whence &= ~AVSEEK_FORCE;
    int64_t base = 0;
    switch (whence) {
        case AVSEEK_SIZE:
            // 输入返回文件长度,输出返回已写字节数
            return c->size;
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = c->pos;
            break;
        case SEEK_END:
            base = c->size;
            break;
        default:
            return AVERROR(EINVAL);
    }
    int64_t np = base + offset;
    if (np < 0) return AVERROR(EINVAL);
    c->pos = np;
    return np; // 成功必须返回新偏移
}

AVIOContext *fd_io_new_avio(FdIoCtx *ctx, int buffer_size, bool for_write, uint8_t **out_buf)
{
    *out_buf = nullptr;
    if (buffer_size <= 0) buffer_size = 0x10000;
    uint8_t *buf = (uint8_t *)av_malloc((size_t)buffer_size);
    if (!buf) return nullptr;
    AVIOContext *pb = avio_alloc_context(buf, buffer_size, for_write ? 1 : 0, ctx,
                                         for_write ? nullptr : fd_io_read,
                                         for_write ? fd_io_write : nullptr,
                                         fd_io_seek);
    if (!pb) {
        av_freep(&buf);
        return nullptr;
    }
    if (out_buf) *out_buf = buf;
    return pb;
}

void fd_io_free_avio(AVIOContext **pb, uint8_t **buf)
{
    // 释放语义(关键!):avio_alloc_context 传入的 buffer "可能被 libavformat 换掉"——
    // ffio_set_buf_size(探测/ffio_ensure_seekback)会 av_free 旧 buffer 再装新 buffer,
    // 所以唯一正确的释放对象是 pb->buffer(当前 buffer);调用方留存的原始指针此刻要么
    // 已被释放、要么恰好还等于 pb->buffer,单独 free 都会双free/堆损坏。
    if (pb && *pb) {
        av_freep(&(*pb)->buffer);
        avio_context_free(pb);
        if (buf && *buf) *buf = nullptr;
    } else if (buf && *buf) {
        // pb 没建起来:buffer 从未被托管,还是调用方自己的
        av_freep(buf);
    }
}

int fd_io_open_input(int fd, int64_t length,
                     AVFormatContext **out_ic, FdIoCtx **out_ctx,
                     AVIOContext **out_pb, uint8_t **out_buf)
{
    *out_ic = nullptr;
    *out_ctx = nullptr;
    *out_pb = nullptr;
    *out_buf = nullptr;

    FdIoCtx *ctx = new FdIoCtx();
    ctx->fd = fd;
    ctx->pos = 0;
    if (length > 0) {
        ctx->size = length;
    } else {
        // Kotlin 没给长度:自己探一次(不影响后续,偏移已回到 0)
        // lseek64:off_t 在 LP32 上 32 位,>2GB 的输入长度会截断
        off64_t end = lseek64(fd, 0, SEEK_END);
        ctx->size = end > 0 ? (int64_t)end : 0;
        lseek64(fd, 0, SEEK_SET);
    }

    AVIOContext *pb = fd_io_new_avio(ctx, 0x10000, false, out_buf);
    if (!pb) {
        delete ctx;
        return AVERROR(ENOMEM);
    }

    AVFormatContext *ic = avformat_alloc_context();
    if (!ic) {
        fd_io_free_avio(&pb, out_buf);
        delete ctx;
        return AVERROR(ENOMEM);
    }

    ic->pb = pb;
    ic->flags |= AVFMT_FLAG_CUSTOM_IO;

    // url 传 "",format 传 nullptr,让 avformat 自己探测容器(ProRes .mov/.mxf)
    int ret = avformat_open_input(&ic, "", nullptr, nullptr);
    if (ret < 0) {
        // 失败时 ic 已被 open_input 内部 avformat_free_context 并置 NULL,
        // 自定义 pb 不会被关闭,这里释放。
        fd_io_free_avio(&pb, out_buf);
        delete ctx;
        return ret;
    }

    *out_ic = ic;
    *out_ctx = ctx;
    *out_pb = pb;
    return 0;
}

void fd_io_free_input(AVFormatContext **ic, FdIoCtx **ctx, AVIOContext **pb, uint8_t **buf)
{
    if (ic && *ic) avformat_close_input(ic); // 自定义 pb 不会被它关闭
    fd_io_free_avio(pb, buf);
    if (ctx && *ctx) {
        delete *ctx;
        *ctx = nullptr;
    }
}

int mem_io_read(void *opaque, uint8_t *buf, int buf_size)
{
    MemIoCtx *c = static_cast<MemIoCtx *>(opaque);
    if (!c || !buf || buf_size <= 0) return AVERROR(EINVAL);
    if (c->pos >= c->size) return AVERROR_EOF;
    const int64_t left = c->size - c->pos;
    const int n = (int)std::min<int64_t>(left, buf_size);
    std::memcpy(buf, c->data + c->pos, (size_t)n);
    c->pos += n;
    return n;
}

int64_t mem_io_seek(void *opaque, int64_t offset, int whence)
{
    MemIoCtx *c = static_cast<MemIoCtx *>(opaque);
    if (!c) return AVERROR(EINVAL);
    if (whence & AVSEEK_FORCE) whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return c->size;
    int64_t base = 0;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = c->pos; break;
        case SEEK_END: base = c->size; break;
        default: return AVERROR(EINVAL);
    }
    if (offset > 0 && base > std::numeric_limits<int64_t>::max() - offset)
        return AVERROR(EINVAL);
    if (offset < 0 && base < -offset) return AVERROR(EINVAL);
    const int64_t next = base + offset;
    if (next < 0 || next > c->size) return AVERROR(EINVAL);
    c->pos = next;
    return next;
}

AVIOContext *mem_io_new_avio(MemIoCtx *ctx, int buffer_size, uint8_t **out_buf)
{
    if (out_buf) *out_buf = nullptr;
    if (!ctx) return nullptr;
    if (buffer_size <= 0) buffer_size = 0x10000;
    uint8_t *buf = (uint8_t *)av_malloc((size_t)buffer_size);
    if (!buf) return nullptr;
    AVIOContext *pb = avio_alloc_context(buf, buffer_size, 0, ctx,
                                         mem_io_read, nullptr, mem_io_seek);
    if (!pb) {
        av_free(buf);
        return nullptr;
    }
    if (out_buf) *out_buf = buf;
    return pb;
}

int mem_io_open_input(const uint8_t *data, int64_t length,
                      AVFormatContext **out_ic, MemIoCtx **out_ctx,
                      AVIOContext **out_pb, uint8_t **out_buf)
{
    if (!out_ic || !out_ctx || !out_pb || !out_buf || !data || length <= 0)
        return AVERROR(EINVAL);
    *out_ic = nullptr;
    *out_ctx = nullptr;
    *out_pb = nullptr;
    *out_buf = nullptr;

    MemIoCtx *ctx = new (std::nothrow) MemIoCtx();
    if (!ctx) return AVERROR(ENOMEM);
    ctx->data = data;
    ctx->size = length;
    ctx->pos = 0;
    AVIOContext *pb = mem_io_new_avio(ctx, 0x10000, out_buf);
    if (!pb) {
        delete ctx;
        return AVERROR(ENOMEM);
    }
    AVFormatContext *ic = avformat_alloc_context();
    if (!ic) {
        fd_io_free_avio(&pb, out_buf);
        delete ctx;
        return AVERROR(ENOMEM);
    }
    ic->pb = pb;
    ic->flags |= AVFMT_FLAG_CUSTOM_IO;
    int ret = avformat_open_input(&ic, "", nullptr, nullptr);
    if (ret < 0) {
        fd_io_free_avio(&pb, out_buf);
        delete ctx;
        return ret;
    }
    *out_ic = ic;
    *out_ctx = ctx;
    *out_pb = pb;
    return 0;
}

void mem_io_free_input(AVFormatContext **ic, MemIoCtx **ctx,
                       AVIOContext **pb, uint8_t **buf)
{
    if (ic && *ic) avformat_close_input(ic);
    fd_io_free_avio(pb, buf);
    if (ctx && *ctx) {
        delete *ctx;
        *ctx = nullptr;
    }
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
const char *err_str(int errnum)
{
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}

enum AVPixelFormat parse_pix_fmt(const std::string &s)
{
    if (s.empty()) return AV_PIX_FMT_NONE;
    return av_get_pix_fmt(s.c_str());
}

int detect_rotation_deg(AVStream *st, const std::string &rotate_spec)
{
    // 显式值优先(auto 才识别 display matrix side data)
    if (!rotate_spec.empty() && rotate_spec != "auto") {
        long v = std::strtol(rotate_spec.c_str(), nullptr, 10);
        if (v < 0) v = -v;
        v = v % 360;
        switch (v) {
            case 90: return 90;
            case 180: return 180;
            case 270: return 270;
            default: return 0;
        }
    }
    if (!st) return 0;
    const int32_t *matrix =
        (const int32_t *)av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, nullptr);
    if (!matrix && st->codecpar) {
        // 探测路径不跑 avformat_find_stream_info,_mov demuxer 把矩阵挂在
        // codecpar->coded_side_data(只有 find_stream_info 才迁到 stream side data),
        // 这里兜底读一遍,否则摄像头竖拍文件探测会报 rot=0。
        const AVPacketSideData *sd = av_packet_side_data_get(
            st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
            AV_PKT_DATA_DISPLAYMATRIX);
        if (sd && sd->size >= 9 * sizeof(int32_t))
            matrix = (const int32_t *)sd->data;
    }
    if (!matrix) return 0;
    // 与 ffmpeg CLI autorotate 同一套算法(display matrix → 顺时针角度)
    double theta = -av_display_rotation_get(matrix);
    theta -= 360.0 * std::floor(theta / 360.0 + 0.9 / 360.0);
    if (std::fabs(theta - 90.0) < 0.01) return 90;
    if (std::fabs(theta - 180.0) < 0.01) return 180;
    if (std::fabs(theta - 270.0) < 0.01) return 270;
    return 0;
}

// ---------------------------------------------------------------------------
// 以下为文件内部实现
// ---------------------------------------------------------------------------
namespace {

int round_down_even(int v) { return v & ~1; }

int pix_depth(enum AVPixelFormat f)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(f);
    return d ? d->comp[0].depth : 8;
}

// filtergraph 选项值转义(':' ',' ';' '\'' '\\')
std::string esc_opt(const std::string &s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == ':' || c == ',' || c == ';' || c == '\'' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

// 进度上报:frames 由调用方传入(Pipe::frames),这里不持有计数状态
struct Reporter {
    const TranscodeParams *p = nullptr;
    int64_t total = -1;

    void operator()(int pct, const char *stage, int64_t frames) const
    {
        if (p && p->on_progress) p->on_progress(pct, stage, frames, total, p->user);
    }
};

uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | (uint64_t)p[i];
    return v;
}

struct TopBox {
    int64_t off;
    int64_t size;
    char type[5];
};

struct MoovOffsetCheck {
    bool have_first = false;
    size_t relative_offset = 0;
    size_t width = 0;
    uint64_t expected = 0;
};

static bool local_box(const uint8_t *data, size_t limit, size_t off,
                      size_t *size, size_t *header)
{
    if (!data || !size || !header || off > limit || limit - off < 8) return false;
    const uint32_t s32 = be32(data + off);
    uint64_t raw_size = s32;
    size_t hdr = 8;
    if (s32 == 1) {
        if (limit - off < 16) return false;
        raw_size = be64(data + off + 8);
        hdr = 16;
    } else if (s32 == 0) {
        raw_size = (uint64_t)(limit - off);
    }
    if (raw_size < hdr || raw_size > (uint64_t)(limit - off) ||
        raw_size > (uint64_t)std::numeric_limits<size_t>::max()) return false;
    *size = (size_t)raw_size;
    *header = hdr;
    return true;
}

static bool box_type_is(const uint8_t *type, const char *name)
{
    return type && name && std::memcmp(type, name, 4) == 0;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static bool is_moov_container(const uint8_t *type)
{
    return box_type_is(type, "moov") || box_type_is(type, "trak") ||
           box_type_is(type, "mdia") || box_type_is(type, "minf") ||
           box_type_is(type, "stbl");
}

static bool rewrite_chunk_offsets(uint8_t *data, size_t off, size_t size, size_t header,
                                  uint64_t delta, MoovOffsetCheck *check,
                                  const char **why)
{
    const size_t width = box_type_is(data + off + 4, "stco") ? 4 : 8;
    if (size < header + 8) {
        if (why) *why = "chunk-offset-header";
        return false;
    }
    const size_t payload_size = size - header;
    const uint32_t count = be32(data + off + header + 4);
    if (count > (payload_size - 8) / width) {
        if (why) *why = "chunk-offset-count";
        return false;
    }
    uint8_t *entry = data + off + header + 8;
    for (uint32_t i = 0; i < count; i++, entry += width) {
        const uint64_t old_value = width == 4 ? (uint64_t)be32(entry) : be64(entry);
        const uint64_t max_value = width == 4
                                       ? (uint64_t)std::numeric_limits<uint32_t>::max()
                                       : std::numeric_limits<uint64_t>::max();
        if (delta > max_value || old_value > max_value - delta) {
            if (why) *why = width == 4 ? "stco-overflow" : "co64-overflow";
            return false;
        }
        const uint64_t value = old_value + delta;
        if (width == 4) put_be32(entry, (uint32_t)value);
        else put_be64(entry, value);
        if (check && !check->have_first) {
            check->have_first = true;
            check->relative_offset = (size_t)(entry - data);
            check->width = width;
            check->expected = value;
        }
    }
    return true;
}

static bool rewrite_moov_children(uint8_t *data, size_t begin, size_t end,
                                  uint64_t delta, MoovOffsetCheck *check,
                                  const char **why)
{
    size_t off = begin;
    while (off < end) {
        size_t size = 0, header = 0;
        if (!local_box(data, end, off, &size, &header)) {
            if (why) *why = "moov-child-structure";
            return false;
        }
        const uint8_t *type = data + off + 4;
        if (box_type_is(type, "stco") || box_type_is(type, "co64")) {
            if (!rewrite_chunk_offsets(data, off, size, header, delta, check, why))
                return false;
        } else if (is_moov_container(type)) {
            const size_t child_begin = off + header;
            if (!rewrite_moov_children(data, child_begin, off + size, delta, check, why))
                return false;
        }
        off += size;
    }
    return off == end;
}

static bool rewrite_moov_chunk_offsets(std::vector<uint8_t> &data, int64_t delta,
                                       MoovOffsetCheck *check, const char **why)
{
    if (delta <= 0 || data.size() < 8) {
        if (why) *why = "moov-rewrite-arg";
        return false;
    }
    size_t root_size = 0, root_header = 0;
    if (!local_box(data.data(), data.size(), 0, &root_size, &root_header) ||
        root_size != data.size() || !box_type_is(data.data() + 4, "moov")) {
        if (why) *why = "moov-root-structure";
        return false;
    }
    return rewrite_moov_children(data.data(), root_header, root_size,
                                 (uint64_t)delta, check, why);
}

// ---------------------------------------------------------------------------
// faststart:把顶层 moov 挪到 ftyp 之后(box 边界分块搬移)。
// 内存只占 moov 本体 + 一个 4MB 块,任意大小文件都适用;失败/异常一律返回 false,
// 调用方保持原文件即可。
// ---------------------------------------------------------------------------
bool pread_exact(int fd, void *buf, size_t n, int64_t off)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        // pread64:见文件头「大文件偏移」注释
        ssize_t r = pread64(fd, p + got, n - got, (off64_t)(off + (int64_t)got));
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        got += (size_t)r;
    }
    return true;
}

bool pwrite_exact(int fd, const void *buf, size_t n, int64_t off)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t done = 0;
    while (done < n) {
        // pwrite64:见文件头「大文件偏移」注释
        ssize_t r = pwrite64(fd, p + done, n - done, (off64_t)(off + (int64_t)done));
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += (size_t)r;
    }
    return true;
}

// 扫描 fd 上 [0, size) 的顶层 box 序列(逐个 pread 8/16 字节头,不整读文件)。
// 返回:0 = 完整铺满整个文件;1 = IO 失败(pre/pwrite 返回 false);2 = 结构不可信
// (box 头截断、大小越界、或顶层 box 没有精确铺满整个文件)。
// 调用方按返回值区分「IO 错」与「结构错」,不要把陈旧 errno 当失败原因。
static int scan_top_level(int fd, int64_t size, std::vector<TopBox> &boxes)
{
    boxes.clear();
    int64_t i = 0;
    while (i + 8 <= size) {
        uint8_t h[16];
        if (!pread_exact(fd, h, 8, i)) return 1;
        const uint32_t s32 = be32(h);
        int64_t sz;
        int64_t hdr = 8;
        if (s32 == 1) {
            if (i + 16 > size) return 2;
            if (!pread_exact(fd, h, 16, i)) return 1;
            sz = (int64_t)be64(h + 8);
            hdr = 16;
        } else if (s32 == 0) {
            sz = size - i; // 直到文件尾
        } else {
            sz = (int64_t)s32;
        }
        if (sz < hdr || i + sz > size) return 2; // 截断/损坏
        TopBox b;
        b.off = i;
        b.size = sz;
        std::memcpy(b.type, h + 4, 4);
        b.type[4] = 0;
        boxes.push_back(b);
        i += sz;
    }
    if (i != size) return 2; // 顶层结构不完整,放弃
    return 0;
}

// 输出 fd 的当前文件长度(真实字节数)。faststart 调用点处 out_pb 已释放,
// avio_size/avio_tell 不再可用,这里直接问 fd。只查询、不写文件;
// 失败(含 fd 无效)返回 -1。
static int64_t out_fd_len(int fd)
{
    if (fd < 0) return -1;
    off64_t e = lseek64(fd, 0, SEEK_END);
    return (e < 0) ? (int64_t)-1 : (int64_t)e;
}

// faststart 失败点(P1-03 可观测性):point = 阶段内的具体步骤;
// err != 0 = 相关 errno(IO 类失败点),0 = 不适用(参数/结构/自检类)。
struct FaststartFail {
    const char *point = "unknown";
    int err = 0;
};

// 置失败点并返回 false。IO 类失败点(刚刚 pread/pwrite 失败,errno 必定是新的)
// 用 PRF_FS_FAIL;参数/结构/自检类失败点 errno 不适用,用 PRF_FS_FAIL_NOERR,
// 避免把陈旧 errno 当成失败原因上报。
#define PRF_FS_FAIL(msg)       do { if (fail) { fail->point = (msg); fail->err = errno; } return false; } while (0)
#define PRF_FS_FAIL_NOERR(msg) do { if (fail) { fail->point = (msg); fail->err = 0; } return false; } while (0)

bool faststart_move_moov(FdIoCtx *oc, FaststartFail *fail = nullptr)
{
    if (!oc || oc->fd < 0) PRF_FS_FAIL_NOERR("bad-arg");
    const int64_t size = oc->size;
    if (size <= 0) PRF_FS_FAIL_NOERR("bad-size");

    // 扫描顶层 box(逐个 pread 头部,不整读文件)
    std::vector<TopBox> boxes;
    const int sr = scan_top_level(oc->fd, size, boxes);
    if (sr == 1) PRF_FS_FAIL("scan-io");
    if (sr == 2) PRF_FS_FAIL_NOERR("scan-structure");
    if (boxes.size() < 2) PRF_FS_FAIL_NOERR("too-few-boxes");

    int moov = -1, ftyp = -1;
    for (size_t k = 0; k < boxes.size(); k++) {
        if (moov < 0 && std::strcmp(boxes[k].type, "moov") == 0) moov = (int)k;
        if (ftyp < 0 && std::strcmp(boxes[k].type, "ftyp") == 0) ftyp = (int)k;
    }
    if (moov < 0) PRF_FS_FAIL_NOERR("no-moov");            // 没有 moov,保持原样
    if (moov == 0) return true;                            // 已在文件头
    if (ftyp >= 0 && moov == ftyp + 1) return true;        // 已紧跟 ftyp

    // moov 是元数据(通常几百 KB)整个读进内存;异常大时放弃(best effort)
    if (boxes[moov].size > ((int64_t)256 << 20)) PRF_FS_FAIL_NOERR("moov-too-large");
    std::vector<uint8_t> moov_data((size_t)boxes[moov].size);
    if (!pread_exact(oc->fd, moov_data.data(), moov_data.size(), boxes[moov].off))
        PRF_FS_FAIL("read-moov");

    // 布局 [ftyp][A][moov][B] → [ftyp][moov][A][B]:
    // A 整体右移 moov 大小,moov 落到 a_begin,B 不动。
    // 从尾部往前按块搬:每个块的目的地址都在未搬源数据之后(最远落入旧 moov 区,
    // 已缓冲),不会自我覆盖;A 右移后的终点恰好是旧 moov 末尾,永不越入 B。
    const int64_t a_begin = (ftyp >= 0) ? boxes[ftyp].off + boxes[ftyp].size : 0;
    const int64_t a_end = boxes[moov].off;
    const int64_t delta = boxes[moov].size;
    if (a_end < a_begin) PRF_FS_FAIL_NOERR("bad-layout");

    // Chunk offsets are absolute file positions. Rewrite the in-memory moov
    // before shifting A; otherwise every relocated media chunk points at stale data.
    MoovOffsetCheck moov_check;
    const char *rewrite_why = nullptr;
    if (!rewrite_moov_chunk_offsets(moov_data, delta, &moov_check, &rewrite_why))
        PRF_FS_FAIL_NOERR(rewrite_why ? rewrite_why : "rewrite-moov");

    const int64_t kChunk = 4 << 20;
    const int64_t chunk_sz = (kChunk < delta) ? kChunk : delta;
    std::vector<uint8_t> chunk((size_t)chunk_sz);
    int64_t pos = a_end;
    while (pos > a_begin) {
        int64_t n = (pos - a_begin > chunk_sz) ? chunk_sz : (pos - a_begin);
        pos -= n;
        if (!pread_exact(oc->fd, chunk.data(), (size_t)n, pos))
            PRF_FS_FAIL("shift-A-read");
        if (!pwrite_exact(oc->fd, chunk.data(), (size_t)n, pos + delta))
            PRF_FS_FAIL("shift-A-write");
    }
    if (!pwrite_exact(oc->fd, moov_data.data(), moov_data.size(), a_begin))
        PRF_FS_FAIL("write-moov");

    // Verify a rewritten chunk offset from the bytes actually stored on disk.
    if (moov_check.have_first) {
        uint8_t actual_bytes[8] = {};
        const size_t n = moov_check.width;
        if (!pread_exact(oc->fd, actual_bytes, n,
                         a_begin + (int64_t)moov_check.relative_offset))
            PRF_FS_FAIL("verify-chunk-offset-io");
        const uint64_t actual = n == 4 ? (uint64_t)be32(actual_bytes) : be64(actual_bytes);
        if (actual != moov_check.expected)
            PRF_FS_FAIL_NOERR("verify-chunk-offset");
    }

    // ---- 搬移后结构自检(P1-03)----
    // 只相信重新读回来的字节:pwrite 返回成功不等于设备上的字节顺序正确,而
    // 半搬移文件与成品的区别只有顶层 box 顺序。重新扫描输出并核对
    // [ftyp][moov](无 ftyp 时为 [moov])、moov 偏移/尺寸与总长度不变,
    // 任一不符即判失败 —— 让成功路径调用方把这种文件按失败任务收尾,
    // 而不是当成「faststart 已完成」的成品。
    std::vector<TopBox> nb;
    const int vr = scan_top_level(oc->fd, size, nb);
    if (vr == 1) PRF_FS_FAIL("verify-rescan-io");
    if (vr == 2) PRF_FS_FAIL_NOERR("verify-rescan-structure");
    const size_t want_moov = (ftyp >= 0) ? 1 : 0;   // 期望 moov 落在第几个 box
    if (nb.size() <= want_moov) PRF_FS_FAIL_NOERR("verify-box-count");
    if (ftyp >= 0 && std::strcmp(nb[0].type, "ftyp") != 0)
        PRF_FS_FAIL_NOERR("verify-box0-not-ftyp");
    if (std::strcmp(nb[want_moov].type, "moov") != 0)
        PRF_FS_FAIL_NOERR("verify-moov-not-first");
    if (nb[want_moov].off != a_begin) PRF_FS_FAIL_NOERR("verify-moov-offset");
    if (nb[want_moov].size != boxes[moov].size) PRF_FS_FAIL_NOERR("verify-moov-size");

    oc->pos = 0;
    return true;
}

#undef PRF_FS_FAIL
#undef PRF_FS_FAIL_NOERR

// ---------------------------------------------------------------------------
// 编码器选择(契约 §3.2 fallback chain)
// ---------------------------------------------------------------------------
struct Family {
    const char *name;      // 契约 vcodec 值
    const char *chain[4];  // avcodec_find_encoder_by_name 顺序,nullptr 结尾
    const char *container; // 输出容器名
};

const Family kFamilies[] = {
    {"h264", {"h264_mediacodec", "libx264", nullptr}, "mp4"},
    {"hevc", {"hevc_mediacodec", "libx265", nullptr}, "mp4"},
    {"vp9", {"libvpx-vp9", nullptr}, "webm"},
    {"prores", {"prores_ks", nullptr}, "mov"},
};

std::string lower(const std::string &s)
{
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) o += (char)tolower((unsigned char)s[i]);
    return o;
}

// 契约 vcodec 常见写法归一到上面四个家族;nullptr = 未知
const Family *family_of(const std::string &vcodec)
{
    std::string v = lower(vcodec);
    if (v == "h264" || v == "h.264" || v == "avc" || v == "libx264" || v == "x264" ||
        v == "h264_mediacodec")
        return &kFamilies[0];
    if (v == "hevc" || v == "h.265" || v == "h265" || v == "libx265" || v == "x265" ||
        v == "hevc_mediacodec")
        return &kFamilies[1];
    if (v == "vp9" || v == "libvpx-vp9" || v == "libvpx") return &kFamilies[2];
    if (v == "prores" || v == "prores_ks" || v == "proresks") return &kFamilies[3];
    return nullptr;
}

// 用户显式写了某个编码器名时,把它排到链首(尊重显式选择)
std::vector<std::string> build_chain(const Family *f, const std::string &vcodec)
{
    std::vector<std::string> chain;
    std::string v = lower(vcodec);
    for (int i = 0; f->chain[i]; i++) {
        std::string n(f->chain[i]);
        if (n == v && (chain.empty() || chain[0] != v)) chain.insert(chain.begin(), v);
        else chain.push_back(n);
    }
    if (chain.empty()) chain.push_back(f->chain[0]);
    return chain;
}

bool codec_supports(const AVCodec *c, enum AVPixelFormat f)
{
    if (!c->pix_fmts) return true; // 未声明则信任
    for (const enum AVPixelFormat *p = c->pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == f) return true;
    }
    return false;
}

enum AVPixelFormat codec_best_fmt(const AVCodec *c)
{
    static const enum AVPixelFormat pri[] = {
        AV_PIX_FMT_YUV420P,     AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_NV12,
        AV_PIX_FMT_YUVJ420P,
    };
    if (!c->pix_fmts) return AV_PIX_FMT_YUV420P;
    for (size_t i = 0; i < sizeof(pri) / sizeof(pri[0]); i++) {
        if (codec_supports(c, pri[i])) return pri[i];
    }
    return AV_PIX_FMT_NONE;
}

// 依 fallback chain 找编码器,实际像素格式写回 *fmt
const AVCodec *pick_video_encoder(const std::vector<std::string> &chain,
                                  enum AVPixelFormat want, enum AVPixelFormat *fmt)
{
    *fmt = AV_PIX_FMT_NONE;
    // 第一轮:优先满足请求的 pixfmt
    for (size_t i = 0; i < chain.size(); i++) {
        const AVCodec *c = avcodec_find_encoder_by_name(chain[i].c_str());
        if (!c) continue;
        if (want != AV_PIX_FMT_NONE && codec_supports(c, want)) {
            *fmt = want;
            return c;
        }
    }
    // 第二轮:允许降到该编码器支持的格式(保证出片)
    for (size_t i = 0; i < chain.size(); i++) {
        const AVCodec *c = avcodec_find_encoder_by_name(chain[i].c_str());
        if (!c) continue;
        enum AVPixelFormat f = codec_best_fmt(c);
        if (f != AV_PIX_FMT_NONE) {
            *fmt = f;
            return c;
        }
    }
    return nullptr;
}

// crf → mediacodec 目标码率(契约 §3.2:18→0.35 / 23→0.18 / 28→0.10 线性插值)
int64_t estimate_mc_bitrate(int64_t src_bps, int crf, int w, int h, int fps)
{
    if (src_bps <= 0) {
        src_bps = (int64_t)((double)w * h * (double)(fps > 0 ? fps : 25) * 0.5);
        if (src_bps <= 0) src_bps = 20000000;
    }
    double f;
    if (crf <= 18) f = 0.35;
    else if (crf <= 23) f = 0.35 + (double)(crf - 18) * (0.18 - 0.35) / 5.0;
    else if (crf <= 28) f = 0.18 + (double)(crf - 23) * (0.10 - 0.18) / 5.0;
    else f = 0.10;
    double bps = (double)src_bps * f;
    if (bps < 200000.0) bps = 200000.0;
    if (bps > 80000000.0) bps = 80000000.0;
    return (int64_t)bps;
}

// ---------------------------------------------------------------------------
// filtergraph 描述串(契约 §7)
//   buffer → [autorotate transpose] → scale → setsar → format → [lut3d | split+blend] → [fps]
//   → buffersink
// ---------------------------------------------------------------------------
std::string build_descr(const proresforge::Spec &spec, int fps_num, int fps_den,
                        enum AVPixelFormat out_fmt, int rotation,
                        bool need_scale, int out_w, int out_h)
{
    // [in]/[out] 标签把解析出的链连到手动创建的 buffersrc("in")/buffersink("out"):
    // 没有标签时 parse_ptr 不会自动连线,avfilter_graph_config 会报 pad 未连接。
    std::string d = "[in]";
    char b[160];

    if (rotation == 90) d += "transpose=clock,";
    else if (rotation == 270) d += "transpose=cclock,";
    else if (rotation == 180) d += "hflip,vflip,";

    if (need_scale) {
        const std::string flags = spec.get(SpecKeys::kScaleFlags);
        std::snprintf(b, sizeof(b), "scale=%d:%d:flags=%s,", out_w, out_h,
                      flags.empty() ? "lanczos" : flags.c_str());
        d += b;
    }

    d += "setsar=1,";

    // Apple Log 先在 LibRaw 的 2020 线性 RGB 上逐通道编码，再转成目标 YUV。
    // 必须位于 format=<out_fmt> 之前，否则 lutrgb 会把 YUV 分量当作 RGB 套曲线。
    const bool applelog =
        spec.get(SpecKeys::kSource) == "dngzip" &&
        spec.get(SpecKeys::kDngMap) == "applelog";
    if (applelog) {
        const char *expr =
            "if(gte(val/65535,0.01),0.08550479*log(val/65535+0.00964052)/"
            "0.6931471805599453+0.69336945,47.28711236*pow(val/65535+0.05641088,2))*65535";
        d += "format=gbrp16,lutrgb=r='";
        d += expr;
        d += "':g='";
        d += expr;
        d += "':b='";
        d += expr;
        d += "',scale=out_color_matrix=bt2020nc,";
    }

    const char *pf = av_get_pix_fmt_name(out_fmt);
    const std::string pfname = (pf && *pf) ? pf : "yuv420p";
    d += "format=" + pfname + ",";

    // LUT(lut3d 只接受 RGB planar,所以需要一次 YUV→RGB→YUV 往返)
    const std::string lut = spec.get(SpecKeys::kLut);
    double intensity = spec.get_d(SpecKeys::kLutIntensity, 1.0);
    if (intensity < 0.0) intensity = 0.0;
    if (intensity > 1.0) intensity = 1.0;
    if (!lut.empty() && intensity > 0.0) {
        // 10/12 位用 gbrp16 往返,尽量少损失
        const std::string rgb = (pix_depth(out_fmt) >= 10) ? "gbrp16" : "gbrp";
        const std::string lut3d = "lut3d=file=" + esc_opt(lut) + ":interp=tetrahedral";
        if (intensity >= 0.999) {
            d += "format=" + rgb + "," + lut3d + ",format=" + pfname + ",";
        } else {
            // <1:一路过 LUT,一路原图,按 intensity 混合(两路都要先 format 成同一 RGB)
            char op[64];
            std::snprintf(op, sizeof(op), "%.4f", intensity);
            d += "split=2[a][b];";
            d += "[a]format=" + rgb + "," + lut3d + "[ga];";
            d += "[b]format=" + rgb + "[gb];";
            d += "[ga][gb]blend=all_mode=normal:all_opacity=" + std::string(op) + ",";
            d += "format=" + pfname + ",";
        }
    }

    if (fps_num > 0 && fps_den > 0) {
        std::snprintf(b, sizeof(b), "fps=%d/%d,", fps_num, fps_den);
        d += b;
    }

    while (!d.empty() && (d.back() == ',' || d.back() == ';')) d.erase(d.size() - 1);
    d += "[out]";
    return d;
}

bool container_takes_audio(const AVOutputFormat *ofmt, enum AVCodecID id)
{
    if (!ofmt) return false;
    return avformat_query_codec(ofmt, id, FF_COMPLIANCE_NORMAL) == 1;
}

// ---------------------------------------------------------------------------
// 管线上下文(把解码→filter→编码→写盘这段收拢成小函数,冲刷与常规路径复用)
// ---------------------------------------------------------------------------
struct Pipe {
    AVFormatContext *oc = nullptr;
    AVFormatContext *ic = nullptr;
    AVCodecContext *dec = nullptr;
    AVCodecContext *enc = nullptr;
    AVFilterGraph *g = nullptr;
    AVFilterContext *fsrc = nullptr;
    AVFilterContext *fsink = nullptr;
    AVRational sink_tb{0, 1};
    AVStream *vost = nullptr;
    AVFrame *dframe = nullptr;
    AVFrame *sframe = nullptr;
    AVPacket *pkt = nullptr; // 视频编码输出
    int64_t last_pts = AV_NOPTS_VALUE; // 编码器 time_base 下上一帧 pts(见 encode_out 注释)
    int64_t synth_pts = 0;   // 源缺 pts 时的合成帧号
    int64_t frames = 0;
    int64_t total = -1;
    int64_t duration_us = 0;
    int fps_num = 25;
    int fps_den = 1;
    Reporter rep;
};

// sframe 里的帧 → 编码 → 写盘(内含 pts 单调化)
int encode_out(Pipe *p)
{
    AVFrame *f = p->sframe;
    // pts 兜底:last_pts 初值 AV_NOPTS_VALUE(见 Pipe 定义处)。首帧 NOPTS 退回 0,
    // 之后沿用“上一帧 + 1”;首帧 pts=0 不会再被误判成 1。
    int64_t pts = f->pts;
    if (pts == AV_NOPTS_VALUE ||
        (p->last_pts != AV_NOPTS_VALUE && pts <= p->last_pts)) {
        pts = (p->last_pts == AV_NOPTS_VALUE) ? 0 : p->last_pts + 1;
    }
    p->last_pts = pts;
    f->pts = pts;
    f->time_base = p->enc->time_base;

    const char *codec_name = (p->enc->codec && p->enc->codec->name)
                                 ? p->enc->codec->name : "unknown";
    const char *pix_name = av_get_pix_fmt_name(p->enc->pix_fmt);
    auto log_encode_error = [&](const char *stage, int error) {
        if (error != AVERROR(EAGAIN) && error != AVERROR_EOF) {
            av_log(nullptr, AV_LOG_ERROR,
                   "transcode: video encode %s failed codec=%s pix_fmt=%s size=%dx%d "
                   "pts=%lld ret=%d (%s)\n",
                   stage, codec_name, pix_name ? pix_name : "unknown", p->enc->width,
                   p->enc->height, (long long)pts, error, err_str(error));
        }
        return error;
    };

    int ret = avcodec_send_frame(p->enc, f);
    if (ret == AVERROR(EAGAIN)) {
        // send_frame 未接受输入时必须先排空编码器再重送同一帧,不能丢帧。
        while (true) {
            ret = avcodec_receive_packet(p->enc, p->pkt);
            if (ret == AVERROR(EAGAIN)) break;
            if (ret == AVERROR_EOF) return ret;
            if (ret < 0) return log_encode_error("receive-after-send-eagain", ret);
            av_packet_rescale_ts(p->pkt, p->enc->time_base, p->vost->time_base);
            p->pkt->stream_index = p->vost->index;
            ret = av_interleaved_write_frame(p->oc, p->pkt);
            av_packet_unref(p->pkt);
            if (ret < 0) {
                av_log(nullptr, AV_LOG_ERROR,
                       "transcode: video mux write failed ret=%d (%s)\n", ret, err_str(ret));
                return ret;
            }
        }
        ret = avcodec_send_frame(p->enc, f);
    }
    if (ret < 0) return log_encode_error("send_frame", ret);
    while (true) {
        ret = avcodec_receive_packet(p->enc, p->pkt);
        if (ret == AVERROR(EAGAIN)) break;
        if (ret == AVERROR_EOF) return ret;
        if (ret < 0) return log_encode_error("receive_packet", ret);
        av_packet_rescale_ts(p->pkt, p->enc->time_base, p->vost->time_base);
        p->pkt->stream_index = p->vost->index;
        ret = av_interleaved_write_frame(p->oc, p->pkt);
        av_packet_unref(p->pkt);
        if (ret < 0) {
            av_log(nullptr, AV_LOG_ERROR,
                   "transcode: video mux write failed ret=%d (%s)\n", ret, err_str(ret));
            return ret;
        }
    }
    return 0;
}

// filtergraph 能出多少帧就编码写多少
int drain_graph(Pipe *p)
{
    while (true) {
        int ret = av_buffersink_get_frame(p->fsink, p->sframe);
        if (ret == AVERROR(EAGAIN)) break;
        if (ret == AVERROR_EOF) break;
        if (ret < 0) return ret;

        // 进度:优先按帧 pts 与总时长比,退回按已处理帧数比。
        // 必须用 pts——滤镜图只重定时 pts;best_effort_timestamp 仍是输入侧时基,
        // 按 sink_tb 换算会算出天文数字,进度直接顶到 100%。
        double pct = -1.0;
        int64_t t = p->sframe->pts;
        if (t != AV_NOPTS_VALUE && p->duration_us > 0) {
            int64_t us = av_rescale_q(t, p->sink_tb, (AVRational){1, AV_TIME_BASE});
            if (us >= 0) pct = 100.0 * (double)us / (double)p->duration_us;
        }
        if (pct < 0.0 && p->total > 0) {
            pct = 100.0 * (double)p->frames / (double)p->total;
        }
        if (pct < 0.0) pct = 0.0;
        if (pct > 100.0) pct = 100.0;

        // sink 输出的 pts 已在 sink_tb,转成编码器 time_base
        p->sframe->pts = av_rescale_q(p->sframe->pts, p->sink_tb, p->enc->time_base);
        ret = encode_out(p);
        av_frame_unref(p->sframe);
        if (ret < 0) return ret;

        p->rep((int)(pct + 0.5), "decode", p->frames);
    }
    return 0;
}

// 一帧解码输出 → filtergraph。
// keep=true 时用 AV_BUFFERSRC_FLAG_KEEP_REF 推帧且不 unref:帧的 buffer 归调用方
// 继续持有(P1-3 池化路径专用,见 AvFramePool 的所有权注释);keep=false 是老路径
// (flags=0,move 之后 unref 是 no-op,仅兜底)。
static int push_frame(Pipe *p, AVFrame *f, bool keep)
{
    if (f->pts == AV_NOPTS_VALUE) {
        // 源没有 pts:按帧率合成,保证单调递增
        f->pts = av_rescale_q(p->synth_pts, (AVRational){p->fps_den, p->fps_num},
                              p->fsrc->inputs[0]->time_base);
        p->synth_pts++;
    }
    int ret = av_buffersrc_add_frame_flags(p->fsrc, f,
                                           keep ? AV_BUFFERSRC_FLAG_KEEP_REF : 0);
    if (!keep) av_frame_unref(f);
    if (ret < 0) return ret;
    // 进度帧数按「已处理的源帧」计(与 totalFrames 同口径):fps 滤镜会复制/丢帧,
    // 若按输出帧计,CFR 化时会出现 119/112 这类越过总数的显示
    p->frames++;
    return drain_graph(p);
}

int push_decoded(Pipe *p)
{
    return push_frame(p, p->dframe, false);
}

// 把解码器里缓存的帧全部排空
int pump_decoder(Pipe *p)
{
    while (true) {
        int ret = avcodec_receive_frame(p->dec, p->dframe);
        if (ret == AVERROR(EAGAIN)) break;
        if (ret == AVERROR_EOF) break;
        if (ret < 0) return ret;
        ret = push_decoded(p);
        if (ret < 0) return ret;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 音频(aac 路径)
// ---------------------------------------------------------------------------
struct APipe {
    AVFormatContext *oc = nullptr;
    AVCodecContext *dec = nullptr;
    AVCodecContext *enc = nullptr;
    SwrContext *swr = nullptr;
    AVAudioFifo *afifo = nullptr; // 重采样样本缓冲:AAC 只收整帧,靠它攒样本
    AVStream *ost = nullptr;
    AVStream *ist = nullptr;
    AVFrame *fin = nullptr;
    AVFrame *fout = nullptr;
    AVPacket *pkt = nullptr;
    int64_t last_out_pts = 0;
    bool pts_init = false;
};

// 从 fifo 取样本喂编码器并写出包;allow_partial=true 仅用于结尾最后一帧
// (AAC 除最后一帧外必须恰好 frame_size 个样本,否则编码器报 EINVAL)
int aac_emit_fifo(APipe *a, bool allow_partial)
{
    const int frame_size = a->enc->frame_size > 0 ? a->enc->frame_size : 1024;
    while (true) {
        const int have = av_audio_fifo_size(a->afifo);
        int take = 0;
        if (have >= frame_size) take = frame_size;
        else if (allow_partial) take = have;
        if (take <= 0) break;

        AVFrame *fout = a->fout;
        av_frame_unref(fout);
        fout->format = a->enc->sample_fmt;
        fout->ch_layout = a->enc->ch_layout;
        fout->sample_rate = a->enc->sample_rate;
        fout->nb_samples = take;
        int ret = av_frame_get_buffer(fout, 0);
        if (ret < 0) return ret;
        if (av_audio_fifo_read(a->afifo, (void **)fout->data, take) < take)
            return AVERROR(EINVAL);
        fout->pts = a->last_out_pts;
        a->last_out_pts += take;
        ret = avcodec_send_frame(a->enc, fout);
        if (ret == AVERROR(EAGAIN)) {
            // 编码器未接受当前 AAC 帧时先排空输出,再重送同一帧。
            while (true) {
                ret = avcodec_receive_packet(a->enc, a->pkt);
                if (ret == AVERROR(EAGAIN)) break;
                if (ret == AVERROR_EOF) return ret;
                if (ret < 0) return ret;
                av_packet_rescale_ts(a->pkt, a->enc->time_base, a->ost->time_base);
                a->pkt->stream_index = a->ost->index;
                ret = av_interleaved_write_frame(a->oc, a->pkt);
                av_packet_unref(a->pkt);
                if (ret < 0) return ret;
            }
            ret = avcodec_send_frame(a->enc, fout);
        }
        if (ret < 0) return ret;
        while (true) {
            ret = avcodec_receive_packet(a->enc, a->pkt);
            if (ret == AVERROR(EAGAIN)) break;
            if (ret == AVERROR_EOF) return ret;
            if (ret < 0) return ret;
            av_packet_rescale_ts(a->pkt, a->enc->time_base, a->ost->time_base);
            a->pkt->stream_index = a->ost->index;
            ret = av_interleaved_write_frame(a->oc, a->pkt);
            av_packet_unref(a->pkt);
            if (ret < 0) return ret;
        }
    }
    return 0;
}

// 解码出一帧:重采样进 fifo,攒满整帧就喂编码器
int aac_encode_frame(APipe *a, AVFrame *f)
{
    int out_samples = (int)av_rescale_rnd(swr_get_delay(a->swr, a->dec->sample_rate) +
                                              f->nb_samples,
                                          a->enc->sample_rate, a->dec->sample_rate, AV_ROUND_UP);
    if (out_samples > 0) {
        if (av_audio_fifo_space(a->afifo) < out_samples) {
            // realloc 只扩 fifo 缓冲,真失败会在紧随的 av_audio_fifo_write 上暴露
            // (返回 <0 的日志),这里留个 warning 便于定位(旧代码裸调,warn_unused_result)。
            if (av_audio_fifo_realloc(a->afifo, av_audio_fifo_size(a->afifo) + out_samples) < 0)
                av_log(nullptr, AV_LOG_WARNING, "transcode: audio fifo realloc(%d) failed\n", out_samples);
        }
        av_frame_unref(a->fout);
        a->fout->format = a->enc->sample_fmt;
        a->fout->ch_layout = a->enc->ch_layout;
        a->fout->sample_rate = a->enc->sample_rate;
        a->fout->nb_samples = out_samples;
        int ret = av_frame_get_buffer(a->fout, 0);
        if (ret < 0) return ret;
        ret = swr_convert(a->swr, a->fout->data, out_samples,
                          (const uint8_t **)f->data, f->nb_samples);
        if (ret < 0) return ret;
        if (ret > 0) {
            if (!a->pts_init) {
                // 输出 pts 用样本计数(起点取首帧 pts),避免逐帧 rescale 漂移
                a->last_out_pts = (f->pts != AV_NOPTS_VALUE)
                                      ? av_rescale_q(f->pts, a->ist->time_base, a->enc->time_base)
                                      : 0;
                a->pts_init = true;
            }
            int w = av_audio_fifo_write(a->afifo, (void **)a->fout->data, ret);
            // 返回值之前没人看:写入不足/失败会让这几毫秒样本凭空消失(尾部静默丢数据)
            if (w < ret) return w < 0 ? w : AVERROR(EINVAL);
        }
    }
    return aac_emit_fifo(a, false);
}

// swr 内部残样冲进 fifo(EOF 时调用一次)
int aac_flush_resample(APipe *a)
{
    while (true) {
        int n = (int)swr_get_delay(a->swr, a->enc->sample_rate) + 16;
        if (av_audio_fifo_space(a->afifo) < n) {
            // 同上:realloc 失败最终由 fifo_write 暴露,这里只记 warning
            if (av_audio_fifo_realloc(a->afifo, av_audio_fifo_size(a->afifo) + n) < 0)
                av_log(nullptr, AV_LOG_WARNING, "transcode: audio fifo realloc(flush %d) failed\n", n);
        }
        av_frame_unref(a->fout);
        a->fout->format = a->enc->sample_fmt;
        a->fout->ch_layout = a->enc->ch_layout;
        a->fout->sample_rate = a->enc->sample_rate;
        a->fout->nb_samples = n;
        int ret = av_frame_get_buffer(a->fout, 0);
        if (ret < 0) return ret;
        ret = swr_convert(a->swr, a->fout->data, n, nullptr, 0);
        if (ret < 0) return ret;
        if (ret <= 0) break;
        int w = av_audio_fifo_write(a->afifo, (void **)a->fout->data, ret);
        if (w < ret) return w < 0 ? w : AVERROR(EINVAL);
    }
    return 0;
}

// 把音频解码器里缓存的帧全部重采样编码写盘
int pump_audio_dec(APipe *a)
{
    while (true) {
        int ret = avcodec_receive_frame(a->dec, a->fin);
        if (ret == AVERROR(EAGAIN)) break;
        if (ret == AVERROR_EOF) {
            ret = aac_flush_resample(a);
            if (ret < 0) return ret;
            return aac_emit_fifo(a, true); // 最后一帧允许不足 frame_size
        }
        if (ret < 0) return ret;
        if (a->fin->pts == AV_NOPTS_VALUE) a->fin->pts = a->last_out_pts;
        ret = aac_encode_frame(a, a->fin);
        av_frame_unref(a->fin);
        if (ret < 0) return ret;
    }
    return 0;
}

int drain_audio_bsf(AVBSFContext *bsf, AVFormatContext *oc, AVStream *ost, AVPacket *pkt)
{
    while (true) {
        int ret = av_bsf_receive_packet(bsf, pkt);
        if (ret == AVERROR(EAGAIN)) return AVERROR(EAGAIN);
        if (ret == AVERROR_EOF) return AVERROR_EOF;
        if (ret < 0) {
            av_packet_unref(pkt);
            return ret;
        }
        pkt->stream_index = ost->index;
        ret = av_interleaved_write_frame(oc, pkt);
        av_packet_unref(pkt);
        if (ret < 0) return ret;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 主入口
// ---------------------------------------------------------------------------
int run_transcode(const TranscodeParams &params)
{
    const bool owns_lifecycle = begin_transcode();

    // 返回值语义:0 成功 / 1 取消 / 2 输出格式或编码器不支持 /
    // 3 输入文件损坏或无法解码 / <0 = -AVERROR
    int result = 0;
    int ret = 0;

    // fd 契约 §2:两个 fd 的所有权已从 Kotlin 移交进来,done: 标签统一 close。
    // 但 spec.parse / build_chain / 分配等任何 bad_alloc 会跳过 done: 的 close
    // (JNI 入口的 catch 只能收成返回码,无法安全补 close —— 不知道这里关没关)。
    // FdGuard 兜住异常出口:正常路径 close 后置 -1(守卫变 no-op),两者互斥。
    // 注意 out_ctx.fd 与 fd_out_owned 是同一个数字:fd_out_owned 置 -1 后
    // fd_io_write 对已关 fd 的写会返回负错误码,不会再有副作用。
    int fd_in_owned = params.fd_in;
    int fd_out_owned = params.fd_out;
    FdGuard fd_in_guard{ &fd_in_owned };
    FdGuard fd_out_guard{ &fd_out_owned };

    // 输入
    AVFormatContext *ic = nullptr;
    FdIoCtx *in_ctx = nullptr;
    AVIOContext *in_pb = nullptr;
    uint8_t *in_buf = nullptr;

    // 输出
    AVFormatContext *oc = nullptr;
    FdIoCtx out_ctx;
    out_ctx.fd = params.fd_out;
    out_ctx.pos = 0;
    out_ctx.size = 0;
    AVIOContext *out_pb = nullptr;
    uint8_t *out_buf = nullptr;
    // av_write_trailer 调用过的标记(P1-03):faststart 移动 moov 的前提就是
    // moov 已由 trailer 写入,因此只允许在「trailer 已写过(且仅此一次,
    // 绝不重复 av_write_trailer)」之后跑;成功路径与 loop_error 路径共用。
    bool trailer_written = false;

    // 视频
    int vstream = -1;
    AVStream *v_ist = nullptr;
    AVCodecContext *dec = nullptr;
    Pipe vp;
    AVCodecContext *enc = nullptr;
    AVStream *v_ost = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *dframe = nullptr;
    AVFrame *sframe = nullptr;

    // 音频
    int astream = -1;
    AVFormatContext *audio_ic = nullptr; // 普通容器复用 ic,DNG ZIP 指向内存 WAV context
    MemIoCtx *audio_in_ctx = nullptr;
    AVIOContext *audio_in_pb = nullptr;
    uint8_t *audio_in_buf = nullptr;
    std::vector<uint8_t> dng_audio_buf;
    AVStream *a_ist = nullptr;
    AVCodecContext *adec = nullptr;
    AVCodecContext *aenc = nullptr;
    SwrContext *swr = nullptr;
    AVBSFContext *bsf = nullptr;
    AVStream *a_ost = nullptr;
    APipe ap;
    AVPacket *apkt = nullptr;
    AVPacket *zip_audio_pkt = nullptr;
    AVFrame *aframe_in = nullptr;
    AVFrame *aframe_out = nullptr;
    AVDictionary *mux_opts = nullptr;
    bool mov_muxer = false;
    bool audio_copy = false; // true = 直通;false = 不处理或 aac
    bool audio_aac = false;
    std::string audio_mode;

    // DNG 序列 zip(契约 §10):src=dngzip 时整条视频链换成「ZIP 中央目录 + zlib inflate
    // + LibRaw」,不走 avformat/avcodec 解码。zip_w/zip_h 是首帧解出的真实尺寸。
    DngZip dngzip;
    std::vector<uint8_t> dng_buf;   // 单帧 DNG(压缩态),循环外声明只为复用容量
    std::vector<uint16_t> dng_rgb;  // 单帧解出的 rgb48le(w*h*3 个 uint16)
    int zip_w = 0, zip_h = 0;
    // init 段首帧解码产物:zip 路径直接 seed 进 DngFrameQueue(见下方主循环),
    // 非 zip 路径保持空,零影响。
    proresforge::DngFrame seed_frame;
    bool have_seed = false;
    DngQuality dngq = DngQuality::BALANCED;
    DngColorMode dngcm = DngColorMode::CAMERA;
    DngMap dngmap = DngMap::NONE;

    // ---------------- spec ----------------
    proresforge::Spec spec;
    spec.parse(params.spec_text);

    std::string vcodec = spec.get(SpecKeys::kVideoCodec);
    if (vcodec.empty()) vcodec = "h264";
    const Family *fam = family_of(vcodec);
    if (!fam) fam = &kFamilies[0]; // 未知 → 退回 h264
    std::vector<std::string> chain = build_chain(fam, vcodec);

    enum AVPixelFormat want_fmt = parse_pix_fmt(spec.get(SpecKeys::kPixFmt));
    // 注意第二参用 0LL/1LL:字面量 0 同时可转 long long 与 long long*,会造成重载歧义
    int out_w = (int)spec.get_i(SpecKeys::kWidth, 0LL);
    int out_h = (int)spec.get_i(SpecKeys::kHeight, 0LL);
    int fps_num = (int)spec.get_i(SpecKeys::kFpsNum, 0LL);
    int fps_den = (int)spec.get_i(SpecKeys::kFpsDen, 0LL);
    std::string rc = spec.get(SpecKeys::kRateControl);
    int crf = (int)spec.get_i(SpecKeys::kCrf, 20);
    int64_t bitrate = spec.get_l(SpecKeys::kBitrate, 0LL);
    int gop_seconds = (int)spec.get_i(SpecKeys::kGopSeconds, 0LL);
    std::string profile = spec.get(SpecKeys::kProfile);
    audio_mode = spec.get(SpecKeys::kAudio);
    if (audio_mode.empty()) audio_mode = "copy";
    int faststart = (int)spec.get_i(SpecKeys::kFastStart, 1LL);

    // DNG zip 开关先于 avformat_open_input 判定(契约 §10.3)
    const bool zip_src = (spec.get(SpecKeys::kSource) == "dngzip");
    if (zip_src) {
        {
            long v = spec.get_l(SpecKeys::kDngQuality, 1LL);
            if (v < 0) v = 0;
            if (v > 2) v = 2;
            dngq = (DngQuality)(int)v;
        }
        {
            const std::string c = spec.get(SpecKeys::kDngColor);
            if (c == "auto") dngcm = DngColorMode::AUTO;
            else if (c == "d65") dngcm = DngColorMode::D65;
            else dngcm = DngColorMode::CAMERA;
        }
        dngmap = spec.get(SpecKeys::kDngMap) == "applelog"
                     ? DngMap::APPLE_LOG
                     : DngMap::NONE;
    }

    vp.rep.p = &params;
    // nativeCancel 可以在 JNI 参数准备完成、真正打开输入之前到达。此时直接
    // 返回取消码,避免启动窗口继续创建输出头或消费首帧。
    if (is_cancelled()) {
        result = 1;
        goto done;
    }
    vp.rep(0, "init", 0);

    // ---------------- 打开输入 ----------------
    if (zip_src) {
        // DNG 序列 zip(契约 §10.3):不解容器,先解中央目录,再解首帧拿尺寸/像素格式。
        // 首帧解不开 → 当作「输入损坏/无法解码」(result=3),与 Kotlin 侧的损坏终态一致。
        std::string zerr;
        if (dngzip.open(params.fd_in, params.length, zerr) != 0) {
            av_log(nullptr, AV_LOG_ERROR, "transcode: dngzip open failed: %s\n", zerr.c_str());
            result = kTranscodeInvalidInput;
            goto done;
        }
        if (dngzip.frame_count() == 0) {
            av_log(nullptr, AV_LOG_ERROR,
                   "transcode: dngzip 里没有 frame-NNNNN.dng 帧(不是 MotionCam 序列 zip)\n");
            result = kTranscodeInvalidInput;
            goto done;
        }
        if (dngzip.read_frame(0, dng_buf, zerr) != 0 ||
            dng_decode_to_rgb48(dng_buf.data(), dng_buf.size(), dngq, dngcm, dng_rgb,
                                zip_w, zip_h, zerr, nullptr, nullptr, dngmap,
                                false) != 0) {
            av_log(nullptr, AV_LOG_ERROR, "transcode: dngzip 首帧解码失败: %s\n", zerr.c_str());
            zip_w = 0;
            zip_h = 0;
            result = kTranscodeInvalidInput;
            goto done;
        }
        if (dngzip.has_audio() && audio_mode != "drop") {
            std::string audio_err;
            if (dngzip.read_audio(dng_audio_buf, audio_err) != 0) {
                av_log(nullptr, AV_LOG_ERROR,
                       "transcode: ZIP audio.wav 解压失败: %s\n", audio_err.c_str());
                result = kTranscodeInvalidInput;
                goto done;
            }
            ret = mem_io_open_input(dng_audio_buf.data(), (int64_t)dng_audio_buf.size(),
                                    &audio_ic, &audio_in_ctx, &audio_in_pb, &audio_in_buf);
            if (ret < 0 || !audio_ic || avformat_find_stream_info(audio_ic, nullptr) < 0) {
                av_log(nullptr, AV_LOG_ERROR,
                       "transcode: ZIP audio.wav 解析失败(ret=%d,%s)\n", ret, err_str(ret));
                result = kTranscodeInvalidInput;
                goto done;
            }
            astream = av_find_best_stream(audio_ic, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (astream < 0) {
                av_log(nullptr, AV_LOG_WARNING, "transcode: ZIP audio.wav 没有可用音频流\n");
                mem_io_free_input(&audio_ic, &audio_in_ctx, &audio_in_pb, &audio_in_buf);
            }
        }
        // 首帧已解(拿尺寸用的那次解码),别让 worker 再从 seq=0 白解一遍:
        // rgb48le 结果搬进 seed 帧,pool.start() 前 seed() 进队列 slot0,
        // worker 从 seq=1 开始派。dng_buf/dng_rgb 之后不再用,顺手转移所有权
        // 避免高层级拷贝(72MB rgb 尤其)。
        seed_frame.rgb.swap(dng_rgb);
        seed_frame.raw.swap(dng_buf);
        seed_frame.w = zip_w;
        seed_frame.h = zip_h;
        have_seed = true;
    } else {
        ret = fd_io_open_input(params.fd_in, params.length, &ic, &in_ctx, &in_pb, &in_buf);
        if (ret < 0) {
            result = kTranscodeInvalidInput;
            goto done;
        }
        ret = avformat_find_stream_info(ic, nullptr);
        if (ret < 0) {
            result = kTranscodeInvalidInput;
            goto done;
        }
        vstream = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vstream < 0) {
            result = kTranscodeInvalidInput;
            goto done;
        }
        v_ist = ic->streams[vstream];
    }

    // ---------------- 输出容器 + 解码器 + 编码器 + 音频 + filtergraph ----------------
    {
        ret = avformat_alloc_output_context2(&oc, nullptr, fam->container, nullptr);
        if (ret < 0 || !oc) {
            result = ret < 0 ? ret : AVERROR(EINVAL);
            goto done;
        }
        out_pb = fd_io_new_avio(&out_ctx, 0x40000, true, &out_buf);
        if (!out_pb) {
            result = AVERROR(ENOMEM);
            goto done;
        }
        oc->pb = out_pb;
        oc->flags |= AVFMT_FLAG_CUSTOM_IO;

        // 解码器(DNG zip 分支没有 FFmpeg 视频解码器:帧由 LibRaw 出,type 是 rgb48le)
        if (!zip_src) {
            const AVCodec *dcodec = avcodec_find_decoder(v_ist->codecpar->codec_id);
            if (!dcodec) {
                result = kTranscodeInvalidInput;
                goto done;
            }
            dec = avcodec_alloc_context3(dcodec);
            if (!dec) {
                result = AVERROR(ENOMEM);
                goto done;
            }
            ret = avcodec_parameters_to_context(dec, v_ist->codecpar);
            if (ret < 0) {
                result = ret;
                goto done;
            }
            dec->thread_count = 4;
            dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            ret = avcodec_open2(dec, dcodec, nullptr);
            if (ret < 0) {
                result = kTranscodeInvalidInput;
                goto done;
            }
            if (dec->pix_fmt == AV_PIX_FMT_NONE) {
                av_log(nullptr, AV_LOG_ERROR,
                       "transcode: input video pixel format is unknown; input may be damaged "
                       "(codec=%s size=%dx%d)\n",
                       dec->codec && dec->codec->name ? dec->codec->name : "unknown",
                       dec->width, dec->height);
                result = kTranscodeInvalidInput;
                goto done;
            }
        }

        int rotation = 0;
        int src_w = 0;
        int src_h = 0;
        if (zip_src) {
            // DNG 帧没有容器旋转信息(传感器方向由相机侧处理),src 即 LibRaw 输出尺寸
            src_w = zip_w;
            src_h = zip_h;
        } else {
            rotation = detect_rotation_deg(v_ist, spec.get(SpecKeys::kRotate));
            src_w = dec->width;
            src_h = dec->height;
        }

        // 输出尺寸(spec 没给就取源尺寸,autorotate 之后)
        if (out_w <= 0 || out_h <= 0) {
            out_w = src_w;
            out_h = src_h;
            if (rotation == 90 || rotation == 270) {
                int t = out_w;
                out_w = out_h;
                out_h = t;
            }
        }
        out_w = round_down_even(out_w);
        out_h = round_down_even(out_h);
        if (out_w < 2) out_w = 2;
        if (out_h < 2) out_h = 2;
        if (out_w > 8192) out_w = 8192;
        if (out_h > 8192) out_h = 8192;

        // 帧率(zip 没有容器帧率:缺 spec 就按 25fps 兜底,Kotlin 会强制用户选)
        if (fps_num <= 0 || fps_den <= 0) {
            bool have = false;
            if (!zip_src) {
                if (v_ist->avg_frame_rate.num > 0 && v_ist->avg_frame_rate.den > 0) {
                    fps_num = v_ist->avg_frame_rate.num;
                    fps_den = v_ist->avg_frame_rate.den;
                    have = true;
                } else if (dec->framerate.num > 0 && dec->framerate.den > 0) {
                    fps_num = dec->framerate.num;
                    fps_den = dec->framerate.den;
                    have = true;
                }
            }
            if (!have) {
                fps_num = 25;
                fps_den = 1;
            }
        }

        // 是否需要 scale(autorotate 后的期望尺寸与目标一致就不加,省一次缩放)
        int expect_w = (rotation == 90 || rotation == 270) ? src_h : src_w;
        int expect_h = (rotation == 90 || rotation == 270) ? src_w : src_h;
        bool need_scale = (out_w != expect_w || out_h != expect_h);

        // 编码器(fallback chain)。候选顺序保持契约语义：先满足请求像素格式，
        // 再允许降级到各编码器的首选格式；每个候选都必须完成 open2 才算可用。
        // MediaCodec 在部分设备上会在 open2 阶段以 EINVAL 拒绝分辨率/码率/位深，
        // 不能只按 avcodec_find_encoder 的结果认定它可用。
        enum AVPixelFormat enc_fmt = AV_PIX_FMT_NONE;
        std::vector<std::pair<const AVCodec *, enum AVPixelFormat>> candidates;
        auto add_candidate = [&](const AVCodec *c, enum AVPixelFormat fmt) {
            if (!c || fmt == AV_PIX_FMT_NONE) return;
            for (size_t i = 0; i < candidates.size(); i++) {
                if (candidates[i].first == c) return;
            }
            candidates.push_back({c, fmt});
        };
        if (want_fmt != AV_PIX_FMT_NONE) {
            for (size_t i = 0; i < chain.size(); i++) {
                const AVCodec *c = avcodec_find_encoder_by_name(chain[i].c_str());
                if (c && codec_supports(c, want_fmt)) add_candidate(c, want_fmt);
            }
        }
        for (size_t i = 0; i < chain.size(); i++) {
            const AVCodec *c = avcodec_find_encoder_by_name(chain[i].c_str());
            if (c) add_candidate(c, codec_best_fmt(c));
        }
        if (candidates.empty()) {
            av_log(nullptr, AV_LOG_ERROR,
                   "transcode: no output encoder available for requested codec family=%s\n",
                   fam && fam->name ? fam->name : "unknown");
            result = 2;
            goto done;
        }

        const AVCodec *ecodec = nullptr;
        std::string enc_name;
        bool is_mc = false;
        bool is_x264 = false;
        bool is_x265 = false;
        bool is_vp9 = false;
        bool is_prores = false;
        int last_open_ret = AVERROR(EINVAL);

        // 把候选参数集中到一个函数,这样 MediaCodec 预检和正式编码器使用完全相同
        // 的 width/height/pix_fmt/fps/码率选项。预检不能复用正式上下文:send_frame
        // 会改变编码器状态,必须在临时上下文上验证后再重新 open 正式上下文。
        auto configure_encoder = [&](AVCodecContext *ctx, const AVCodec *codec,
                                     enum AVPixelFormat fmt, AVDictionary **opts) {
            const std::string name = (codec && codec->name) ? codec->name : "unknown";
            const bool mc = name.find("mediacodec") != std::string::npos;
            const bool x264 = name == "libx264";
            const bool x265 = name == "libx265";
            const bool vp9 = name == "libvpx-vp9";
            const bool prores = name == "prores_ks";

            ctx->width = out_w;
            ctx->height = out_h;
            ctx->pix_fmt = fmt;
            ctx->sample_aspect_ratio = (AVRational){1, 1};
            ctx->framerate = (AVRational){fps_num, fps_den};
            ctx->time_base = (AVRational){fps_den, fps_num}; // 统一 1/fps
            // mediacodec 没有默认关键帧间隔(gop_size/fps == 0 会变成每帧关键帧),故给 2 秒
            ctx->gop_size = (gop_seconds > 0 || mc)
                                ? fps_num * (gop_seconds > 0 ? gop_seconds : 2)
                                : 0;
            ctx->max_b_frames = (x264 || x265) ? 2 : 0;

            // 码率控制(契约 §3.2)
            if (x264) {
                av_dict_set(opts, "preset", "medium", 0);
                if (!profile.empty()) av_dict_set(opts, "profile", profile.c_str(), 0);
                if (rc == "bitrate" && bitrate > 0) ctx->bit_rate = bitrate;
                else av_dict_set_int(opts, "crf", crf, 0);
            } else if (x265) {
                if (rc == "bitrate" && bitrate > 0) ctx->bit_rate = bitrate;
                else av_dict_set_int(opts, "crf", crf, 0);
                if (!profile.empty()) av_dict_set(opts, "profile", profile.c_str(), 0);
                else if (pix_depth(fmt) >= 10) av_dict_set(opts, "profile", "main10", 0);
                av_dict_set(opts, "log-level", "error", 0);
            } else if (vp9) {
                int cq = crf;
                if (cq < 0) cq = 0;
                if (cq > 63) cq = 63;
                if (rc == "bitrate" && bitrate > 0) {
                    ctx->bit_rate = bitrate;
                    av_dict_set_int(opts, "b", bitrate, 0);
                } else {
                    av_dict_set_int(opts, "cq-level", cq, 0);
                }
                av_dict_set(opts, "deadline", "good", 0);
                av_dict_set(opts, "cpu-used", "4", 0);
                av_dict_set(opts, "row-mt", "1", 0);
            } else if (prores) {
                const char *p = "hq"; // 422 HQ
                if (!profile.empty()) p = profile.c_str();
                else if (fmt == AV_PIX_FMT_YUV444P10 || fmt == AV_PIX_FMT_YUVA444P10) p = "4444";
                av_opt_set(ctx->priv_data, "profile", p, 0);
#if defined(PRF_PRORES_THREADS) && PRF_PRORES_THREADS > 1
                ctx->thread_count = PRF_PRORES_THREADS;
                ctx->thread_type  = FF_THREAD_FRAME;
#endif
            } else if (mc) {
                int64_t src_bps = (v_ist && v_ist->codecpar) ? v_ist->codecpar->bit_rate : 0;
                if (src_bps <= 0 && ic) src_bps = ic->bit_rate;
                const int64_t bps = (rc == "bitrate" && bitrate > 0)
                                        ? bitrate
                                        : estimate_mc_bitrate(src_bps, crf, out_w, out_h, fps_num);
                ctx->bit_rate = bps;
                if (av_opt_set_int(ctx->priv_data, "bitrate_mode", 1, 0) < 0) {
                    av_log(nullptr, AV_LOG_WARNING,
                           "transcode: bitrate_mode set failed for %s\n", name.c_str());
                }
            } else if (rc == "bitrate" && bitrate > 0) {
                ctx->bit_rate = bitrate;
            }
        };

        bool preflight_fatal = false;
        auto preflight_mediacodec = [&](const AVCodec *codec, enum AVPixelFormat fmt) -> int {
            const char *name = (codec && codec->name) ? codec->name : "unknown";
            const char *pf = av_get_pix_fmt_name(fmt);
            AVCodecContext *probe = avcodec_alloc_context3(codec);
            if (!probe) {
                preflight_fatal = true;
                return AVERROR(ENOMEM);
            }

            auto fail_probe = [&](const char *stage, int error, AVFrame *frame, AVPacket *packet) -> int {
                av_log(nullptr, AV_LOG_ERROR,
                       "transcode: video encoder preflight failed codec=%s pix_fmt=%s "
                       "size=%dx%d fps=%d/%d stage=%s ret=%d (%s)\n",
                       name, pf ? pf : "unknown", out_w, out_h, fps_num, fps_den,
                       stage, error, err_str(error));
                av_packet_free(&packet);
                av_frame_free(&frame);
                avcodec_free_context(&probe);
                return error;
            };

            AVDictionary *probe_opts = nullptr;
            configure_encoder(probe, codec, fmt, &probe_opts);
            int probe_ret = avcodec_open2(probe, codec, &probe_opts);
            av_dict_free(&probe_opts);
            if (probe_ret < 0)
                return fail_probe("open", probe_ret, nullptr, nullptr);

            AVFrame *frame = av_frame_alloc();
            AVPacket *packet = av_packet_alloc();
            if (!frame || !packet) {
                preflight_fatal = true;
                return fail_probe("alloc", AVERROR(ENOMEM), frame, packet);
            }
            frame->format = fmt;
            frame->width = out_w;
            frame->height = out_h;
            frame->pts = 0;
            probe_ret = av_frame_get_buffer(frame, 32);
            if (probe_ret < 0) {
                if (probe_ret == AVERROR(ENOMEM)) preflight_fatal = true;
                return fail_probe("frame-buffer", probe_ret, frame, packet);
            }

            auto drain_probe = [&]() -> int {
                while (true) {
                    int r = avcodec_receive_packet(probe, packet);
                    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return 0;
                    if (r < 0) return r;
                    av_packet_unref(packet);
                }
            };

            // 空白但尺寸/格式正确的帧足以触发 MediaCodec 的输入协商;临时上下文
            // 只用于探测,绝不把这一个测试包写入输出文件。
            probe_ret = avcodec_send_frame(probe, frame);
            if (probe_ret == AVERROR(EAGAIN)) {
                int r = drain_probe();
                if (r < 0) return fail_probe("receive-after-send-eagain", r, frame, packet);
                probe_ret = avcodec_send_frame(probe, frame);
            }
            if (probe_ret < 0 && probe_ret != AVERROR(EAGAIN) && probe_ret != AVERROR_EOF)
                return fail_probe("send-frame", probe_ret, frame, packet);
            if (probe_ret == 0) {
                int r = drain_probe();
                if (r < 0) return fail_probe("receive-packet", r, frame, packet);
            }

            av_log(nullptr, AV_LOG_INFO,
                   "transcode: video encoder preflight ok codec=%s pix_fmt=%s size=%dx%d fps=%d/%d\n",
                   name, pf ? pf : "unknown", out_w, out_h, fps_num, fps_den);
            av_packet_free(&packet);
            av_frame_free(&frame);
            avcodec_free_context(&probe);
            return 0;
        };

        for (size_t candidate_i = 0; candidate_i < candidates.size(); candidate_i++) {
            const AVCodec *candidate_codec = candidates[candidate_i].first;
            const enum AVPixelFormat candidate_fmt = candidates[candidate_i].second;
            const char *candidate_pf = av_get_pix_fmt_name(candidate_fmt);
            enc = avcodec_alloc_context3(candidate_codec);
            if (!enc) {
                result = AVERROR(ENOMEM);
                goto done;
            }
            enc_fmt = candidate_fmt;
            enc_name = candidate_codec->name ? candidate_codec->name : "unknown";
            is_mc = enc_name.find("mediacodec") != std::string::npos;
            is_x264 = enc_name == "libx264";
            is_x265 = enc_name == "libx265";
            is_vp9 = enc_name == "libvpx-vp9";
            is_prores = enc_name == "prores_ks";

            AVDictionary *enc_opts = nullptr;
            configure_encoder(enc, candidate_codec, candidate_fmt, &enc_opts);
            if (is_mc) {
                ret = preflight_mediacodec(candidate_codec, candidate_fmt);
                if (ret < 0) {
                    av_dict_free(&enc_opts);
                    last_open_ret = ret;
                    av_log(nullptr, AV_LOG_WARNING,
                           "transcode: video encoder preflight rejected codec=%s pix_fmt=%s "
                           "ret=%d (%s)%s\n",
                           enc_name.c_str(), candidate_pf ? candidate_pf : "unknown", ret,
                           err_str(ret), candidate_i + 1 < candidates.size()
                           ? "; trying fallback" : "; no fallback available");
                    avcodec_free_context(&enc);
                    if (preflight_fatal) {
                        result = ret;
                        goto done;
                    }
                    continue;
                }
            }

            ret = avcodec_open2(enc, candidate_codec, &enc_opts);
            av_dict_free(&enc_opts);
            if (ret == 0) {
                ecodec = candidate_codec;
                av_log(nullptr, AV_LOG_INFO,
                       "transcode: video encoder selected=%s pix_fmt=%s size=%dx%d fps=%d/%d\n",
                       enc_name.c_str(), candidate_pf ? candidate_pf : "unknown",
                       out_w, out_h, fps_num, fps_den);
                break;
            }

            last_open_ret = ret;
            av_log(nullptr, AV_LOG_WARNING,
                   "transcode: video encoder open failed codec=%s pix_fmt=%s size=%dx%d fps=%d/%d "
                   "ret=%d (%s)%s\n",
                   enc_name.c_str(), candidate_pf ? candidate_pf : "unknown", out_w, out_h,
                   fps_num, fps_den, ret, err_str(ret), candidate_i + 1 < candidates.size()
                   ? "; trying fallback" : "; no fallback available");
            avcodec_free_context(&enc);
        }
        if (!enc || !ecodec) {
            av_log(nullptr, AV_LOG_ERROR,
                   "transcode: no usable output video encoder for requested format (last_ret=%d, %s)\n",
                   last_open_ret, err_str(last_open_ret));
            result = 2;
            goto done;
        }

        // 输出视频流
        v_ost = avformat_new_stream(oc, nullptr);
        if (!v_ost) {
            result = AVERROR(ENOMEM);
            goto done;
        }
        ret = avcodec_parameters_from_context(v_ost->codecpar, enc);
        if (ret < 0) {
            result = ret;
            goto done;
        }
        v_ost->time_base = enc->time_base;
        v_ost->avg_frame_rate = (AVRational){fps_num, fps_den};

        // 输出色彩标签(契约 §3:cspace/cprim/ctrc 记入流 metadata,不做任何转换;
        // mp4/mov muxer 会据此写 colr box;不认识的标签解析成 UNSPECIFIED,等于没写)
        {
            const std::string cs = spec.get(SpecKeys::kColorSpace);
            const std::string cp = spec.get(SpecKeys::kColorPrimaries);
            const std::string ct = spec.get(SpecKeys::kColorTrc);
            if (!cs.empty()) v_ost->codecpar->color_space = (enum AVColorSpace)av_color_space_from_name(cs.c_str());
            if (!cp.empty()) v_ost->codecpar->color_primaries = (enum AVColorPrimaries)av_color_primaries_from_name(cp.c_str());
            if (!ct.empty()) v_ost->codecpar->color_trc = (enum AVColorTransferCharacteristic)av_color_transfer_from_name(ct.c_str());
        }
        if (zip_src) {
            if (dngmap == DngMap::APPLE_LOG) {
                // Apple Log 没有 H.273 transfer 枚举;保持 unknown,避免被 bt709 误读。
                v_ost->codecpar->color_space = AVCOL_SPC_BT2020_NCL;
                v_ost->codecpar->color_primaries = AVCOL_PRI_BT2020;
                v_ost->codecpar->color_trc = AVCOL_TRC_UNSPECIFIED;
            } else {
                // 契约 §10.3:LibRaw 出的是 16bit sRGB,输出按 bt709 primaries/trc 打标
                if (v_ost->codecpar->color_primaries == AVCOL_PRI_UNSPECIFIED)
                    v_ost->codecpar->color_primaries = AVCOL_PRI_BT709;
                if (v_ost->codecpar->color_trc == AVCOL_TRC_UNSPECIFIED)
                    v_ost->codecpar->color_trc = AVCOL_TRC_BT709;
                if (v_ost->codecpar->color_space == AVCOL_SPC_UNSPECIFIED)
                    v_ost->codecpar->color_space = AVCOL_SPC_BT709;
            }
        }
        // 容器级 title 元数据(契约 §3:可空)
        {
            const std::string ttl = spec.get(SpecKeys::kTitle);
            if (!ttl.empty()) av_dict_set(&oc->metadata, "title", ttl.c_str(), 0);
        }

        // ---------------- 音频 ----------------
        // 普通容器从 ic 取音频;DNG zip 从同一个 ZIP 解出的 WAV context 取音频。
        if (zip_src) {
            // astream 已在 zip 初始化阶段设置;audio_ic 可能因没有可用流被释放。
            if (audio_ic && astream >= 0) a_ist = audio_ic->streams[astream];
        } else {
            astream = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, vstream, nullptr, 0);
            if (astream >= 0) a_ist = ic->streams[astream];
        }
        if (astream >= 0 && a_ist && audio_mode != "drop") {
            enum AVCodecID aid = a_ist->codecpar->codec_id;

            if (audio_mode == "copy" && !container_takes_audio(oc->oformat, aid)) {
                audio_mode = "aac"; // 容器不匹配自动降级
            }

            // PCM 族不能 copy 进 MP4(sowt/in24/in32 等是 MOV tag,MP4 只认压缩音频)
            // N11-1:上界原来是 PCM_F16LE,漏掉同块的 F24LE/VIDC/SGA(0x10803–0x10805);
            // PCM 块占满 0x10000–0x10FFF,ADPCM_IMA_QT(0x11000)起是下一个块,用 <
            // 覆盖整个 PCM 块。
            if (audio_mode == "copy") {
                bool is_pcm = (aid >= AV_CODEC_ID_FIRST_AUDIO && aid < AV_CODEC_ID_ADPCM_IMA_QT);
                bool is_mp4 = (std::strcmp(oc->oformat->name, "mp4") == 0 || 
                              std::strcmp(oc->oformat->name, "ipod") == 0 ||
                              std::strcmp(oc->oformat->name, "m4v") == 0);
                if (is_pcm && is_mp4) {
                    av_log(nullptr, AV_LOG_INFO, 
                           "transcode: PCM audio cannot copy to MP4, auto fallback to AAC\n");
                    audio_mode = "aac";
                }
            }

            if (audio_mode == "aac" && !container_takes_audio(oc->oformat, AV_CODEC_ID_AAC)) {
                audio_mode = "drop";
            }

            if (audio_mode == "copy") {
                a_ost = avformat_new_stream(oc, nullptr);
                if (!a_ost) {
                    result = AVERROR(ENOMEM);
                    goto done;
                }
                ret = avcodec_parameters_copy(a_ost->codecpar, a_ist->codecpar);
                if (ret < 0) {
                    result = ret;
                    goto done;
                }
                if (zip_src) {
                    // WAV 的 codec_tag=1 是 RIFF/WAVE 格式码,不能原样带进
                    // MOV/MP4;清零让目标 muxer 按 codec_id 选择 sowt 等合法 tag。
                    a_ost->codecpar->codec_tag = 0;
                }
                a_ost->time_base = a_ist->time_base;
                // AAC ADTS → mp4/mov 需要 aac_adtstoasc
                if (aid == AV_CODEC_ID_AAC) {
                    const char *cn = fam->container;
                    if (std::strcmp(cn, "mp4") == 0 || std::strcmp(cn, "mov") == 0 ||
                        std::strcmp(cn, "ipod") == 0 || std::strcmp(cn, "m4v") == 0) {
                        const AVBitStreamFilter *f = av_bsf_get_by_name("aac_adtstoasc");
                        if (f) {
                            if (av_bsf_alloc(f, &bsf) == 0) {
                                if (avcodec_parameters_copy(bsf->par_in, a_ist->codecpar) == 0 &&
                                    av_bsf_init(bsf) == 0) {
                                    // ok
                                } else {
                                    av_bsf_free(&bsf);
                                    bsf = nullptr;
                                }
                            }
                        }
                    }
                }
                audio_copy = true;
            } else if (audio_mode == "aac") {
                const AVCodec *ac = avcodec_find_encoder(AV_CODEC_ID_AAC);
                const AVCodec *ad = avcodec_find_decoder(aid);
                if (ac && ad) {
                    adec = avcodec_alloc_context3(ad);
                    if (adec) {
                        ret = avcodec_parameters_to_context(adec, a_ist->codecpar);
                        if (ret == 0) ret = avcodec_open2(adec, ad, nullptr);
                        if (ret < 0) {
                            avcodec_free_context(&adec);
                        }
                    }
                }
                if (adec && ac) {
                    aenc = avcodec_alloc_context3(ac);
                    if (aenc) {
                        // 原生 AAC 编码器首选 fltp,显式挑出来比直接取 [0] 稳
                        aenc->sample_fmt = AV_SAMPLE_FMT_NONE;
                        for (int i = 0; ac->sample_fmts && ac->sample_fmts[i] != AV_SAMPLE_FMT_NONE; i++) {
                            if (ac->sample_fmts[i] == AV_SAMPLE_FMT_FLTP) {
                                aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
                                break;
                            }
                        }
                        // 没挑到 fltp:退回列表首项。若列表本身就是空的(理论上 AAC 一定
                        // 有,自定义 build 未必),用 NONE 而不是解引用空数组。
                        if (aenc->sample_fmt == AV_SAMPLE_FMT_NONE && ac->sample_fmts &&
                            ac->sample_fmts[0] != AV_SAMPLE_FMT_NONE) {
                            aenc->sample_fmt = ac->sample_fmts[0];
                        }
                        aenc->sample_rate = 48000;
                        av_channel_layout_default(&aenc->ch_layout, 2);
                        aenc->time_base = (AVRational){1, 48000};
                        aenc->bit_rate = 160000;
                        if (avcodec_open2(aenc, ac, nullptr) == 0) {
                            a_ost = avformat_new_stream(oc, nullptr);
                            if (a_ost &&
                                avcodec_parameters_from_context(a_ost->codecpar, aenc) == 0) {
                                a_ost->time_base = aenc->time_base;
                                swr = swr_alloc();
                                if (swr) {
                                    ret = av_opt_set_chlayout(swr, "in_chlayout", &adec->ch_layout, 0);
                                    ret |= av_opt_set_int(swr, "in_sample_rate", adec->sample_rate, 0);
                                    ret |= av_opt_set_sample_fmt(swr, "in_sample_fmt", adec->sample_fmt, 0);
                                    ret |= av_opt_set_chlayout(swr, "out_chlayout", &aenc->ch_layout, 0);
                                    ret |= av_opt_set_int(swr, "out_sample_rate", aenc->sample_rate, 0);
                                    ret |= av_opt_set_sample_fmt(swr, "out_sample_fmt", aenc->sample_fmt, 0);
                                    ret = swr_init(swr);
                                    if (ret < 0) {
                                        swr_free(&swr);
                                        audio_mode = "drop";
                                    }
                                }
                            } else {
                                audio_mode = "drop";
                            }
                            if (audio_mode != "drop") audio_aac = true;
                        } else {
                            avcodec_free_context(&aenc);
                            audio_mode = "drop";
                        }
                    } else {
                        audio_mode = "drop";
                    }
                }
                // 上面任一步不可用(找不到 aac 编码器/解码器、打开失败)就丢轨,
                // 别把半成品状态带进主循环
                if (audio_mode == "aac" && !audio_aac) audio_mode = "drop";
            }
        }

        // ---------------- 分配帧/包 + 建 filtergraph ----------------
        pkt = av_packet_alloc();
        dframe = av_frame_alloc();
        sframe = av_frame_alloc();
        apkt = av_packet_alloc();
        if (zip_src && audio_ic && astream >= 0) zip_audio_pkt = av_packet_alloc();
        if (audio_aac) {
            aframe_in = av_frame_alloc();
            aframe_out = av_frame_alloc();
        }
        if (!pkt || !dframe || !sframe || !apkt ||
            (zip_src && audio_ic && astream >= 0 && !zip_audio_pkt)) {
            result = AVERROR(ENOMEM);
            goto done;
        }
        if (audio_aac && (!aframe_in || !aframe_out)) {
            result = AVERROR(ENOMEM);
            goto done;
        }

        {
            const AVFilter *buffersrc = avfilter_get_by_name("buffer");
            const AVFilter *buffersink = avfilter_get_by_name("buffersink");
            if (!buffersrc || !buffersink) {
                result = AVERROR_UNKNOWN;
                goto done;
            }
            vp.g = avfilter_graph_alloc();
            if (!vp.g) {
                result = AVERROR(ENOMEM);
                goto done;
            }
            char bargs[512];
            if (zip_src) {
                // 契约 §10.3:LibRaw 输出 rgb48le,time_base=1/fps,sar=1(无 autorotate)
                std::snprintf(bargs, sizeof(bargs),
                              "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                              zip_w, zip_h, (int)AV_PIX_FMT_RGB48LE, fps_den, fps_num, 1, 1);
            } else {
                std::snprintf(bargs, sizeof(bargs),
                              "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                              dec->width, dec->height, (int)dec->pix_fmt, v_ist->time_base.num,
                              v_ist->time_base.den, dec->sample_aspect_ratio.num,
                              dec->sample_aspect_ratio.den);
            }
            ret = avfilter_graph_create_filter(&vp.fsrc, buffersrc, "in", bargs, nullptr, vp.g);
            if (ret < 0) {
                result = ret;
                goto done;
            }
            ret = avfilter_graph_create_filter(&vp.fsink, buffersink, "out", nullptr, nullptr, vp.g);
            if (ret < 0) {
                result = ret;
                goto done;
            }
            std::string descr = build_descr(spec, fps_num, fps_den, enc_fmt, rotation, need_scale,
                                            out_w, out_h);
            AVFilterInOut *inputs = avfilter_inout_alloc();
            AVFilterInOut *outputs = avfilter_inout_alloc();
            if (!inputs || !outputs) {
                if (inputs) avfilter_inout_free(&inputs);
                if (outputs) avfilter_inout_free(&outputs);
                result = AVERROR(ENOMEM);
                goto done;
            }
            inputs->name = av_strdup("out");
            inputs->filter_ctx = vp.fsink;
            inputs->pad_idx = 0;
            inputs->next = nullptr;
            outputs->name = av_strdup("in");
            outputs->filter_ctx = vp.fsrc;
            outputs->pad_idx = 0;
            outputs->next = nullptr;
            ret = avfilter_graph_parse_ptr(vp.g, descr.c_str(), &inputs, &outputs, nullptr);
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            if (ret < 0) {
                result = ret;
                goto done;
            }
            ret = avfilter_graph_config(vp.g, nullptr);
            if (ret < 0) {
                result = ret;
                goto done;
            }
            vp.sink_tb = av_buffersink_get_time_base(vp.fsink);
        }

        // 总帧数/总时长估算
        vp.oc = oc;
        vp.ic = ic;
        vp.dec = dec;
        vp.enc = enc;
        vp.vost = v_ost;
        vp.dframe = dframe;
        vp.sframe = sframe;
        vp.pkt = pkt;
        vp.duration_us = (ic && ic->duration > 0) ? ic->duration : 0;
        vp.fps_num = fps_num;
        vp.fps_den = fps_den;
        if (zip_src) {
            // 契约 §10.3:进度分母就是 zip 帧数;时长按 nb_frames/fps 折算
            vp.rep.total = (int64_t)dngzip.frame_count();
            vp.duration_us = av_rescale_q((int64_t)dngzip.frame_count(),
                                         (AVRational){fps_den, fps_num},
                                         (AVRational){1, AV_TIME_BASE});
        } else {
            int64_t nb = v_ist->nb_frames;
            if (nb <= 0 && ic->duration > 0) {
                double fps = (double)fps_num / (fps_den > 0 ? fps_den : 1);
                if (fps > 0) nb = (int64_t)((double)ic->duration / (double)AV_TIME_BASE * fps + 0.5);
            }
            vp.rep.total = nb > 0 ? nb : -1;
        }

        ap.oc = oc;
        ap.dec = adec;
        ap.enc = aenc;
        ap.swr = swr;
        ap.ost = a_ost;
        ap.ist = a_ist;
        ap.fin = aframe_in;
        ap.fout = aframe_out;
        ap.pkt = apkt;
        if (aenc) {
            ap.afifo = av_audio_fifo_alloc(aenc->sample_fmt, aenc->ch_layout.nb_channels,
                                           (aenc->frame_size > 0 ? aenc->frame_size : 1024) * 4);
            if (!ap.afifo) {
                result = AVERROR(ENOMEM);
                goto done;
            }
        }
    }

    // ---------------- 写文件头 ----------------
    mov_muxer = oc->oformat && oc->oformat->name &&
                (std::strstr(oc->oformat->name, "mov") != nullptr ||
                 std::strstr(oc->oformat->name, "mp4") != nullptr);
    if (zip_src && dngmap == DngMap::APPLE_LOG && mov_muxer) {
        // FFmpeg 6.1 只有 write_colr 才会在 transfer=unknown 时仍写出 nclc。
        av_dict_set(&mux_opts, "movflags", "+write_colr", 0);
    }
    ret = mux_opts ? avformat_write_header(oc, &mux_opts)
                   : avformat_write_header(oc, nullptr);
    av_dict_free(&mux_opts);
    if (ret < 0) {
        result = ret;
        goto done;
    }

    // ---------------- 主循环 ----------------
    {
        if (zip_src) {
            // DNG 视频解码和 ZIP 音频读取共享同一输出容器;按音频时间戳
            // 读取 WAV 包,沿用普通容器的 copy/AAC 写包逻辑。
            bool audio_reported = false;
            bool audio_eof = false;
            bool audio_packet_pending = false;
            AVPacket *audio_pkt = zip_audio_pkt;
            auto write_copy_packet = [&](AVPacket *packet) -> int {
                av_packet_rescale_ts(packet, a_ist->time_base, a_ost->time_base);
                packet->stream_index = a_ost->index;
                if (bsf) {
                    int r = av_bsf_send_packet(bsf, packet);
                    while (r == AVERROR(EAGAIN)) {
                        const int dr = drain_audio_bsf(bsf, oc, a_ost, apkt);
                        if (dr == AVERROR(EAGAIN)) {
                            r = av_bsf_send_packet(bsf, packet);
                            continue;
                        }
                        if (dr == AVERROR_EOF) return AVERROR_EOF;
                        return dr;
                    }
                    if (r < 0) return r;
                    r = drain_audio_bsf(bsf, oc, a_ost, apkt);
                    return (r == AVERROR(EAGAIN) || r == AVERROR_EOF) ? 0 : r;
                }
                return av_interleaved_write_frame(oc, packet);
            };
            auto pump_zip_audio = [&](int64_t video_pts, bool drain_all) -> int {
                if (!audio_ic || astream < 0 || audio_mode == "drop") return 0;
                AVPacket *read_pkt = audio_pkt;
                while (!audio_eof) {
                    int r = 0;
                    if (audio_packet_pending) {
                        audio_packet_pending = false;
                    } else {
                        r = av_read_frame(audio_ic, read_pkt);
                        if (r == AVERROR_EOF) {
                            audio_eof = true;
                            break;
                        }
                        if (r < 0) return r;
                    }
                    if (read_pkt->stream_index != astream) {
                        av_packet_unref(read_pkt);
                        continue;
                    }
                    int64_t pts = read_pkt->pts;
                    if (pts == AV_NOPTS_VALUE) pts = read_pkt->dts;
                    const int64_t audio_us = (pts == AV_NOPTS_VALUE)
                        ? 0 : av_rescale_q(pts, a_ist->time_base, (AVRational){1, AV_TIME_BASE});
                    const int64_t video_us = av_rescale_q(video_pts,
                        (AVRational){fps_den, fps_num}, (AVRational){1, AV_TIME_BASE});
                    if (pts != AV_NOPTS_VALUE && audio_us > video_us && !drain_all) {
                        audio_packet_pending = true;
                        break;
                    }
                    if (audio_copy) {
                        r = write_copy_packet(read_pkt);
                        av_packet_unref(read_pkt);
                        if (r < 0) return r;
                    } else if (audio_aac && adec) {
                        while (true) {
                            r = avcodec_send_packet(adec, read_pkt);
                            if (r != AVERROR(EAGAIN)) break;
                            const int pr = pump_audio_dec(&ap);
                            if (pr < 0) {
                                av_packet_unref(read_pkt);
                                return pr;
                            }
                        }
                        av_packet_unref(read_pkt);
                        if (r < 0 && r != AVERROR_EOF) return r;
                        r = pump_audio_dec(&ap);
                        if (r < 0) return r;
                    } else {
                        av_packet_unref(read_pkt);
                    }
                    if (!audio_reported) {
                        audio_reported = true;
                        vp.rep(-1, "audio", vp.frames);
                    }
                }
                return 0;
            };

            const size_t total_frames = dngzip.frame_count();
            int spec_dng_threads = (int)spec.get_l(SpecKeys::kDngThreads, 0LL);
            int spec_dng_depth = (int)spec.get_l(SpecKeys::kDngDepth, 0LL);
            if (spec_dng_threads < 0) spec_dng_threads = 0;
            if (spec_dng_depth < 0) spec_dng_depth = 0;
            const DngParallel par =
                dng_parallel_params(spec_dng_threads, spec_dng_depth, zip_w, zip_h);
            av_log(nullptr, AV_LOG_INFO,
                   "transcode: dngzip 并行解码 %zu 帧:%d 线程 / 预取深度 %d(源 %dx%d)",
                   total_frames, par.nthreads, par.depth, zip_w, zip_h);

            // 工作线程里的单帧解码:read_frame + LibRaw。
            // LibRaw 实例在 dng_decode_to_rgb48 内部 new 出来后又 recycle,各实例
            // 互相独立,所以整条链是线程安全的;队列只是「派帧号 + 收结果」。
            // raw/rgb 两个 vector 都用槽位里的(容量跨轮复用,不每帧新建)。
            // 尺寸不一致在这一步就拦掉(prores 需要恒定分辨率),错误经队列扩散到主线程。
            auto decode_fn = [&](size_t i, proresforge::DngFrame &out, std::string &e) -> int {
                if (dngzip.read_frame(i, out.raw, e) != 0) return -1;
                int fw = 0, fh = 0;
                if (dng_decode_to_rgb48(out.raw.data(), out.raw.size(), dngq, dngcm,
                                        out.rgb, fw, fh, e, nullptr, nullptr, dngmap,
                                        false) != 0) {
                    return -1;
                }
                if (fw != zip_w || fh != zip_h) {
                    e = "zip 第 " + std::to_string(i + 1) + " 帧尺寸与首帧不一致(" +
                        std::to_string(fw) + "x" + std::to_string(fh) + " vs " +
                        std::to_string(zip_w) + "x" + std::to_string(zip_h) + ")";
                    return -1;
                }
                out.w = fw;
                out.h = fh;
                return 0;
            };

            proresforge::DngFrameQueue pool;
            // init 段已解的 seq=0 首帧喂进队列(seed),worker 从 seq=1 起派,
            // 省掉整任务最贵一帧的重复解码(~0.4s/帧 @4096x3072 BALANCED)。
            // total_frames==1 时 next() 第一轮即可消费 seed 帧、worker 空转
            // 退出,不会卡(见 dngframeq.h seed() 注释)。
            if (have_seed) {
                if (!pool.seed(std::move(seed_frame))) {
                    av_log(nullptr, AV_LOG_WARNING,
                           "transcode: DNG 首帧 seed 被拒(已启动/重复 seed?),退回全量解码\n");
                }
            }
            // L-6:start() 现在会在「连 8MB 工作线程栈都保证不了」时硬失败。
            // 这种队列绝不能带着进主循环 —— 降级路径跑在主线程上,栈大小同样
            // 不受控,LibRaw 会在上面 SIGSEGV。按内部 IO 错误处理。
            if (!pool.start(total_frames, decode_fn, par.nthreads, (size_t)par.depth)) {
                av_log(nullptr, AV_LOG_ERROR, "transcode: DNG 解码队列启动失败: %s\n",
                       pool.last_error().c_str());
                result = AVERROR(EIO);
                goto done;
            }

            proresforge::DngFrame df;
#if PRF_AVFRAME_POOL
            // P1-3 第一步:AVFrame 小池(壳 + buffer 跨帧复用,所有权与安全判据
            // 见文件上方 AvFramePool 注释)。N = depth+2,4096x3072 下按预算封顶 3;
            // 帧数比 N 还少时按帧数收(与 DngFrameQueue 把 depth 钳到 total 同道理,
            // 别为 2 帧的片段常驻 3 份 72MB)。
            int frpool_n = avframe_pool_count(par.depth, zip_w, zip_h);
            if (frpool_n > (int)total_frames) frpool_n = (int)total_frames;
            if (frpool_n < 1) frpool_n = 1;
            proresforge::AvFramePool frpool;
            if (!frpool.start(frpool_n)) {
                pool.cancel();
                pool.stop();
                result = AVERROR(ENOMEM);
                goto done;
            }
#endif
            for (size_t i = 0; i < total_frames; i++) {
                if (g_cancel.load(std::memory_order_relaxed)) {
                    pool.cancel();
                    pool.stop();
                    result = 1;
                    goto done;
                }
                std::string e;
                int rc = pool.next(df, e);
                if (rc != proresforge::DngFrameQueue::kNextOk) {
                    // L-5:区分「取消」/「队列空(正常收尾)」/失败。队列在上面的
                    // 固定 i < total_frames 循环里不可能提前取空;万一出现,那是
                    // 派帧侧的 bug,不能当取消显给用户,按失败报。
                    if (rc == proresforge::DngFrameQueue::kNextCancelled) {
                        av_log(nullptr, AV_LOG_ERROR,
                               "transcode: DNG 序列解码被取消(第 %zu 帧): %s\n", i + 1, e.c_str());
                        pool.cancel();
                        pool.stop();
                        result = 1;
                        goto done;
                    }
                    if (rc == proresforge::DngFrameQueue::kNextDrained) {
                        av_log(nullptr, AV_LOG_ERROR,
                               "transcode: DNG 序列帧数不足(第 %zu 帧时队列已空): %s\n",
                               i + 1, e.c_str());
                        pool.cancel();
                        pool.stop();
                        result = kTranscodeInvalidInput;   // 不静默跳帧(契约 §10.3)
                        goto loop_error;
                    }
                    av_log(nullptr, AV_LOG_ERROR, "transcode: DNG 序列解码失败(第 %zu 帧): %s\n",
                           i + 1, e.c_str());
                    pool.cancel();
                    pool.stop();
                    result = kTranscodeInvalidInput;   // 不静默跳帧(契约 §10.3)
                    goto loop_error;
                }
#if PRF_AVFRAME_POOL
                // P1-3:从池里取壳。命中(count==1)时 data/linesize 直接沿用,
                // memcpy 写的是常驻页;未命中才 av_frame_get_buffer 重新分配。
                AVFrame *pf = nullptr;
                if (!frpool.acquire(zip_w, zip_h, (int64_t)i, &pf)) {
                    pool.cancel();
                    pool.stop();
                    result = AVERROR(ENOMEM);
                    goto loop_error;
                }
                for (int y = 0; y < zip_h; y++) {
                    uint8_t *dst = pf->data[0] + (size_t)y * (size_t)pf->linesize[0];
                    const uint16_t *src = df.rgb.data() + (size_t)y * (size_t)zip_w * 3;
                    memcpy(dst, (const void *)src, (size_t)zip_w * 3 * sizeof(uint16_t));
                }
                // keep=true: buffersrc 只 clone 引用,壳与 buffer 留在池里下一轮复用
                ret = push_frame(&vp, pf, true);
#else
                av_frame_unref(dframe);
                dframe->format = AV_PIX_FMT_RGB48LE;
                dframe->width = zip_w;
                dframe->height = zip_h;
                dframe->pts = (int64_t)i; // buffer args: time_base=1/fps
                ret = av_frame_get_buffer(dframe, 32);
                if (ret < 0) {
                    pool.cancel();
                    pool.stop();
                    result = ret;
                    goto loop_error;
                }
                for (int y = 0; y < zip_h; y++) {
                    uint8_t *dst = dframe->data[0] + (size_t)y * (size_t)dframe->linesize[0];
                    const uint16_t *src = df.rgb.data() + (size_t)y * (size_t)zip_w * 3;
                    memcpy(dst, (const void *)src, (size_t)zip_w * 3 * sizeof(uint16_t));
                }
                ret = push_decoded(&vp);
#endif
                if (ret < 0) {
                    pool.cancel();
                    pool.stop();
                    goto loop_error;
                }
                if (audio_ic && astream >= 0 && audio_mode != "drop") {
                    const int audio_ret = pump_zip_audio((int64_t)i, false);
                    if (audio_ret < 0) {
                        ret = audio_ret;
                        pool.cancel();
                        pool.stop();
                        goto loop_error;
                    }
                }
            }
            if (!audio_eof && audio_ic && astream >= 0 && audio_mode != "drop") {
                ret = pump_zip_audio((int64_t)total_frames, true);
                if (ret < 0) goto loop_error;
                if (audio_packet_pending) {
                    audio_packet_pending = false;
                }
            }
            if (audio_copy && bsf) {
                while (true) {
                    ret = av_bsf_send_packet(bsf, nullptr);
                    if (ret != AVERROR(EAGAIN)) break;
                    const int dr = drain_audio_bsf(bsf, oc, a_ost, apkt);
                    if (dr == AVERROR(EAGAIN)) continue;
                    if (dr == AVERROR_EOF) { ret = AVERROR_EOF; break; }
                    ret = dr;
                    goto loop_error;
                }
                if (ret < 0 && ret != AVERROR_EOF) goto loop_error;
                ret = drain_audio_bsf(bsf, oc, a_ost, apkt);
                if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                    goto loop_error;
            }
            if (audio_aac && adec) {

                while (true) {
                    ret = avcodec_send_packet(adec, nullptr);
                    if (ret != AVERROR(EAGAIN)) break;
                    ret = pump_audio_dec(&ap);
                    if (ret < 0) goto loop_error;
                }
                if (ret < 0 && ret != AVERROR_EOF) goto loop_error;
                ret = pump_audio_dec(&ap);
                if (ret < 0) goto loop_error;
            }
            pool.stop();   // 正常收尾:join 掉还在收尾的工作线程(此时队列应已空)
#if PRF_AVFRAME_POOL
            av_log(nullptr, AV_LOG_INFO,
                   "transcode: AVFrame 池 N=%zu(深度 %d):原地复用 %lld 帧 / 重新分配 %lld 帧",
                   frpool.size(), par.depth, (long long)frpool.hits,
                   (long long)frpool.misses);
            frpool.stop();
#endif
        } else {

        bool eof_seen = false;
        bool audio_reported = false;
        while (true) {
            if (g_cancel.load(std::memory_order_relaxed)) {
                result = 1;
                goto done;
            }
            ret = av_read_frame(ic, pkt);
            if (ret < 0) {
                // 只有真正的 EOF 才能进入解码器冲刷；I/O 错误、EAGAIN 等
                // 不能被误当成输入结束，否则会把截断/读失败输入报告成成功。
                if (ret != AVERROR_EOF) goto loop_error;
                if (eof_seen) break; // 第二次读到尾:结束
                eof_seen = true;
                // 冲刷解码器 → filtergraph(注意:滤镜图的 EOF 要等解码器缓帧
                // 全部推入后才能打,否则 push_decoded 会撞上 buffersrc 的 EOF → EINVAL)
                if (dec) {
                    while (true) {
                        ret = avcodec_send_packet(dec, nullptr);
                        if (ret != AVERROR(EAGAIN)) break;
                        const int pump_ret = pump_decoder(&vp);
                        if (pump_ret < 0) { ret = pump_ret; goto loop_error; }
                        // pump 成功只排空已有帧;NULL 仍未被接受,必须重送。
                    }
                    if (ret < 0 && ret != AVERROR_EOF) goto loop_error;
                }
                if (adec) {
                    while (true) {
                        ret = avcodec_send_packet(adec, nullptr);
                        if (ret != AVERROR(EAGAIN)) break;
                        const int pump_ret = pump_audio_dec(&ap);
                        if (pump_ret < 0) { ret = pump_ret; goto loop_error; }
                        // 同上:不要让 pump 的 0 覆盖 send 的 EAGAIN。
                    }
                    if (ret < 0 && ret != AVERROR_EOF) goto loop_error;
                }
                if (dec) {
                    ret = pump_decoder(&vp);
                    if (ret < 0) goto loop_error;
                }
                if (adec) {
                    ret = pump_audio_dec(&ap);
                    if (ret < 0) goto loop_error;
                }
                if (bsf) {
                    while (true) {
                        ret = av_bsf_send_packet(bsf, nullptr);
                        if (ret != AVERROR(EAGAIN)) break;
                        const int dr = drain_audio_bsf(bsf, oc, a_ost, apkt);
                        if (dr == AVERROR(EAGAIN)) continue;
                        if (dr == AVERROR_EOF) {
                            ret = AVERROR_EOF;
                            break;
                        }
                        ret = dr;
                        goto loop_error;
                    }
                    if (ret < 0 && ret != AVERROR_EOF) goto loop_error;
                    ret = drain_audio_bsf(bsf, oc, a_ost, apkt);
                    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                        goto loop_error;
                }
                continue;
            }

            int si = pkt->stream_index;
            if (vstream >= 0 && si == vstream) {
                // ---- 视频 ----
                while (true) {
                    ret = avcodec_send_packet(dec, pkt);
                    if (ret != AVERROR(EAGAIN)) break;
                    const int pump_ret = pump_decoder(&vp);
                    if (pump_ret < 0) {
                        ret = pump_ret;
                        av_packet_unref(pkt);
                        goto loop_error;
                    }
                }
                av_packet_unref(pkt);
                if (ret == AVERROR_EOF) continue;
                if (ret < 0) goto loop_error;
                ret = pump_decoder(&vp);
                if (ret < 0) goto loop_error;
            } else if (astream >= 0 && si == astream && audio_copy && a_ost) {
                // ---- 音频直通 ----
                av_packet_rescale_ts(pkt, a_ist->time_base, a_ost->time_base);
                pkt->stream_index = a_ost->index;
                if (bsf) {
                    ret = av_bsf_send_packet(bsf, pkt);
                    while (ret == AVERROR(EAGAIN)) {
                        const int dr = drain_audio_bsf(bsf, oc, a_ost, apkt);
                        if (dr == AVERROR(EAGAIN)) {
                            ret = av_bsf_send_packet(bsf, pkt);
                            continue;
                        }
                        if (dr == AVERROR_EOF) {
                            ret = AVERROR_EOF;
                            break;
                        }
                        av_packet_unref(pkt);
                        ret = dr;
                        goto loop_error;
                    }
                    av_packet_unref(pkt);
                    if (ret == AVERROR_EOF) continue;
                    if (ret < 0) goto loop_error;
                    ret = drain_audio_bsf(bsf, oc, a_ost, apkt);
                    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                        goto loop_error;
                } else {
                    ret = av_interleaved_write_frame(oc, pkt);
                    av_packet_unref(pkt);
                    if (ret < 0) goto loop_error;
                }
                if (!audio_reported) {
                    audio_reported = true;
                    vp.rep(-1, "audio", vp.frames);
                }
            } else if (astream >= 0 && si == astream && audio_aac && adec) {
                // ---- 音频转码 ----
                while (true) {
                    ret = avcodec_send_packet(adec, pkt);
                    if (ret != AVERROR(EAGAIN)) break;
                    const int pump_ret = pump_audio_dec(&ap);
                    if (pump_ret < 0) {
                        ret = pump_ret;
                        av_packet_unref(pkt);
                        goto loop_error;
                    }
                }
                av_packet_unref(pkt);
                if (ret == AVERROR_EOF) continue;
                if (ret < 0) goto loop_error;
                ret = pump_audio_dec(&ap);
                if (ret < 0) goto loop_error;
                if (!audio_reported) {
                    audio_reported = true;
                    vp.rep(-1, "audio", vp.frames);
                }
            } else {
                av_packet_unref(pkt);
            }
        }

        } // end if (zip_src) ... else 普通容器主循环

        // ---------------- flush 编码器 ----------------
        if (result == 0) vp.rep(100, "decode", vp.frames);
        // 滤镜图 EOF:解码器缓帧已全部推入,现在关掉输入并排空滤镜内部
        // 缓存的帧(fps/scale 等滤镜在 EOF 时才吐出尾帧)。重复 EOF 安全。
        if (vp.fsrc) {
            // EOF:返回值 <0 只是「输入已被关闭/已 EOF 过」,不是致命错误;
            // 真正要判断的是紧接着的 drain_graph(尾帧排空失败才算错)。
            ret = av_buffersrc_add_frame_flags(vp.fsrc, nullptr, 0);
            if (ret < 0 && ret != AVERROR_EOF) {
                result = ret;
                goto done;
            }
            ret = drain_graph(&vp);
            if (ret < 0) {
                result = ret;
                goto done;
            }
        }
        if (enc) {
            // send_frame(NULL) 遇 EAGAIN 时先排空已有包，再重送 NULL；否则编码器尾帧
            // 可能永远不会进入 flush 状态。
            // R10-5:两级 EAGAIN 各自吸收,但连续「send NULL→EAGAIN 且紧接着 receive
            // 也是 EAGAIN、一个包都没写出去」计一次零进展(kFlushStallLimit 轮封顶);
            // 触顶即真实负错误走 done —— 旧代码停在这个状态时 result 仍是 0,
            // Kotlin 侧看到「成功」,trailer 带着截断的音/视频流被判完成。
            int flush_stall = 0;
            while (true) {
                // 取消点已过主循环:flush 期间自己看 g_cancel,否则取消按不下去。
                if (g_cancel.load(std::memory_order_relaxed)) {
                    result = 1; // 与主循环取消点同一返回码(RC_CANCELLED)
                    goto done;
                }
                ret = avcodec_send_frame(enc, nullptr);
                if (ret != AVERROR(EAGAIN)) break;
                bool progressed = false; // 本轮是否真的写出过包
                while (true) {
                    ret = avcodec_receive_packet(enc, pkt);
                    if (ret == AVERROR(EAGAIN)) break;
                    if (ret == AVERROR_EOF) break;
                    if (ret < 0) {
                        av_packet_unref(pkt);
                        result = ret;
                        goto done;
                    }
                    av_packet_rescale_ts(pkt, enc->time_base, v_ost->time_base);
                    pkt->stream_index = v_ost->index;
                    ret = av_interleaved_write_frame(oc, pkt);
                    av_packet_unref(pkt);
                    if (ret < 0) { result = ret; goto done; }
                    progressed = true;
                }
                if (ret == AVERROR_EOF) break;
                if (!progressed) {
                    if (++flush_stall > kFlushStallLimit) {
                        result = AVERROR(EAGAIN); // 真实负错误,不是「成功」
                        goto done;
                    }
                } else {
                    flush_stall = 0; // 有产出:抖动重新计数
                }
            }
            if (ret < 0 && ret != AVERROR_EOF) {
                result = ret;
                goto loop_error;
            }
            while (true) {
                ret = avcodec_receive_packet(enc, pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                // 非 EAGAIN/EOF 的接收错误/写盘失败都必须让 result 变成非 0:
                // 旧代码在这里 break 后 result 仍是 0,Kotlin 侧看到「成功」,
                // 于是尾帧丢失但通知却弹「完成」。
                if (ret < 0) {
                    av_packet_unref(pkt);
                    result = ret;
                    goto done;
                }
                av_packet_rescale_ts(pkt, enc->time_base, v_ost->time_base);
                pkt->stream_index = v_ost->index;
                ret = av_interleaved_write_frame(oc, pkt);
                av_packet_unref(pkt);
                if (ret < 0) {
                    result = ret;
                    goto done;
                }
            }
        }
        if (audio_aac && aenc) {
            // R10-5:与视频 flush 同形 —— 连续两级 EAGAIN 零进展封顶,取消自己看
            // g_cancel(主循环的取消点已经走完)。
            int flush_stall = 0;
            while (true) {
                if (g_cancel.load(std::memory_order_relaxed)) {
                    result = 1; // 与主循环取消点同一返回码(RC_CANCELLED)
                    goto done;
                }
                ret = avcodec_send_frame(aenc, nullptr);
                if (ret != AVERROR(EAGAIN)) break;
                bool progressed = false; // 本轮是否真的写出过包
                while (true) {
                    ret = avcodec_receive_packet(aenc, apkt);
                    if (ret == AVERROR(EAGAIN)) break;
                    if (ret == AVERROR_EOF) break;
                    if (ret < 0) {
                        av_packet_unref(apkt);
                        result = ret;
                        goto done;
                    }
                    av_packet_rescale_ts(apkt, aenc->time_base, a_ost->time_base);
                    apkt->stream_index = a_ost->index;
                    ret = av_interleaved_write_frame(oc, apkt);
                    av_packet_unref(apkt);
                    if (ret < 0) { result = ret; goto done; }
                    progressed = true;
                }
                if (ret == AVERROR_EOF) break;
                if (!progressed) {
                    if (++flush_stall > kFlushStallLimit) {
                        result = AVERROR(EAGAIN); // 真实负错误,不是「成功」
                        goto done;
                    }
                } else {
                    flush_stall = 0; // 有产出:抖动重新计数
                }
            }
            if (ret < 0 && ret != AVERROR_EOF) {
                result = ret;
                goto loop_error;
            }
            while (true) {
                ret = avcodec_receive_packet(aenc, apkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) {
                    av_packet_unref(apkt);
                    result = ret;
                    goto done;
                }
                av_packet_rescale_ts(apkt, aenc->time_base, a_ost->time_base);
                apkt->stream_index = a_ost->index;
                ret = av_interleaved_write_frame(oc, apkt);
                av_packet_unref(apkt);
                if (ret < 0) {
                    result = ret;
                    goto done;
                }
            }
        }
        if (result == 0) vp.rep(100, "audio", vp.frames);
    }

    // ---------------- 写文件尾 + faststart ----------------
    if (!oc) {
        result = AVERROR(EINVAL);
        goto done;
    }
    ret = av_write_trailer(oc);
    trailer_written = true;   // 已调用(仅此一次;faststart 只在此之后进行)
    if (ret < 0) {
        result = ret;
        goto done;
    }
    // 自定义 AVIO 的残留 buffer 必须手动 flush(avformat 不会替我们 flush)
    if (out_pb) avio_flush(out_pb);
    vp.rep(0, "finalize", vp.frames);

    // 释放输出上下文(自定义 pb 不随之关闭,下面显式释放)
    avformat_free_context(oc);
    oc = nullptr;
    fd_io_free_avio(&out_pb, &out_buf);

    if (faststart > 0 && fam && trailer_written) {
        const char *cn = fam->container;
        if (cn && (std::strcmp(cn, "mp4") == 0 || std::strcmp(cn, "mov") == 0 ||
                   std::strcmp(cn, "ipod") == 0 || std::strcmp(cn, "m4v") == 0)) {
            // P1-03:faststart 失败必须可观测、可判定,不能伪装成已完成。
            // faststart_move_moov 内部已在「搬移后重新扫描顶层 box」自检
            // ([ftyp][moov]、偏移/尺寸/总长度),这里的职责是把任何失败升级为
            // 任务失败:AV_LOG_ERROR 带阶段/errno/输出长度/失败点,并把 result
            // 置非 0 —— 由下面 done 的既有错误清理 vp.rep(-1, "error") 上报,
            // Kotlin 侧删除输出文件,最终不留下可被误认为成品的半搬移文件。
            // 不补写 trailer(已写过且仅此一次),不改既有清理流程。
            FaststartFail fsf;
            if (!faststart_move_moov(&out_ctx, &fsf)) {
                char ebuf[80];
                if (fsf.err != 0)
                    snprintf(ebuf, sizeof(ebuf), "%d(%s)", fsf.err, std::strerror(fsf.err));
                else
                    snprintf(ebuf, sizeof(ebuf), "n/a");
                av_log(nullptr, AV_LOG_ERROR,
                       "transcode: faststart 失败(stage=moov-move, point=%s, errno=%s, "
                       "out_len=%lld, out_ctx.size=%lld):按失败收尾,不伪装完成\n",
                       fsf.point ? fsf.point : "unknown", ebuf,
                       (long long)out_fd_len(out_ctx.fd), (long long)out_ctx.size);
                if (result == 0) result = AVERROR(EIO);
            }
        }
    }

    if (result == 0) vp.rep(100, "done", vp.frames);
    goto done;

loop_error:
    // 主循环出错:尽量把尾写完,让输出尽量可播,再返回错误码。
    // (旧代码在这里置 loop_error,但变量声明已被删——编译器抓到的漏网点,正好删掉。)
    if (result == 0) result = ret;
    if (enc) {
        ret = avcodec_send_frame(enc, nullptr);
        if (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            while (true) {
                ret = avcodec_receive_packet(enc, pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) {
                    av_packet_unref(pkt);
                    if (result == 0) result = ret;
                    break;
                }
                av_packet_rescale_ts(pkt, enc->time_base, v_ost->time_base);
                pkt->stream_index = v_ost->index;
                ret = av_interleaved_write_frame(oc, pkt);
                av_packet_unref(pkt);
                if (ret < 0) break;
            }
        } else if (result == 0) {
            result = ret;
        }
    }
    if (audio_aac && aenc) {
        ret = avcodec_send_frame(aenc, nullptr);
        if (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            while (true) {
                ret = avcodec_receive_packet(aenc, apkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) {
                    av_packet_unref(apkt);
                    if (result == 0) result = ret;
                    break;
                }
                av_packet_rescale_ts(apkt, aenc->time_base, a_ost->time_base);
                apkt->stream_index = a_ost->index;
                ret = av_interleaved_write_frame(oc, apkt);
                av_packet_unref(apkt);
                if (ret < 0) break;
            }
        } else if (result == 0) {
            result = ret;
        }
    }
    if (oc) {
        if (av_write_trailer(oc) == 0 && out_pb) {
            avio_flush(out_pb);
        }
        // 错误收尾路径也(且仅)在这里调一次 trailer;打标记后 faststart 才允许跑,
        // 调用顺序本身保证 av_write_trailer 绝不会因 faststart 被重复调用。
        trailer_written = true;
        avformat_free_context(oc);
        oc = nullptr;
        fd_io_free_avio(&out_pb, &out_buf);
        // 错误收尾路径的 faststart:保留 best-effort 尝试(此时 trailer 已写过,
        // 上面 trailer_written 为真),让错误输出也尽量可播;但铁律:
        //   * 绝不重复 av_write_trailer(moov 只能来自这唯一一次 trailer);
        //   * 只写 AV_LOG_WARNING(同样带 errno/输出长度/阶段),不改 result
        //     —— 原始错误是任务结果,绝不被 faststart 覆盖;
        //   * 不产生二次异常(纯日志 + 局部栈变量,无分配无抛出)。
        if (faststart > 0 && fam && trailer_written) {
            const char *cn = fam->container;
            if (cn && (std::strcmp(cn, "mp4") == 0 || std::strcmp(cn, "mov") == 0 ||
                       std::strcmp(cn, "ipod") == 0 || std::strcmp(cn, "m4v") == 0)) {
                FaststartFail fsf;
                if (!faststart_move_moov(&out_ctx, &fsf)) {
                    char ebuf[80];
                    if (fsf.err != 0)
                        snprintf(ebuf, sizeof(ebuf), "%d(%s)", fsf.err, std::strerror(fsf.err));
                    else
                        snprintf(ebuf, sizeof(ebuf), "n/a");
                    av_log(nullptr, AV_LOG_WARNING,
                           "transcode: faststart 失败(stage=moov-move, point=%s, errno=%s, "
                           "out_len=%lld, out_ctx.size=%lld):错误收尾路径仅告警,原始错误不变\n",
                           fsf.point ? fsf.point : "unknown", ebuf,
                           (long long)out_fd_len(out_ctx.fd), (long long)out_ctx.size);
                }
            }
        }
    }

done:
    // ---------------- 统一清理 ----------------
    // fd 是 Kotlin detachFd() 移交过来的所有权(契约 §2),native 用完必须 close;
    // 此处是正常路径唯一出口:close 后置 -1(函数入口的 FdGuard 随之变 no-op,
    // 异常出口由守卫兜底),绝不 double-close。
    if (fd_in_owned >= 0) { close(fd_in_owned); fd_in_owned = -1; }
    if (fd_out_owned >= 0) { close(fd_out_owned); fd_out_owned = -1; }
    if (oc) avformat_free_context(oc);
    fd_io_free_avio(&out_pb, &out_buf);
    fd_io_free_input(&ic, &in_ctx, &in_pb, &in_buf);
    if (audio_ic && audio_ic != ic) {
        mem_io_free_input(&audio_ic, &audio_in_ctx, &audio_in_pb, &audio_in_buf);
    }
    dng_audio_buf.clear();
    if (vp.g) avfilter_graph_free(&vp.g);
    if (enc) avcodec_free_context(&enc);
    if (dec) avcodec_free_context(&dec);
    if (aenc) avcodec_free_context(&aenc);
    if (adec) avcodec_free_context(&adec);
    if (swr) swr_free(&swr);
    if (bsf) av_bsf_free(&bsf);
    if (pkt) av_packet_free(&pkt);
    if (apkt) av_packet_free(&apkt);
    if (zip_audio_pkt) av_packet_free(&zip_audio_pkt);
    if (dframe) av_frame_free(&dframe);
    if (sframe) av_frame_free(&sframe);
    if (aframe_in) av_frame_free(&aframe_in);
    if (aframe_out) av_frame_free(&aframe_out);
    if (ap.afifo) av_audio_fifo_free(ap.afifo);

    if (result != 0 && result != 1) {
        vp.rep(-1, "error", vp.frames);
    }
    if (owns_lifecycle) end_transcode();
    return result;
}

} // namespace proresforge
