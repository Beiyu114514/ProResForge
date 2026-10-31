// dngzip.h
//
// DNG 序列 zip(MotionCam)→ 解码单帧 DNG 的 native 侧实现,契约 INTERFACES.md §10。
//
//   输入是一个 SAF fd 指向的 .zip,里面是 ~900 个 frame-NNNNN.dng。
//   整条链不落临时文件、不解压到 cacheDir、不 mmap 整个 zip:
//     DngZip::open        解析中央目录(含 ZIP64),按帧号自然序建条目表(指向 fd,零拷贝)
//     DngZip::read_frame  单帧 inflate 到内存
//     dng_decode_to_rgb48 LibRaw open_buffer/unpack/dcraw_process → 16bit RGB rgb48le
//
// 为什么不用 FFmpeg 6.1 的 tiff.c:它只能给 CFA Bayer 帧,没有解马赛克/相机 WB/
// 色彩矩阵/DNG opcodes,且 DNG tiled 路径是 AVERROR_PATCHWELCOME(契约 §10.1)。

#ifndef PRORESFORGE_DNGZIP_H
#define PRORESFORGE_DNGZIP_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace proresforge {

// 中央目录里的一条 DNG 帧条目。
//   method: 0 = store,8 = deflate
//   csize / usize: 压缩前后字节数(ZIP64 下也是真实字节数)
//   lho: 本地文件头在 zip 里的绝对偏移(数据起点要自己按本地头算,见 dngzip.cpp)
//   crc: 中央目录的 CRC-32(read_frame 读取后校验,R12-6)
struct DngZipEntry {
    std::string name;
    uint16_t method = 0;
    uint64_t csize = 0;
    uint64_t usize = 0;
    uint64_t lho = 0;
    uint32_t crc = 0;
};

class DngZip {
public:
    DngZip() = default;
    ~DngZip();

    DngZip(const DngZip &) = delete;
    DngZip &operator=(const DngZip &) = delete;

    /** 头 4 字节是 ZIP 魔数 PK\x03\x04 时为真(只判入口,不解条目)。 */
    static bool is_zip(const uint8_t *head, size_t n);

    /**
     * 解析 zip 中央目录,收集所有 DNG 帧条目并按帧号自然序
     * (frame-00002.dng 在 frame-00010.dng 之前)排序。
     * 条目名规则见 dngzip.cpp 的 is_dng_frame_name():优先
     * ^frame[_-]?\d+\.dng$(大小写不敏感,位数不写死、可带目录前缀),
     * 一个都没命中才回退成 zip 内全部 .dng(见 docs/INTERFACES.md §10.2)。
     * @return 0 成功(即使 0 帧也算成功,调用方查 frame_count());<0 失败,err 有原因。
     *         fd 只存下不拥有,调用方负责关。
     */
    int open(int fd, int64_t len, std::string &err);

    /** 匹配到的 DNG 帧条目数。 */
    size_t frame_count() const { return m_frames.size(); }

    /** 第 i 帧条目(已按自然序排好;不是字典序、不是 lho 序)。 */
    const DngZipEntry &entry(size_t i) const { return m_frames[i]; }

    /** ZIP 中是否存在 basename 为 audio.wav 的音频成员。 */
    bool has_audio() const { return m_have_audio; }

    /** 仅在 has_audio() 为真时有效。 */
    const DngZipEntry &audio_entry() const { return m_audio; }

    /**
     * 把 ZIP 内 audio.wav 解压进内存。out 会被清空后 resize 成 usize。
     * @return 0 成功;<0 失败,err 带有条目名和原因。
     */
    int read_audio(std::vector<uint8_t> &out, std::string &err);

    /**
     * 把第 i 帧解压进内存(out 会被清空后 resize 成 usize)。
     * @return 0 成功;<0 失败,err 形如 "zip 第 123 帧(frame-00123.dng)解压失败: ...".
     *         失败时绝不静默跳帧(契约 §10.3)。
     */
    int read_frame(size_t i, std::vector<uint8_t> &out, std::string &err);

private:
    int m_fd = -1;
    int64_t m_len = 0;
    std::vector<DngZipEntry> m_frames;
    DngZipEntry m_audio;
    bool m_have_audio = false;

    int64_t pread_at(uint64_t off, void *buf, size_t n) const;
};

// ---------------------------------------------------------------------------
// LibRaw 单帧解码(契约 §10.4)
// ---------------------------------------------------------------------------

/** dngq 画质档(dngzip.h 与 transcode.cpp/preview.cpp 共用)。 */
enum class DngQuality : int {
    FAST = 0,     // half_size=1 + user_qual=0,最快
    BALANCED = 1, // user_qual=2 (PPG),默认
    FINE = 2      // user_qual=3 (AHD),最慢最好
};

/** dngcolor 白平衡档。 */
enum class DngColorMode : int {
    CAMERA = 0, // 相机/机身 WB(use_camera_wb),默认
    AUTO = 1,   // LibRaw 自动 WB(use_auto_wb)
    D65 = 2     // 固定日光:user_mul 全 1
};

/** DNG 线性色彩映射(契约 §3)。默认 NONE 保持既有 sRGB 线性输出。 */
enum class DngMap : int {
    NONE = 0,
    APPLE_LOG = 1,
};

/**
 * 解单帧 DNG → 16bit RGB,交错存成 rgb48le 形状(每像素 R,G,B 各一个 uint16,行主序)。
 *
 * LibRaw 内部每像素 4 个 ushort(image[i][0..3]),这里取前 3 通道;行距按
 * frame 宽度( LibRaw 的 image  [h][w][4])逐行拷贝,保证内存连续。
 *
 * @param buf/size 内存里的 DNG 文件内容(read_frame 的输出)。
 * @param q/cm/map spec 的 dngq / dngcolor / dngmap 档位;map=applelog 时输出 Rec.2020 线性。
 * @param out_rgb  输出(元素个数 == w*h*3)。
 * @param w/h      输出真实宽高(可能是 demosaic 后的尺寸;half_size 档会是尺寸的一半)。
 * @param raw_w/raw_h  可选:解包前的原始全尺寸。探测路径使用它上报媒体尺寸,
 *                     转码/预览路径可传 nullptr。
 * @param want_half_size  半尺寸解算开关,**默认关**。仅预览路径会打开(N-08):
 *                     预览只求「看得见的图」,4096x3072 全幅 RGB48 = 72MB 解码
 *                     占用,而 mjpeg 输出上限按 kPreviewMaxBytes 只有 64MB
 *                     —— 全幅预览必败。注意与 dngq=FAST 的区别:FAST 是「半尺寸
 *                     + 最快 demosaic」的画质档,会改变成品/预览的画质语义;
 *                     本参数只是降采样解码,档位 q 原样透传。
 *                     走的是 dngzip.cpp 里「恢复 tlr_params0 快照 → apply_libraw_params」
 *                     的每帧路径,与 fastsize 复用陷阱(FAST 帧后跟 BALANCED 帧残留
 *                     half_size=1)同一条防线:复用实例的 params 每帧都从构造默认值
 *                     复位,不会被上一帧的档位字段污染。
 * @return 0 成功;<0 失败,err 有原因(LibRaw 错误码会被格式化成文字)。
 */
int dng_decode_to_rgb48(const uint8_t *buf, size_t size,
                        DngQuality q, DngColorMode cm,
                        std::vector<uint16_t> &out_rgb,
                        int &w, int &h, std::string &err,
                        int *raw_w = nullptr, int *raw_h = nullptr,
                        DngMap map = DngMap::NONE,
                        bool want_half_size = false);

} // namespace proresforge

#endif // PRORESFORGE_DNGZIP_H
