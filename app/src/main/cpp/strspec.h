// strspec.h
//
// 转码参数 spec 的 k=v 解析器(header-only,纯 C++,不依赖 FFmpeg)。
//
// spec 是一份以 '\n' 分隔的 key=value 文本,由 Kotlin 侧 FFmpeg.Spec 序列化得到,
// 形如(键名集合见 SpecKeys,与契约 INTERFACES.md §3 逐字一致):
//
//   vcodec=libx264
//   pixfmt=yuv420p
//   w=1920
//   h=1080
//   fpsnum=30
//   fpsden=1
//   rc=crf
//   crf=20
//   bitrate=8000000
//   gop=2
//   profile=
//   lut=/data/user/0/com.proresforge.app/cache/luts/<sha>.cube
//   lutint=1.0
//   scale=lanczos
//   rotate=auto
//   cspace=
//   cprim=
//   ctrc=
//   audio=copy
//   faststart=1
//   title=
//   src=dngzip              # DNG 序列 zip(MotionCam),见契约 §10;缺省 = 普通容器
//   dngq=1                  # 0 快速/1 均衡/2 精细(仅 src=dngzip 有效)
//   dngcolor=camera         # camera/auto/d65(仅 src=dngzip 有效)
//   dngmap=none             # none/applelog(仅 src=dngzip 有效)
//
// 约定:
//   * 键名区分大小写;value 可以含 '='(只按第一个 '=' 切分)。
//   * value 不含换行;text 里的行可以带 '\r'(回车),由 trim 去掉。
//   * 缺失的键通过 get 返回空串 / get_i、get_d 返回 fallback(不抛异常、不崩溃)。
//   * 查找是线性扫描(O(n),n<=27,与下方 kAll 的键数一致),转码全程只解析一次,足够快。
//
// 注意:Kotlin 侧的 Spec → 文本序列化必须使用同一份键名(见 SpecKeys),
// 这里任何改动都会破坏 Kotlin/C++ 的契约,不要单独增删键。
#ifndef PRORESFORGE_STRSPEC_H
#define PRORESFORGE_STRSPEC_H

#include <string>
#include <vector>
#include <cstdlib>
#include <cstdint>
#include <cstdio>

namespace proresforge {

// 与契约 INTERFACES.md §3 表完全一致的键名清单(不得增删)。
// Kotlin 侧 FFmpeg.Spec.toSpecText() 逐字使用这些名字。
struct SpecKeys {
    static const char *kVideoCodec;   // vcodec   h264_mediacodec/libx264/hevc_mediacodec/libx265/libvpx-vp9/prores_ks
    static const char *kPixFmt;       // pixfmt   yuv420p/yuv420p10le/yuv422p10le
    static const char *kWidth;        // w
    static const char *kHeight;       // h
    static const char *kFpsNum;       // fpsnum
    static const char *kFpsDen;       // fpsden
    static const char *kRateControl;  // rc       crf/bitrate/q
    static const char *kCrf;          // crf
    static const char *kBitrate;      // bitrate  bps,可能超过 int32
    static const char *kGopSeconds;   // gop     0 = 编码器默认
    static const char *kProfile;      // profile 可空
    static const char *kLut;          // lut      .cube 绝对路径,可空
    static const char *kLutIntensity; // lutint   0..1,<1 时 split+blend
    static const char *kScaleFlags;   // scale    lanczos/bicubic/fast_bilinear
    static const char *kRotate;       // rotate   auto/0/90/180/270
    static const char *kColorSpace;   // cspace   可空,只写 metadata
    static const char *kColorPrimaries;// cprim  可空,只写 metadata
    static const char *kColorTrc;     // ctrc    可空,只写 metadata
    static const char *kAudio;        // audio    copy/aac/drop
    static const char *kFastStart;    // faststart 0/1
    static const char *kTitle;        // title   可空
    static const char *kSource;       // src     dngzip = DNG 序列 zip(契约 §10)
    static const char *kDngQuality;   // dngq    0 快速/1 均衡/2 精细(仅 src=dngzip)
    static const char *kDngColor;     // dngcolor camera/auto/d65(仅 src=dngzip)
    static const char *kDngMap;       // dngmap none/applelog(仅 src=dngzip)
    static const char *kDngThreads;   // dngthreads 并行解码线程数,0=按内存预算自动(仅 src=dngzip)
    static const char *kDngDepth;     // dngdepth  预取深度(已解码帧排队数),0=自动(仅 src=dngzip)

    // 全部键名(共 27 个,与契约 §3 表格行一致;顺序即表格顺序,定义见文件末尾)。
    static const char *const kAll[27];
};

// 一份解析后的 spec。构造后调用 parse(spec_text, out)。
// 只提供 getter,不做校验之外的逻辑;校验由 transcode.cpp 负责。
class Spec {
public:
    std::string spec_text;

    // 一条解析后的 k=v(必须在使用它的方法签名之前可见,参数类型不做完整类上下文延迟)
    typedef std::pair<std::string, std::string> KV;

    // 解析整段文本;失败返回 false(spec_text 为空时视为空 spec,返回 true)。
    bool parse(const char *text) {
        pairs.clear();
        if (!text) return true;
        spec_text = text;
        size_t pos = 0;
        std::string s(text);
        while (pos < s.size()) {
            size_t nl = s.find('\n', pos);
            if (nl == std::string::npos) nl = s.size();
            std::string line = trim(s.substr(pos, nl - pos));
            if (!line.empty() && line[0] != '#') {
                size_t eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string k = trim(line.substr(0, eq));
                    std::string v = trim(line.substr(eq + 1));
                    if (!k.empty()) pairs.push_back(KV(k, v));
                }
            }
            pos = nl + 1;
        }
        return true;
    }

    bool parse(const std::string &text) { return parse(text.c_str()); }

    // ---------- 字符串 ----------
    // 查找键,找不到返回 false
    static bool find(const std::vector<KV> &v, const std::string &key, std::string *out) {
        for (size_t i = 0; i < v.size(); i++) {
            if (v[i].first == key) { if (out) *out = v[i].second; return true; }
        }
        return false;
    }

    bool find(const std::string &key, std::string *out) const { return find(pairs, key, out); }

    // 取值,缺失时返回空串
    static std::string get(const std::vector<KV> &v, const std::string &key) {
        std::string s;
        return find(v, key, &s) ? s : std::string();
    }

    static std::string get(const std::vector<KV> &v, const char *key) {
        return get(v, std::string(key));
    }

    std::string get(const char *key) const { return get(pairs, std::string(key)); }
    std::string get(const std::string &key) const { return get(pairs, key); }

    // 写入/更新键(已存在则覆盖,否则追加)
    static void set(std::vector<KV> &v, const std::string &key, const std::string &value) {
        for (size_t i = 0; i < v.size(); i++) {
            if (v[i].first == key) { v[i].second = value; return; }
        }
        v.push_back(KV(key, value));
    }

    static void set(std::vector<KV> &v, const char *key, const std::string &value) {
        set(v, std::string(key), value);
    }

    void set(const char *key, const std::string &value) { set(pairs, key, value); }

    // ---------- 数值 ----------
    static bool get_i(const std::vector<KV> &v, const char *key, long long *out) {
        std::string s = get(v, key);
        if (s.empty()) return false;
        char *end = nullptr;
        long long n = std::strtoll(s.c_str(), &end, 10);
        if (!end || *end != '\0') return false; // 有垃圾字符,拒绝
        if (out) *out = n;
        return true;
    }

    static long long get_i(const std::vector<KV> &v, const char *key, long long fallback) {
        long long n = 0;
        return get_i(v, key, &n) ? n : fallback;
    }

    long long get_i(const char *key, long long fallback) const { return get_i(pairs, key, fallback); }

    // 写入/更新数值键(nativeInfo 等收集侧用)
    void set_i(const char *key, long long value) { set_i(pairs, key, value); }

    static bool set_i(std::vector<KV> &v, const char *key, long long value) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", value);
        set(v, key, std::string(buf));
        return true;
    }

    static bool get_l(const std::vector<KV> &v, const char *key, int64_t *out) {
        std::string s = get(v, key);
        if (s.empty()) return false;
        char *end = nullptr;
        long long n = std::strtoll(s.c_str(), &end, 10);
        if (!end || *end != '\0') return false;
        if (out) *out = (int64_t)n;
        return true;
    }

    static int64_t get_l(const std::vector<KV> &v, const char *key, int64_t fallback) {
        int64_t n = 0;
        return get_l(v, key, &n) ? n : fallback;
    }

    int64_t get_l(const char *key, int64_t fallback) const { return get_l(pairs, key, fallback); }

    static bool get_d(const std::vector<KV> &v, const char *key, double *out) {
        std::string s = get(v, key);
        if (s.empty()) return false;
        char *end = nullptr;
        double d = std::strtod(s.c_str(), &end);
        if (!end || *end != '\0') return false;
        if (out) *out = d;
        return true;
    }

    static double get_d(const std::vector<KV> &v, const char *key, double fallback) {
        double d = 0;
        return get_d(v, key, &d) ? d : fallback;
    }

    double get_d(const char *key, double fallback) const { return get_d(pairs, key, fallback); }

    // 直接访问底层
    const std::vector<KV> &items() const { return pairs; }

    // 重新序列化成 "k=v\n" 文本(调试/日志用)
    static std::string serialize(const std::vector<KV> &v) {
        std::string s;
        for (size_t i = 0; i < v.size(); i++) {
            s += v[i].first;
            s += '=';
            s += v[i].second;
            s += '\n';
        }
        return s;
    }

    // ---------- 文本工具 ----------
    // 去掉首尾空白(空格/制表/回车/换行)
    static std::string trim(const std::string &s) {
        size_t b = 0, e = s.size();
        while (b < e && is_ws(s[b])) b++;
        while (e > b && is_ws(s[e - 1])) e--;
        return s.substr(b, e - b);
    }

    static std::string trim(const char *s) { return s ? trim(std::string(s)) : std::string(); }

    // 按 sep 切分(不忽略空段)
    static std::vector<std::string> split(const std::string &s, char sep) {
        std::vector<std::string> out;
        size_t pos = 0;
        while (true) {
            size_t p = s.find(sep, pos);
            if (p == std::string::npos) { out.push_back(s.substr(pos)); break; }
            out.push_back(s.substr(pos, p - pos));
            pos = p + 1;
        }
        return out;
    }

private:
    std::vector<KV> pairs;

    static bool is_ws(char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    }
};

// 键名定义(契约 §3,逐字对应)
inline const char *SpecKeys::kVideoCodec = "vcodec";
inline const char *SpecKeys::kPixFmt = "pixfmt";
inline const char *SpecKeys::kWidth = "w";
inline const char *SpecKeys::kHeight = "h";
inline const char *SpecKeys::kFpsNum = "fpsnum";
inline const char *SpecKeys::kFpsDen = "fpsden";
inline const char *SpecKeys::kRateControl = "rc";
inline const char *SpecKeys::kCrf = "crf";
inline const char *SpecKeys::kBitrate = "bitrate";
inline const char *SpecKeys::kGopSeconds = "gop";
inline const char *SpecKeys::kProfile = "profile";
inline const char *SpecKeys::kLut = "lut";
inline const char *SpecKeys::kLutIntensity = "lutint";
inline const char *SpecKeys::kScaleFlags = "scale";
inline const char *SpecKeys::kRotate = "rotate";
inline const char *SpecKeys::kColorSpace = "cspace";
inline const char *SpecKeys::kColorPrimaries = "cprim";
inline const char *SpecKeys::kColorTrc = "ctrc";
inline const char *SpecKeys::kAudio = "audio";
inline const char *SpecKeys::kFastStart = "faststart";
inline const char *SpecKeys::kTitle = "title";
inline const char *SpecKeys::kSource = "src";
inline const char *SpecKeys::kDngQuality = "dngq";
inline const char *SpecKeys::kDngColor = "dngcolor";
inline const char *SpecKeys::kDngMap = "dngmap";
inline const char *SpecKeys::kDngThreads = "dngthreads";
inline const char *SpecKeys::kDngDepth = "dngdepth";

// 键名清单(契约 §3 表顺序,共 27 个);inline 变量,不需要额外的 .cpp 定义。
//   kSource/kDngQuality/kDngColor/kDngMap 是 DNG 序列 zip(契约 §10)新增的四个键:
//   src=dngzip 时走 DngZip+LibRaw 单帧解码链;dngq=0|1|2 是 LibRaw 画质档,
//   dngcolor=camera|auto|d65 是白平衡档。
//   kDngThreads/kDngDepth(2026-10-05 补)是并行解码的两个旋钮:解码是整条链里
//   86% 的耗时(实测 4096x3072 BALANCED:read 0.056s / decode 0.351s),不并行就只能
//   跑满一个核;0 表示按「内存预算 / 单帧峰值」自动推算,见 transcode.cpp 定参处。
inline const char *const SpecKeys::kAll[27] = {
    SpecKeys::kVideoCodec, SpecKeys::kPixFmt, SpecKeys::kWidth, SpecKeys::kHeight,
    SpecKeys::kFpsNum, SpecKeys::kFpsDen, SpecKeys::kRateControl, SpecKeys::kCrf,
    SpecKeys::kBitrate, SpecKeys::kGopSeconds, SpecKeys::kProfile, SpecKeys::kLut,
    SpecKeys::kLutIntensity, SpecKeys::kScaleFlags, SpecKeys::kRotate,
    SpecKeys::kColorSpace, SpecKeys::kColorPrimaries, SpecKeys::kColorTrc,
    SpecKeys::kAudio, SpecKeys::kFastStart, SpecKeys::kTitle,
    SpecKeys::kSource, SpecKeys::kDngQuality, SpecKeys::kDngColor, SpecKeys::kDngMap,
    SpecKeys::kDngThreads, SpecKeys::kDngDepth
};

} // namespace proresforge

#endif // PRORESFORGE_STRSPEC_H
