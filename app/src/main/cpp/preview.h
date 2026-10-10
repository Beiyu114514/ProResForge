// preview.h —— 单帧预览(抽取 + 编码成 MJPEG)
//
// 契约 §2 nativePreview:
//   从 fd(length 字节的可 seek 文件)的 atUs 微秒处抽一帧视频,
//   可选套 LUT 调色,再按 maxW 缩放后编码成 JPEG(quality 85)返回字节数组;失败返回空数组。
//   (跨语言契约:JNI 层把空 vector 转成 null,Kotlin 侧 ByteArray? 判 null 属正常路径。)
//
// 本头文件只被 preview.cpp / native-lib.cpp 使用。

#ifndef PRORESFORGE_PREVIEW_H
#define PRORESFORGE_PREVIEW_H

#include <cstdint>
#include <vector>

namespace proresforge {

// 抽取单帧预览 JPEG;返回 JPEG 字节,失败返回空 vector。
//   fd/length   : Kotlin detachFd() 得到的输入文件(契约 §2,内部 close)
//   at_us       : 目标时间点(微秒);用 av_seek_frame(AVSEEK_FLAG_BACKWARD) 定位
//   lut_path    : .cube LUT 绝对路径;nullptr/空串 表示不调色
//   lut_intensity : LUT 强度 0..1;>=0.999 直接套用,否则 split+blend 与原帧混合
//   graded      : false 时完全不过 LUT(查看原片)
//   max_w       : 输出最大宽度,<=0 表示维持原始尺寸
//   dng_quality / dng_color / dng_map : DNG 序列 zip 的解码档位,与转码 spec 的
//                dngq/dngcolor/dngmap 同枚举值;越界时退回 FAST/CAMERA/NONE。
//                预览与转码必须同参数,否则用户看到的画面和成品不一致(N-7)。
std::vector<uint8_t> render_preview_jpeg(int fd, int64_t length, int64_t at_us,
                                         const char *lut_path, double lut_intensity,
                                         bool graded, int max_w,
                                         int dng_quality, int dng_color, int dng_map);

} // namespace proresforge

#endif // PRORESFORGE_PREVIEW_H
