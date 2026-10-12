// transcode.h
//
// 转码模块对外契约:参数结构体、进度回调、取消标志、fd IO 助手。
// 实现见 transcode.cpp;fd IO 助手同时被 preview.cpp 复用。
//
// 约定(与契约 INTERFACES.md 一致):
//   * fd = Kotlin contentResolver.openFileDescriptor(...).detachFd() 得到的文件描述符,
//     native 侧"拥有"它:内部 pread/pwrite 自己维护偏移,用完 close(fd)。
//   * 输入/输出都走自定义 AVIOContext(avio_alloc_context),read/seek/write 三个回调齐全;
//     seek 必须处理 AVSEEK_SIZE(输入返回文件长度,输出返回已写字节数)。
//   * run_transcode 返回:0 成功,1 用户取消,2 输出格式/编码器不支持,
//     3 输入文件损坏或无法解码,<0 = -AVERROR。
#ifndef PRORESFORGE_TRANSCODE_H
#define PRORESFORGE_TRANSCODE_H

#include "strspec.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unistd.h>   // close(fd),FdGuard 用

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
}

namespace proresforge {

// ---------------------------------------------------------------------------
// fd RAII 守卫(由 preview.cpp 的 FdReleaser 抽到共享头;collect_media_info 与
// run_transcode 的异常路径同样需要,见 native-lib.cpp / transcode.cpp):
//   fd = 指向「本函数持有的 fd」的指针;析构时 close 并置 -1。
//   用法:正常路径显式 close(*fd) 并置 -1(守卫随之变 no-op);goto/return 之外
//   的异常出口(bad_alloc 等跳过清理代码)由析构兜底。两者互斥,绝不
//   double-close。
//   典型形态:
//     int fd_owned = fd;
//     FdGuard fd_guard{ &fd_owned };
//     ...  // 正常出口:if (fd_owned >= 0) { close(fd_owned); fd_owned = -1; }
// ---------------------------------------------------------------------------
struct FdGuard {
    int *fd;
    ~FdGuard() { if (fd && *fd >= 0) { close(*fd); *fd = -1; } }
};

// Stable return code for a recognized container whose video payload cannot be decoded.
// Keep this separate from 2, which means the requested output is unsupported.
constexpr int kTranscodeInvalidInput = 3;

// ---------------------------------------------------------------------------
// 取消标志
// nativeCancel 置位;转码主循环(av_read_frame / avcodec_receive_packet 之间)轮询。
// 取消是"尽快退出":收到后直接跳统一清理,不写 trailer;残缺输出由 Kotlin 负责删除
// (C 端不能 unlink outFd)。
// ---------------------------------------------------------------------------
extern std::atomic<bool> g_cancel;
// 建立/结束一次 nativeRun 的生命周期边界。返回 true 表示当前调用建立了
// 边界并负责配对 end_transcode();JNI 入口与直接 run_transcode 调用都可使用。
bool begin_transcode();
void end_transcode();
void request_cancel();
// 作废尚未被 begin_transcode 消费的 pending 取消(转码未起时有效)。
void clear_cancel_pending();
bool is_cancelled();

// ---------------------------------------------------------------------------
// 进度回调
//   pct        0..100;-1 表示该 stage 没有百分比可报
//   stage      契约 §5 STAGES:init/decode/audio/finalize/done/error
//   frame      已完成帧数(该 stage 可报帧;否则 0)
//   total_frames 总帧数(未知为 -1)
// 回调在转码工作线程被调用,实现必须短(直接 CallVoidMethod,不做 IO)。
// ---------------------------------------------------------------------------
typedef void (*ProgressFn)(int pct, const char *stage, int64_t frame, int64_t total_frames, void *user);

struct TranscodeParams {
    int fd_in = -1;          // 输入 fd(native 拥有)
    int64_t length = 0;      // 输入长度(Kotlin 已知;<=0 时用 lseek 兜底)
    int fd_out = -1;         // 输出 fd(native 拥有,Kotlin 已截断为 0)
    std::string spec_text;   // spec k=v\n 原文
    ProgressFn on_progress = nullptr;
    void *user = nullptr;    // 传给 on_progress(JNI 全局引用对象)
};

// 执行一次转码。成功返回 0;1 取消;2 输出格式/编码器不支持;3 输入损坏或无法解码;
// <0 = -AVERROR。
int run_transcode(const TranscodeParams &params);

// ---------------------------------------------------------------------------
// fd IO(Kotlin detachFd 得到的文件描述符)
// 输入/输出共用:把 fd + 自维护偏移包成 AVIOContext。
//   size 的含义:输入 = 文件总长度(AVSEEK_SIZE 时返回);输出 = 已写字节数。
// ---------------------------------------------------------------------------
struct FdIoCtx {
    int fd = -1;
    int64_t pos = 0;   // 当前读写偏移(pread/pwrite 的位置参数)
    int64_t size = 0;  // 输入:总长度;输出:已写字节数
};

// 三个 AVIO 回调(n6.1 的 avio_alloc_context write_packet 参数是 uint8_t*,非 const;
// const 化是 n7.0 的事,别提前改,否则 avio_alloc_context 匹配不上)
int fd_io_read(void *opaque, uint8_t *buf, int buf_size);
int fd_io_write(void *opaque, uint8_t *buf, int buf_size);
int64_t fd_io_seek(void *opaque, int64_t offset, int whence);

// 用 FdIoCtx 创建一个 AVIOContext;buffer 由调用者释放(见 fd_io_free)。
// for_write=false → read 回调;true → write 回调。失败返回 nullptr。
AVIOContext *fd_io_new_avio(FdIoCtx *ctx, int buffer_size, bool for_write, uint8_t **out_buf);

// 释放 AVIOContext 及其 buffer。
// 注意:libavformat 在探测/seekback 时可能 av_free 掉创建时传入的 buffer 并换新
// (ffio_set_buf_size),因此释放的是 pb->buffer(当前 buffer);*buf 仅为
// "pb 未建成时的兜底",pb 存在时会被置空、绝不单独释放(否则双free)。
void fd_io_free_avio(AVIOContext **pb, uint8_t **buf);

// 打开输入:alloc context、挂自定义 pb、avformat_open_input(url="")、find_stream_info。
// 成功返回 0 并把四样东西都写回给调用者(由 fd_io_free_input 统一释放)。
// fd 不在此关闭。
int fd_io_open_input(int fd, int64_t length,
                     AVFormatContext **out_ic, FdIoCtx **out_ctx,
                     AVIOContext **out_pb, uint8_t **out_buf);

// 释放输入(ic 用 avformat_close_input;自定义 pb 不被自动关闭,必须显式释放)。
// 一直到达 avformat_open_input 失败,也应调用(此时 ic 已被其内部置 NULL)。
void fd_io_free_input(AVFormatContext **ic, FdIoCtx **ctx, AVIOContext **pb, uint8_t **buf);

// ---------------------------------------------------------------------------
// 内存输入(用于 ZIP 内嵌 audio.wav)
// ---------------------------------------------------------------------------
// data 不由 MemIoCtx 拥有,调用方必须保证其在 AVFormatContext 释放前保持有效。
struct MemIoCtx {
    const uint8_t *data = nullptr;
    int64_t size = 0;
    int64_t pos = 0;
};

int mem_io_read(void *opaque, uint8_t *buf, int buf_size);
int64_t mem_io_seek(void *opaque, int64_t offset, int whence);
AVIOContext *mem_io_new_avio(MemIoCtx *ctx, int buffer_size, uint8_t **out_buf);
int mem_io_open_input(const uint8_t *data, int64_t length,
                      AVFormatContext **out_ic, MemIoCtx **out_ctx,
                      AVIOContext **out_pb, uint8_t **out_buf);
void mem_io_free_input(AVFormatContext **ic, MemIoCtx **ctx,
                       AVIOContext **pb, uint8_t **buf);

// ---------------------------------------------------------------------------
// 通用小工具(transcode.cpp / preview.cpp 共用)
// ---------------------------------------------------------------------------

// 源旋转角度(度,0/90/180/270)。explicit_rot:auto=识别 display matrix,否则用绝对值。
int detect_rotation_deg(AVStream *st, const std::string &rotate_spec);

// 给定像素格式字符串→AVPixelFormat,未知返回 AV_PIX_FMT_NONE。
enum AVPixelFormat parse_pix_fmt(const std::string &s);

// 生成 UTF-8 的简短错误描述(内置静态缓冲,可立即 printf/日志)
const char *err_str(int errnum);

} // namespace proresforge

#endif // PRORESFORGE_TRANSCODE_H
