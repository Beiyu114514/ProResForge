// transcode_harness.cpp —— 复现 run_transcode 完整转码路径(诊断用,不进 APK)
//
// 用法: transcode_harness <in> <out> [key=value ...]
//   默认 spec 用 prores_ks(x86 原生编码器,无外部库依赖),可用 key=value 覆盖:
//     transcode_harness in.mp4 out.mov audio=aac vcodec=prores_ks
//   返回码与 nativeRun 一致:0 成功 / 2 不支持 / <0 = -AVERROR。
#include "transcode.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <libavutil/log.h>
}

static void prog(int pct, const char *stage, int64_t frame, int64_t total, void *)
{
    fprintf(stderr, "[%3d%%] %-8s frame=%lld/%lld\n", pct, stage,
            (long long)frame, (long long)total);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <in> <out> [k=v ...]\n", argv[0]); return 2; }
    av_log_set_level(AV_LOG_INFO);

    int fdi = open(argv[1], O_RDONLY);
    if (fdi < 0) { perror("open in"); return 1; }
    int fdo = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fdo < 0) { perror("open out"); return 1; }

    // 默认 spec:与 FFmpeg.buildSpec(PRORES, HIGH, ORIG, ORIG, aac) 等价
    std::string spec =
        "vcodec=prores_ks\n"
        "pixfmt=yuv422p10le\n"
        "w=0\nh=0\n"
        "fpsnum=0\nfpsden=1\n"
        "rc=bitrate\ncrf=0\nbitrate=100000000\n"
        "gop=2\n"
        "scale=lanczos\n"
        "rotate=auto\n"
        "audio=aac\n"
        "faststart=1\n";

    // 命令行 k=v 覆盖(整行替换或追加)
    for (int i = 3; i < argc; i++) {
        std::string kv = argv[i];
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = kv.substr(0, eq), val = kv.substr(eq + 1);
        std::string out;
        size_t pos = 0;
        bool replaced = false;
        while (pos < spec.size()) {
            const size_t nl = spec.find('\n', pos);
            const std::string line = spec.substr(pos, nl - pos);
            const size_t leq = line.find('=');
            if (leq != std::string::npos && line.substr(0, leq) == key) {
                out += key + "=" + val + "\n";
                replaced = true;
            } else {
                out += line + "\n";
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        if (!replaced) out += key + "=" + val + "\n";
        spec = out;
    }
    fprintf(stderr, "---- spec ----\n%s--------------\n", spec.c_str());

    proresforge::TranscodeParams p;
    p.fd_in = fdi;
    p.length = (int64_t)lseek(fdi, 0, SEEK_END);
    p.fd_out = fdo;
    p.spec_text = spec;
    p.on_progress = prog;
    p.user = nullptr;

    const int rc = proresforge::run_transcode(p);
    fprintf(stderr, "run_transcode rc=%d\n", rc);
    close(fdi);
    close(fdo);
    return rc;
}
