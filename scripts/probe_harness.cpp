// probe_harness.cpp —— 复现 MediaProbe/nativeInfo 的完整探测路径并计时(诊断用,不进 APK)
//
// 用法: probe_harness <file> [--trace]
//   复刻 collect_media_info:fd → fd_io_open_input → avformat_find_stream_info
//   → av_find_best_stream/rotation 等,逐阶段打印耗时;--trace 打开 ffmpeg debug 日志。
#include "transcode.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
}

using namespace proresforge;
using clk = std::chrono::steady_clock;
static double since(clk::time_point t0)
{
    return std::chrono::duration<double>(clk::now() - t0).count();
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <file> [--trace]\n", argv[0]); return 2; }
    const bool trace = (argc > 2 && std::strcmp(argv[2], "--trace") == 0);
    av_log_set_level(trace ? AV_LOG_DEBUG : AV_LOG_INFO);
    avformat_network_init();

    const char *path = argv[1];
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    const off_t len = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    printf("file=%s size=%lld\n", path, (long long)len);
    fflush(stdout);

    auto t0 = clk::now();
    AVFormatContext *ic = nullptr;
    FdIoCtx *ctx = nullptr;
    AVIOContext *pb = nullptr;
    uint8_t *buf = nullptr;
    int r = fd_io_open_input(fd, (int64_t)len, &ic, &ctx, &pb, &buf);
    printf("[%8.3f] fd_io_open_input rc=%d\n", since(t0), r);
    fflush(stdout);
    if (r < 0) { fd_io_free_input(&ic, &ctx, &pb, &buf); close(fd); return 1; }

    r = avformat_find_stream_info(ic, nullptr);
    printf("[%8.3f] find_stream_info rc=%d (%s)\n", since(t0), r, err_str(r));
    fflush(stdout);
    if (r < 0) { fd_io_free_input(&ic, &ctx, &pb, &buf); close(fd); return 1; }

    const int vi = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    printf("[%8.3f] best video stream=%d\n", since(t0), vi);
    if (vi >= 0) {
        AVStream *st = ic->streams[vi];
        const AVCodecParameters *cp = st->codecpar;
        printf("    codec=%s %dx%d pixfmt=%s profile=%d fps=%d/%d nb_frames=%lld rot=%d\n",
               avcodec_get_name(cp->codec_id), cp->width, cp->height,
               av_get_pix_fmt_name((enum AVPixelFormat)cp->format), cp->profile,
               st->avg_frame_rate.num, st->avg_frame_rate.den, (long long)st->nb_frames,
               detect_rotation_deg(st, "auto"));
        printf("    cspace=%s cprim=%s ctrc=%s\n",
               av_color_space_name(cp->color_space), av_color_primaries_name(cp->color_primaries),
               av_color_transfer_name(cp->color_trc));
    }
    const int ai = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, vi, nullptr, 0);
    printf("[%8.3f] best audio stream=%d duration=%lldus bitrate=%lld\n",
           since(t0), ai, (long long)ic->duration, (long long)ic->bit_rate);
    fflush(stdout);

    fd_io_free_input(&ic, &ctx, &pb, &buf);
    close(fd);
    printf("[%8.3f] done\n", since(t0));
    return 0;
}
