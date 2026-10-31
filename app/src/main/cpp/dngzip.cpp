// dngzip.cpp
//
// DNG 序列 zip(MotionCam)解析 + LibRaw 单帧解码,契约 INTERFACES.md §10。
//
// ZIP 部分完全手写(不依赖 FFmpeg、不解压到磁盘):
//   * 尾部 64KB 窗口反扫 EOCD(0x06054b50)拿总条目数与中央目录偏移;
//     ZIP64 sentinel 时再读 ZIP64 EOCD locator(0x07064b50)→ ZIP64 EOCD(0x06064b50)。
//   * 中央目录项(0x02014b50)取 method/csize/usize/lho;ZIP64 的真实 8 字节值放在
//     extra field(id 0x0001)里,按「是否存在 sentinel」的顺序消费。
//   * read_frame 按**本地**头的 name_len/extra_len 算数据起点(本地 extra 与中央目录
//     的 extra 长度可以不同),再 raw inflate(inflateInit2(-15))、按 1MB pread 喂。
//   * 条目只收 ^frame-\d{5}\.dng$(按 basename 判断,大小写不敏感)且 usize>0,
//     排序按**帧号自然序**(frame-00002.dng < frame-00010.dng),不是字典序、不是 lho 序。
//
// LibRaw 部分:open_buffer → unpack → dcraw_process → imgdata.image(每像素 4 个
// ushort 交错)取前 3 通道逐行拷成 16bit RGB(rgb48le 形状)。
//
// P1-2(LibRaw 实例 worker-local 复用):LibRaw 实例改为**线程局部**常驻,同一
// 解码 worker 线程跨帧复用,不再每帧 make_unique + 析构;帧间状态由成功路径
// 尾部的 lr->recycle() 复位(释放 raw_alloc/image 等大缓冲),失败路径丢弃
// 实例下次重建。宏 PRF_LIBRAW_REUSE 默认 1,便于 A/B 与回退。

#include "dngzip.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <algorithm>
#include <memory>
#include <limits>

#include <unistd.h>
#include <errno.h>

// ---------------------------------------------------------------------------
// 大文件偏移:本文件的 IO 一律走显式 64 位变体 pread64/pwrite64/lseek64,
// **不**改用 _FILE_OFFSET_BITS=64。
//
// 原因:NDK r27 的 sys/cdefs.h:195 规定 LP32(armeabi-v7a)下 off_t 默认 **32 位**,
// 只有定义 _FILE_OFFSET_BITS=64 才变 64 位。MotionCam DNG 序列 zip 单包 5.2GB 量级:
// 中央目录偏移 5,273,658,428 本身已超 2^32,且 4GiB 之后的帧(lho > 4GiB)有 60 多项。
// off_t 截断后 pread 拿到的是 offset & 0xFFFFFFFF,读到的既不是本地头也不是数据
// → 探测失败 → UI "无法解析"。(LP64 平台 off_t 本来 64 位,所以此 bug 只在 32 位
// 设备复现;unistd.h:332 vs 344 两套声明就是同一个 off_t 的两副面孔。)
//
// 为什么不全文件定义 _FILE_OFFSET_BITS=64:它会经 FFmpeg 头传播,改变
// ff_AMediaCodec_queueInputBuffer 的 off_t 形参 ABI(NdkMediaCodec.h:267-286 有
// static_assert 专门卡这一点),并让 libraw.a(以 NDK 默认 32 位 off_t 编译)与本
// so 产生 off_t 大小不一致的风险。显式调用 *64 不变 ABI、不改宏、零传染面。
// ---------------------------------------------------------------------------

#include <zlib.h>

// 诊断日志走 av_log(已由 native-lib 的 pf_av_log 落到 logcat + app.log,
// 用户点「日志」就能看到「为什么这个 zip 没解出来」—— 2026-10-04 真机反馈
// 「无法解析」时之所以无从下手,就是因为这条链上一条日志都没有)。
// 注意:libavutil/log.h 没有 extern "C" 保护(FFmpeg 的 libavutil 头是个例外),
// C++ 里 include 会把 av_log 链成 C++ 符号 → 链接期 undefined symbol。
extern "C" {
#include <libavutil/log.h>
}

// LibRaw 是 C++ 库(libraw.a 由 LibRaw-cmake 的 C++ target 产出),
// 必须按 C++ 包含:包进 extern "C" 会把它传递 include 的 <fstream>/<filesystem>
// 也变成 C 链接,libc++ 直接报 "functions that differ only in their return type"。
#include <libraw/libraw.h>

// ---------------------------------------------------------------------------
// P1-2 编译宏:worker-local LibRaw 实例复用开关。
//   1(默认)= 线程局部常驻实例,帧间 recycle 复位后复用,失败即丢弃重建;
//   0      = 原行为,每帧 make_unique<LibRaw>() + 析构。
// 复用实现与验证结论见 dng_decode_to_rgb48 内注释;A/B 一致性要求:
// PRF_LIBRAW_REUSE=0/1 两种构建对同一帧的解码输出必须逐字节一致
// (host 侧 /tmp/prf-bench2 bench_reuse SHA-256 实测)。
// ---------------------------------------------------------------------------
#ifndef PRF_LIBRAW_REUSE
#define PRF_LIBRAW_REUSE 1
#endif

namespace proresforge {

// ---------------------------------------------------------------------------
// ZIP 字节序/签名小工具(全部显式 LE,不依赖主机字节序)
// ---------------------------------------------------------------------------
namespace {

// 单帧 DNG 的尺寸上限(压缩后)。真 MotionCam 的 4K 帧压缩后 ~5-15MB,
// 这里留足余量;超过说明 usize 撒谎(或被伪造),直接报错而不是吃内存。
constexpr uint64_t kMaxFrameUsize = 512ull * 1024 * 1024;
// WAV 也必须完整驻留内存才能挂到 FFmpeg 的自定义 AVIO;沿用同一上限,
// 避免恶意 ZIP 通过 audio.wav 触发不可控分配。
constexpr uint64_t kMaxAudioUsize = 512ull * 1024 * 1024;
// 探测日志:zip 里最多列几个条目名(避免 900 帧刷屏)
constexpr size_t kLogMaxNames = 8;

constexpr uint32_t kSigEOCD      = 0x06054b50u; // End of central directory
constexpr uint32_t kSigZ64EOCD   = 0x06064b50u; // ZIP64 end of central directory
constexpr uint32_t kSigZ64Loc    = 0x07064b50u; // ZIP64 EOCD locator
constexpr uint32_t kSigCDEntry   = 0x02014b50u; // Central directory file header
constexpr uint32_t kSigLocal     = 0x04034b50u; // Local file header

constexpr uint32_t kU16Max = 0xFFFFu;
constexpr uint32_t kU32Max = 0xFFFFFFFFu;

// 尾部扫 EOCD 的窗口:注释最长 64KB + EOCD 本身 22 字节
constexpr int64_t kEocdSearchWindow = 64 * 1024;
// inflate 喂食块大小
constexpr size_t kInflateChunk = 1u << 20;

inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

inline bool checked_add_u64(uint64_t a, uint64_t b, uint64_t &out) {
    if (b > std::numeric_limits<uint64_t>::max() - a) return false;
    out = a + b;
    return true;
}

// 名字的 basename(去掉 zip 里可能的目录部分)
inline std::string basename_of(const std::string &n) {
    size_t slash = n.find_last_of("/\\");
    return slash == std::string::npos ? n : n.substr(slash + 1);
}

// ASCII 小写化(用于大小写不敏感的后缀/前缀比较)
inline char lower_ascii(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= 'A' && u <= 'Z') ? (char)(u - 'A' + 'a') : (char)u;
}

// 单个字符是否 ASCII 字母(数字检查走 ctype)
inline bool is_digit_char(char c) {
    return c >= '0' && c <= '9';
}

// basename 是否以 .dng 结尾(大小写不敏感)
inline bool has_dng_ext(const std::string &raw_name) {
    const std::string n = basename_of(raw_name);
    if (n.size() < 5) return false;
    const size_t p = n.size() - 4;   // ".dng" 起点
    return lower_ascii(n[p]) == '.' && lower_ascii(n[p + 1]) == 'd' &&
           lower_ascii(n[p + 2]) == 'n' && lower_ascii(n[p + 3]) == 'g';
}

// ^frame[_-]?\d+\.dng$(大小写不敏感)。
// 历史约定是 frame-#####(连字符 5 位),但不同导出工具/版本会用
// frame_000000.dng(下划线 6 位)之类;位数不写死。
inline bool is_dng_frame_name(const std::string &raw_name) {
    const std::string base = basename_of(raw_name);
    if (!has_dng_ext(base)) return false;
    std::string stem = base.substr(0, base.size() - 4);   // 去掉 .dng
    for (char &ch : stem) ch = lower_ascii(ch);
    if (stem.compare(0, 5, "frame") != 0) return false;
    size_t i = 5;
    if (i < stem.size() && (stem[i] == '-' || stem[i] == '_')) i++;
    if (i >= stem.size() || !is_digit_char(stem[i])) return false;
    for (; i < stem.size(); i++) {
        if (!is_digit_char(stem[i])) return false;
    }
    return true;
}

// 自然序比较:数字段按数值比,非数字段按字节比;数值相等时更短的(零填充更少的)在前。
inline bool natural_less(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[j];
        bool da = isdigit(ca) != 0, db = isdigit(cb) != 0;
        if (da && db) {
            size_t ei = i, ej = j;
            while (ei < a.size() && isdigit((unsigned char)a[ei])) ei++;
            while (ej < b.size() && isdigit((unsigned char)b[ej])) ej++;
            // 跳过前导零(保留最后一位,避免 "00000" 变空)
            size_t si = i, sj = j;
            while (si + 1 < ei && a[si] == '0') si++;
            while (sj + 1 < ej && b[sj] == '0') sj++;
            size_t la = ei - si, lb = ej - sj;
            if (la != lb) return la < lb;               // 位数多的数字更大
            int cmp = a.compare(si, la, b, sj, lb);
            if (cmp != 0) return cmp < 0;
            i = ei;
            j = ej;
        } else {
            if (ca != cb) return ca < cb;
            i++;
            j++;
        }
    }
    // 前缀相同:剩下的内容更少的在前
    return (a.size() - i) < (b.size() - j);
}

// CRC-32 以 8 位十六进制呈现(与 zip 工具/规范的写法对齐,便于对照修复)
inline std::string hex32(uint32_t v) {
    char buf[11];
    std::snprintf(buf, sizeof(buf), "0x%08X", (unsigned)v);
    return std::string(buf);
}

} // namespace

// ---------------------------------------------------------------------------
// DngZip
// ---------------------------------------------------------------------------
DngZip::~DngZip() = default;

bool DngZip::is_zip(const uint8_t *head, size_t n) {
    return head && n >= 4 &&
           head[0] == 0x50 && head[1] == 0x4b && head[2] == 0x03 && head[3] == 0x04;
}

int64_t DngZip::pread_at(uint64_t off, void *buf, size_t n) const {
    size_t done = 0;
    uint8_t *p = (uint8_t *)buf;
    while (done < n) {
        // pread64:off_t 在 LP32 上只有 32 位,5.2GB zip 的 CD 偏移/lho 会截断
        // (详见文件头「大文件偏移」注释)。
        uint64_t read_off = 0;
        if (!checked_add_u64(off, (uint64_t)done, read_off)) return -1;
        ssize_t r = pread64(m_fd, p + done, n - done, (off64_t)read_off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;            // EOF:短读
        done += (size_t)r;
    }
    return (int64_t)done;
}

int DngZip::open(int fd, int64_t len, std::string &err) {
    m_fd = fd;
    m_frames.clear();
    m_audio = DngZipEntry{};
    m_have_audio = false;

    if (fd < 0) {
        err = "zip: 无效的 fd/长度";
        av_log(nullptr, AV_LOG_WARNING, "dngzip: open 失败: fd=%d len=%lld\n", fd,
               (long long)len);
        return -1;
    }

    // len <= 0 时先用 lseek64 兜底测一次真实长度:上层 FFmpeg.kt 的
    // pfd.statSize 对某些 SAF provider 会抛异常并回退成 -1L(见
    // `writeFd()`),而 MotionCam 的 zip 又全都是 5GB 量级 —— 直接 reject 会把
    // 合法的 DNG 序列 zip 误判成「无法解析」。lseek64 不依赖 off_t 位宽。
    if (len <= 0) {
        off64_t end = lseek64(fd, 0, SEEK_END);
        if (end > 0) {
            av_log(nullptr, AV_LOG_WARNING,
                   "dngzip: len=%lld 不可用,改用 lseek64 测得长度 %lld\n",
                   (long long)len, (long long)end);
            len = (int64_t)end;
        }
    }
    m_len = len;

    if (len <= 0) {
        err = "zip: 无效的 fd/长度";
        av_log(nullptr, AV_LOG_WARNING, "dngzip: open 失败: fd=%d len=%lld\n", fd,
               (long long)len);
        return -1;
    }

    // ---- 1. 尾部窗口反扫 EOCD -------------------------------------------------
    const int64_t win = std::min<int64_t>(len, kEocdSearchWindow);
    if (win < 22) {
        err = "zip: 文件太小,不是有效 zip";
        return -1;
    }
    const int64_t tail_off = len - win;
    std::vector<uint8_t> tail((size_t)win);
    if (pread_at((uint64_t)tail_off, tail.data(), tail.size()) != (int64_t)tail.size()) {
        err = "zip: 读取尾部落盘失败(EOCD 扫描窗口)";
        return -1;
    }

    int64_t eocd = -1;
    for (int64_t i = win - 22; i >= 0; i--) {
        if (rd32(tail.data() + i) == kSigEOCD) {
            eocd = tail_off + i;
            break;                    // 最后一个 EOCD(注释里即便有同样的字节也罕见)
        }
    }
    if (eocd < 0) {
        err = "zip: 找不到 EOCD(文件损坏或根本不是 zip)";
        av_log(nullptr, AV_LOG_WARNING,
               "dngzip: open 失败(len=%lld): 尾窗 64KB 内没有 EOCD 签名\n",
               (long long)len);
        return -1;
    }

    const uint8_t *e = tail.data() + (size_t)(eocd - tail_off);
    uint64_t total_entries = rd16(e + 10);   // 16 位版本(会被 0xFFFF 覆盖)
    uint64_t cd_off = rd32(e + 16);           // 32 位偏移
    uint64_t cd_size = rd32(e + 12);
    bool need_zip64 = (rd16(e + 10) == kU16Max) || (rd32(e + 12) == kU32Max) ||
                      (rd32(e + 16) == kU32Max);

    // ---- 2. ZIP64:EOCD 前 20 字节的 locator → ZIP64 EOCD ----------------------
    if (need_zip64) {
        const int64_t loc_pos = eocd - 20;
        if (loc_pos < 0) {
            err = "zip: ZIP64 EOCD locator 越界";
            return -1;
        }
        uint8_t loc[20];
        if (pread_at((uint64_t)loc_pos, loc, sizeof(loc)) != (int64_t)sizeof(loc) ||
            rd32(loc) != kSigZ64Loc) {
            err = "zip: ZIP64 EOCD locator 损坏(sig 不符)";
            return -1;
        }
        const uint64_t z64_off = rd64(loc + 8);
        // L-2:原来直接拿 z64_off pread,靠失败兜底报「ZIP64 EOCD 损坏」,错误笼统;
        // 打开时先校验范围,越界就直接给出「ZIP64 EOCD 偏移越界」+ 具体偏移。
        // 用减法判断避免 z64_off + 56 在 uint64_t 上溢出。
        const uint64_t file_len = (uint64_t)m_len;
        if (z64_off > file_len || 56u > file_len - z64_off) {
            av_log(nullptr, AV_LOG_ERROR,
                   "dngzip: ZIP64 EOCD 偏移越界(off=%llu, len=%lld)\n",
                   (unsigned long long)z64_off, (long long)m_len);
            err = "zip: ZIP64 EOCD 偏移越界";
            return -1;
        }
        uint8_t zh[56];              // ZIP64 EOCD 固定 56 字节
        if (pread_at(z64_off, zh, sizeof(zh)) != (int64_t)sizeof(zh) ||
            rd32(zh) != kSigZ64EOCD) {
            err = "zip: ZIP64 EOCD 损坏(sig 不符)";
            return -1;
        }
        total_entries = rd64(zh + 32);   // 全局条目数
        cd_size = rd64(zh + 40);
        cd_off = rd64(zh + 48);
    }

    const uint64_t file_len = (uint64_t)m_len;
    if (cd_off > file_len || cd_size > file_len - cd_off) {
        av_log(nullptr, AV_LOG_ERROR,
               "dngzip: 中央目录范围越界(off=%llu, size=%llu, len=%lld)\n",
               (unsigned long long)cd_off, (unsigned long long)cd_size,
               (long long)m_len);
        err = "zip: 中央目录偏移/长度越界";
        return -1;
    }
    uint64_t cd_end = 0;
    if (!checked_add_u64(cd_off, cd_size, cd_end)) {
        av_log(nullptr, AV_LOG_ERROR,
               "dngzip: 中央目录范围计算溢出(off=%llu, size=%llu)\n",
               (unsigned long long)cd_off, (unsigned long long)cd_size);
        err = "zip: 中央目录范围溢出";
        return -1;
    }

    // ---- 3. 顺序走中央目录 ----------------------------------------------------
    // frames_named / dng_all 分开收集:前者只收 frame-NNNNN 约定名,后者收所有
    // .dng;前者空就用后者兜底(不同 MotionCam 版本/导出工具命名不一样,见上)。
    std::vector<DngZipEntry> frames_named;
    std::vector<DngZipEntry> dng_all;
    std::string log_names;      // 前几个条目名,供诊断日志
    size_t dng_seen = 0;
    uint64_t pos = cd_off;
    for (uint64_t k = 0; k < total_entries; k++) {
        // 每次读头前都受中央目录闭区间 [cd_off, cd_end) 限制,不能只看文件长度。
        if (pos > cd_end || cd_end - pos < 46u) {
            av_log(nullptr, AV_LOG_ERROR,
                   "dngzip: 中央目录项头超出 cd_size(pos=%llu, cd_end=%llu, entry=%llu)\n",
                   (unsigned long long)pos, (unsigned long long)cd_end,
                   (unsigned long long)k);
            err = "zip: 中央目录项头超出中央目录范围";
            return -1;
        }
        uint8_t ch[46];
        if (pread_at(pos, ch, sizeof(ch)) != (int64_t)sizeof(ch)) {
            err = "zip: 读中央目录头失败(文件可能被截断)";
            av_log(nullptr, AV_LOG_ERROR,
                   "dngzip: 读中央目录头失败(pos=%llu, entry=%llu)\n",
                   (unsigned long long)pos, (unsigned long long)k);
            return -1;
        }
        if (rd32(ch) != kSigCDEntry) {
            // R10-4:签名不符一律失败。旧代码「已收集到帧就 break」会把后半段
            // 损坏的中央目录当成成功:函数末尾仍 return 0,转码按截短后的帧表
            // 继续跑,用户看到「成功」但后半段帧根本不在列表里。
            // 自第一帧之前的损坏本来就报错,中途的坏也不例外,错误串与两表都空
            // 时保持同一句。
            err = "zip: 中央目录项签名不符(不是 zip 或被破坏)";
            av_log(nullptr, AV_LOG_ERROR,
                   "dngzip: 中央目录项签名不符(pos=%llu, entry=%llu)\n",
                   (unsigned long long)pos, (unsigned long long)k);
            return -1;
        }

        const uint16_t method = rd16(ch + 10);
        const uint32_t crc32_cd = rd32(ch + 16);
        const uint32_t csize32 = rd32(ch + 20);
        const uint32_t usize32 = rd32(ch + 24);
        const uint16_t nlen = rd16(ch + 28);
        const uint16_t xlen = rd16(ch + 30);
        const uint16_t clen = rd16(ch + 32);
        const uint32_t lho32 = rd32(ch + 42);

        uint64_t entry_size = 46;
        uint64_t next_pos = 0;
        if (!checked_add_u64(entry_size, (uint64_t)nlen, entry_size) ||
            !checked_add_u64(entry_size, (uint64_t)xlen, entry_size) ||
            !checked_add_u64(entry_size, (uint64_t)clen, entry_size) ||
            !checked_add_u64(pos, entry_size, next_pos) || next_pos > cd_end) {
            av_log(nullptr, AV_LOG_ERROR,
                   "dngzip: 中央目录项长度越界(pos=%llu, name=%u, extra=%u, comment=%u)\n",
                   (unsigned long long)pos, (unsigned)nlen, (unsigned)xlen,
                   (unsigned)clen);
            err = "zip: 中央目录项长度超出中央目录范围";
            return -1;
        }
        uint64_t extra_off = 0;
        if (!checked_add_u64(pos, 46u, extra_off) ||
            !checked_add_u64(extra_off, (uint64_t)nlen, extra_off)) {
            av_log(nullptr, AV_LOG_ERROR,
                   "dngzip: 中央目录 extra 偏移计算溢出(pos=%llu)\n",
                   (unsigned long long)pos);
            err = "zip: 中央目录 extra 偏移溢出";
            return -1;
        }

        DngZipEntry ent;
        ent.method = method;

        if (nlen == 0) {
            // 无名条目(规范上不允许):跳过它,但必须先把 pos 推到本条末尾,
            // 否则后续每一项都在错位(把名字/extra 当作 CD 头解析)。
            av_log(nullptr, AV_LOG_WARNING,
                   "dngzip: 中央目录出现无名条目,跳过(pos=%llu)\n",
                   (unsigned long long)pos);
            pos = next_pos;
            continue;
        }

        // 中央目录头只包含 filename 长度;必须读取实际 filename,否则 ent.name
        // 保持空串,后续 frame/.dng 匹配和诊断日志都会失效。
        uint64_t name_off = 0;
        if (!checked_add_u64(pos, 46u, name_off)) {
            av_log(nullptr, AV_LOG_ERROR,
                   "dngzip: 中央目录 filename 偏移计算溢出(pos=%llu)\n",
                   (unsigned long long)pos);
            err = "zip: 中央目录 filename 偏移溢出";
            return -1;
        }
        std::vector<uint8_t> name_bytes((size_t)nlen);
        if (pread_at(name_off, name_bytes.data(), name_bytes.size()) !=
            (int64_t)name_bytes.size()) {
            av_log(nullptr, AV_LOG_ERROR,
                   "dngzip: 读中央目录 filename 失败(pos=%llu, nlen=%u, len=%lld)\n",
                   (unsigned long long)pos, (unsigned)nlen, (long long)m_len);
            err = "zip: 读中央目录 filename 失败(文件可能被截断)";
            return -1;
        }
        ent.name.assign(reinterpret_cast<const char *>(name_bytes.data()),
                        name_bytes.size());

        uint64_t csize = csize32, usize = usize32, lho = lho32;
        bool have_csize = csize32 != kU32Max;
        bool have_usize = usize32 != kU32Max;
        bool have_lho = lho32 != kU32Max;
        // ZIP64 extra(id 0x0001):8 字节字段按「对应 32 位值是否为 0xFFFFFFFF」
        // 依次出现(APPNOTE 固定顺序,可能存在性条件不同);越界即停。
        if (xlen > 0) {
            std::vector<uint8_t> xb((size_t)xlen);
            // L-1:原来 pread 失败会静默 continue,usize/csize/lho 保持 0xFFFFFFFF,
            // 之后只报一句含糊的「local header 失败」,更糟的是 ZIP64 extra 按
            // sentinel 顺序消费,可能把错的数据当成帧数据解出来。契约 §10 要求
            // 失败必须 av_log、不静默:这里直接 open() 失败返回 -1。
            if (pread_at(extra_off, xb.data(), xb.size()) != (int64_t)xb.size()) {
                av_log(nullptr, AV_LOG_ERROR,
                       "dngzip: 读 ZIP64 extra 失败(pos=%llu, nlen=%u, xlen=%u, len=%lld)\n",
                       (unsigned long long)pos, (unsigned)nlen, (unsigned)xlen,
                       (long long)m_len);
                err = "zip: 读中央目录 extra 字段失败(文件可能被截断)";
                return -1;
            }
            size_t p = 0;
            while (p + 4 <= xb.size()) {
                uint16_t id = rd16(xb.data() + p);
                uint16_t sz = rd16(xb.data() + p + 2);
                if (p + 4 + sz > xb.size() || sz == 0) break;
                if (id == 0x0001) {
                    const uint8_t *v = xb.data() + p + 4;
                    const size_t sz_left = sz;              // extra 字段载荷长度
                    size_t q = 0;
                    if (usize32 == kU32Max && q + 8 <= sz_left) { usize = rd64(v + q); q += 8; have_usize = true; }
                    if (csize32 == kU32Max && q + 8 <= sz_left) { csize = rd64(v + q); q += 8; have_csize = true; }
                    if (lho32  == kU32Max && q + 8 <= sz_left) { lho  = rd64(v + q); q += 8; have_lho = true; }
                    break;            // 只需第一个 ZIP64 extra
                }
                p += 4 + sz;
            }
        }
        if (!have_usize || !have_csize || !have_lho) {
            av_log(nullptr, AV_LOG_ERROR,
                   "dngzip: ZIP64 extra 缺少必要字段(pos=%llu)\n",
                   (unsigned long long)pos);
            err = "zip: ZIP64 extra 缺少必要字段";
            return -1;
        }
        ent.csize = csize;
        ent.usize = usize;
        ent.lho = lho;
        ent.crc = crc32_cd;

        const std::string base_name = basename_of(ent.name);
        bool is_audio = base_name.size() == 9;
        if (is_audio) {
            const char *want = "audio.wav";
            for (size_t bi = 0; bi < base_name.size(); bi++) {
                if (lower_ascii(base_name[bi]) != want[bi]) {
                    is_audio = false;
                    break;
                }
            }
        }
        if (is_audio && ent.usize > 0) {
            if (!m_have_audio) {
                m_audio = ent;
                m_have_audio = true;
            } else {
                av_log(nullptr, AV_LOG_WARNING,
                       "dngzip: 发现重复 audio.wav,忽略条目 %s\n", ent.name.c_str());
            }
        }

        // 帧条目:MotionCam 约定 frame-#####.dng;但导出工具/版本会用
        // frame_000000.dng 之类。写死的正则漏掉它们 → 整包 0 帧 → 「不可解析」
        // (2026-10-04 真机反馈)。因此:
        //   * 主匹配 frame[_-]?\d+\.dng(位数不写死);
        //   * 一个都没匹配上时,回退成「所有 .dng 条目」(仍是 zip → 仍是 DNG 序列);
        //   * 非 .dng 的条目(readme/txt/音频)一律忽略。
        const bool is_frame = is_dng_frame_name(ent.name);
        const bool is_dng = is_frame || has_dng_ext(ent.name);
        if (is_dng) {
            if (is_frame) frames_named.push_back(ent);
            if (usize > 0 || csize > 0 || ent.method == 8) dng_all.push_back(ent);
        }
        if (dng_seen < kLogMaxNames) {
            log_names += (log_names.empty() ? "" : ", ") + ent.name;
            dng_seen++;
        }

        // 中央目录项总长 = 46 + name + extra + comment;next_pos 已 checked。
        pos = next_pos;
    }

    // ---- 4. 取帧表:优先 frame-NNNNN 命中,否则回退全部 .dng -------------------
    // 用 move 而不是拷贝:每帧条目几百字节,900 帧也就百来 KB。
    std::vector<DngZipEntry> found = std::move(frames_named);
    if (found.empty()) {
        found = std::move(dng_all);
        if (!found.empty()) {
            av_log(nullptr, AV_LOG_INFO,
                   "dngzip: 没有 frame-NNNNN.dng 条目,回退成 zip 内全部 .dng 共 %zu 帧\n",
                   found.size());
        }
    } else {
        av_log(nullptr, AV_LOG_INFO,
               "dngzip: 命中 frame-NNNNN.dng 条目 %zu 帧(另见 .dng %zu 个)\n",
               found.size(), dng_all.size());
    }

    // 自然序排序(零填充混杂时退回数值相等 + 字典序 suffices)—— 绝不字典序!
    std::sort(found.begin(), found.end(),
              [](const DngZipEntry &a, const DngZipEntry &b) {
                  return natural_less(a.name, b.name);
              });
    if (found.empty()) {
        av_log(nullptr, AV_LOG_WARNING,
               "dngzip: zip 里没有任何 .dng 帧(EOCD/CD 解析成功);前几个条目名: [%s]\n",
               log_names.c_str());
    }
    m_frames = std::move(found);
    return 0;
}

int DngZip::read_frame(size_t i, std::vector<uint8_t> &out, std::string &err) {
    out.clear();
    if (m_fd < 0) {
        err = "zip: 未 open";
        return -1;
    }
    if (i >= m_frames.size()) {
        err = "zip: 帧下标越界";
        return -1;
    }
    const DngZipEntry &ent = m_frames[i];
    const size_t ordinal = i + 1;   // 用户视角 1 起

    // ---- 本地头(签名 + 本地 name_len/extra_len;本地 extra 可能与 CD 不同) ------
    const uint64_t file_len = (uint64_t)m_len;
    if (ent.lho > file_len || 30u > file_len - ent.lho) {
        err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
              ")解压失败: 本地头范围越界(lho=" + std::to_string(ent.lho) + ")";
        av_log(nullptr, AV_LOG_ERROR, "dngzip: %s\n", err.c_str());
        return -1;
    }
    uint8_t lh[30];
    if (pread_at(ent.lho, lh, sizeof(lh)) != (int64_t)sizeof(lh)) {
        err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
              ")解压失败: 读本地头失败(lho=" + std::to_string(ent.lho) + ")";
        av_log(nullptr, AV_LOG_ERROR, "dngzip: %s\n", err.c_str());
        return -1;
    }
    if (rd32(lh) != kSigLocal) {
        err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
              ")解压失败: 本地头签名不符(不是 0x04034b50)";
        av_log(nullptr, AV_LOG_ERROR, "dngzip: %s\n", err.c_str());
        return -1;
    }
    const uint16_t nlen = rd16(lh + 26);
    const uint16_t xlen = rd16(lh + 28);
    uint64_t data_off = 0;
    if (!checked_add_u64(ent.lho, 30u, data_off) ||
        !checked_add_u64(data_off, (uint64_t)nlen, data_off) ||
        !checked_add_u64(data_off, (uint64_t)xlen, data_off) ||
        data_off > file_len) {
        err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
              ")解压失败: 数据偏移越界";
        av_log(nullptr, AV_LOG_ERROR, "dngzip: %s\n", err.c_str());
        return -1;
    }
    // 剩余可用字节(以本地头为起点,防止 usize/csize 撒谎)。压缩数据也
    // 只能在其中央目录声明的 csize 范围内读取,否则可能吞入后续 ZIP 区域。
    const uint64_t avail = file_len - data_off;
    if (ent.csize > avail) {
        err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
              ")解压失败: csize 超出可用数据范围(csize=" +
              std::to_string(ent.csize) + ", avail=" + std::to_string(avail) + ")";
        av_log(nullptr, AV_LOG_ERROR, "dngzip: %s\n", err.c_str());
        return -1;
    }

    if (ent.usize > kMaxFrameUsize) {
        err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
              ")解压失败: usize 过大(" + std::to_string(ent.usize) + " 字节)";
        av_log(nullptr, AV_LOG_ERROR, "dngzip: %s\n", err.c_str());
        out.clear();
        return -1;
    }

    if (ent.method == 0) {
        // stored 数据没有解压过程,压缩前后长度必须一致,且不能超出文件剩余范围。
        if (ent.csize != ent.usize || ent.usize > avail) {
            err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
                  ")解压失败: stored 尺寸无效(csize=" +
                  std::to_string(ent.csize) + ", usize=" +
                  std::to_string(ent.usize) + ", avail=" +
                  std::to_string(avail) + ")";
            av_log(nullptr, AV_LOG_ERROR, "dngzip: %s\n", err.c_str());
            out.clear();
            return -1;
        }
        out.resize(ent.usize);
        uLong crc = crc32(0L, Z_NULL, 0);
        uint64_t left = ent.usize, done = 0;
        while (left > 0) {
            size_t want = (size_t)std::min<uint64_t>(left, kInflateChunk);
            uint64_t read_off = 0;
            if (!checked_add_u64(data_off, done, read_off)) {
                err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
                      ")解压失败: stored 数据偏移溢出";
                av_log(nullptr, AV_LOG_ERROR, "dngzip: %s\n", err.c_str());
                out.clear();
                return -1;
            }
            ssize_t r = pread_at(read_off, out.data() + done, want);
            if (r != (ssize_t)want) {
                err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
                      ")解压失败: stored 短读";
                av_log(nullptr, AV_LOG_ERROR, "dngzip: %s\n", err.c_str());
                out.clear();
                return -1;
            }
            crc = crc32(crc, out.data() + done, want);
            done += want;
            left -= want;
        }
        if ((uint32_t)crc != ent.crc) {
            err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
                  ")解压失败: CRC-32 不符(期望 " + hex32(ent.crc) +
                  ", 实际 " + hex32((uint32_t)crc) + ")";
            av_log(nullptr, AV_LOG_ERROR, "dngzip: %s\n", err.c_str());
            out.clear();
            return -1;
        }
        return 0;
    }

    out.resize(ent.usize);

    if (ent.method != 8) {
        err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
              ")解压失败: 不支持的压缩方法 " + std::to_string((unsigned)ent.method);
        av_log(nullptr, AV_LOG_ERROR, "dngzip: %s\n", err.c_str());
        out.clear();
        return -1;
    }
    if (ent.usize > 0x7FFFFFFFull) {
        // inflate 的 avail_out 是 uInt,单帧 DNG 不该有这个量级;拦掉防溢出
        err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
              ")解压失败: usize 过大(" + std::to_string((unsigned long long)ent.usize) + ")";
        out.clear();
        return -1;
    }

    // ---- deflate:raw inflate,1MB 一块喂,复用一次 z_stream ----------------------
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -15) != Z_OK) {          // -15 = 裸 deflate,无 zlib/gzip 头
        err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
              ")解压失败: inflateInit2";
        out.clear();
        return -1;
    }

    std::vector<uint8_t> inbuf(kInflateChunk);
    uint64_t src_done = 0;
    uint64_t crc_done = 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    int zrc = Z_OK;
    bool failed = false;

    zs.next_out = out.data();
    zs.avail_out = (uInt)ent.usize;                    // 上限即 usize,防止恶意 usize

    while (!failed) {
        if (zs.avail_in == 0) {
            if (src_done >= ent.csize) {                // 喂完声明的 csize 还没到流尾
                err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
                      ")解压失败: 压缩数据不足";
                failed = true;
                break;
            }
            uint64_t left = ent.csize - src_done;
            size_t want = (size_t)std::min<uint64_t>(left, (uint64_t)kInflateChunk);
            uint64_t read_off = 0;
            if (!checked_add_u64(data_off, src_done, read_off)) {
                err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
                      ")解压失败: 压缩数据偏移溢出";
                failed = true;
                break;
            }
            ssize_t r = pread_at(read_off, inbuf.data(), want);
            if (r <= 0) {
                err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
                      ")解压失败: 读压缩数据失败";
                failed = true;
                break;
            }
            src_done += (uint64_t)r;
            zs.next_in = inbuf.data();
            zs.avail_in = (uInt)r;
        }

        zrc = inflate(&zs, Z_NO_FLUSH);
        const uLong produced = zs.total_out - crc_done;
        if (produced > 0) {
            crc = crc32(crc, out.data() + crc_done, (uInt)produced);
            crc_done += produced;
        }
        if (zrc == Z_STREAM_END) break;
        if (zrc == Z_OK) {
            if (zs.avail_out == 0) {                   // 解出的比 usize 还多
                err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
                      ")解压失败: 解压结果大于声明的 usize";
                failed = true;
                break;
            }
            continue;                                  // 继续喂
        }
        if (zrc == Z_BUF_ERROR) {
            // 输出满了但流没结束(见上)或输入暂时不够 —— 上面两个分支已覆盖
            err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
                  ")解压失败: inflate Z_BUF_ERROR";
            failed = true;
            break;
        }
        {
            const char *why = zs.msg ? zs.msg : "unknown";
            err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
                  ")解压失败: " + why;
        }
        failed = true;
        break;
    }

    const uint64_t consumed_csize = src_done - (uint64_t)zs.avail_in;
    inflateEnd(&zs);
    if (failed) {
        out.clear();
        av_log(nullptr, AV_LOG_ERROR, "dngzip: %s\n", err.c_str());
        return -1;
    }
    if (zrc != Z_STREAM_END) {
        err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
              ")解压失败: deflate 流未完整结束";
        out.clear();
        return -1;
    }
    if (zs.total_out != ent.usize) {
        err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
              ")解压失败: 解出长度不符(usize=" + std::to_string((unsigned long long)ent.usize) +
              ", 实际=" + std::to_string((unsigned long long)zs.total_out) + ")";
        out.clear();
        return -1;
    }
    if (consumed_csize != ent.csize) {
        err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
              ")解压失败: 压缩尺寸不符(csize=" +
              std::to_string((unsigned long long)ent.csize) + ", 实际消费=" +
              std::to_string((unsigned long long)consumed_csize) + ")";
        out.clear();
        av_log(nullptr, AV_LOG_ERROR, "dngzip: %s\n", err.c_str());
        return -1;
    }
    if ((uint32_t)crc != ent.crc) {
        err = "zip 第 " + std::to_string(ordinal) + " 帧(" + ent.name +
              ")解压失败: CRC-32 不符(期望 " + hex32(ent.crc) +
              ", 实际 " + hex32((uint32_t)crc) + ")";
        av_log(nullptr, AV_LOG_ERROR, "dngzip: %s\n", err.c_str());
        out.clear();
        return -1;
    }
    return 0;
}

int DngZip::read_audio(std::vector<uint8_t> &out, std::string &err) {
    out.clear();
    if (m_fd < 0) {
        err = "zip: 未 open";
        return -1;
    }
    if (!m_have_audio) {
        err = "zip: 未找到 audio.wav";
        return -1;
    }
    if (m_audio.usize == 0 || m_audio.usize > kMaxAudioUsize) {
        err = "zip audio.wav 解压失败: usize 无效或过大(" +
              std::to_string((unsigned long long)m_audio.usize) + ")";
        return -1;
    }

    // read_frame() 和 audio.wav 共用完全相同的本地头、store/deflate、尺寸和
    // EOF 校验。临时把音频条目放入一个只有一项的帧表，避免复制另一份容易
    // 漂移的 ZIP 解压实现；调用期间 DngZip 不会被其他线程访问。
    std::vector<DngZipEntry> saved;
    saved.swap(m_frames);
    m_frames.push_back(m_audio);
    const int rc = read_frame(0, out, err);
    m_frames.swap(saved);
    if (rc != 0) {
        err = "zip audio.wav 解压失败: " + err;
        return rc;
    }
    if (out.size() > kMaxAudioUsize) {
        err = "zip audio.wav 解压失败: usize 过大(" +
              std::to_string((unsigned long long)out.size()) + ")";
        out.clear();
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// LibRaw 单帧解码
// ---------------------------------------------------------------------------
namespace {

// 契约 §10.4:注意 no_auto_bright 必须显式置 1 —— LibRaw 默认会给半尺寸解算做
// 一次提亮(改变曝光语义),不关就和桌面参考实现对不上。
// want_half_size(N-08):预览路径显式请求半尺寸解算。与 dngq=FAST 内建的
// half_size=1 是两件事 —— FAST 同时换 demosaic(user_qual=0),本开关只降采样,
// 档位画质语义不变(否则预览会把 BALANCED/FINE 的成品画质谎报成 FAST 档)。
// LibRaw 0.21.4:imgdata.params.half_size 与 half_size 的 demosaic 绑定在读
// raw 阶段,对 half_size 输出没有任何后处理分支,true→1 / false→0 显式赋值
// 即可,复用时由 tlr_params0 快照每帧复位(见 dng_decode_to_rgb48 注释)。
void apply_libraw_params(LibRaw &lr, DngQuality q, DngColorMode cm, DngMap map,
                         bool want_half_size) {
    lr.imgdata.params.output_bps = 16;
    // 1 = sRGB(既有档),8 = Rec.2020 线性(Apple Log 档在滤镜中编码曲线)
    lr.imgdata.params.output_color = (map == DngMap::APPLE_LOG) ? 8 : 1;
    lr.imgdata.params.no_auto_bright = 1;
    lr.imgdata.params.use_camera_wb = 0;
    lr.imgdata.params.use_auto_wb = 0;
    lr.imgdata.params.gamm[0] = 0;
    lr.imgdata.params.gamm[1] = 0;
    lr.imgdata.params.no_interpolation = 0;
    // N-08:预览显式要半尺寸解的开关(默认关)。必须**无条件**赋值:
    // false 也要写 0,否则 FAST 档留下的 half_size=1 残影会跟着 LibRaw 实例
    // 一路带到 BALANCED/FINE(dngzip.cpp 的复用陷阱,见 dng_decode_to_rgb48）。
    lr.imgdata.params.half_size = (want_half_size || q == DngQuality::FAST) ? 1 : 0;

    switch (q) {
        case DngQuality::FAST:
            lr.imgdata.params.half_size = 1;   // 半尺寸解算:Bayer 直接读到 RGB
            lr.imgdata.params.user_qual = 0;
            break;
        case DngQuality::BALANCED:
            lr.imgdata.params.user_qual = 2;   // PPG
            break;
        case DngQuality::FINE:
            lr.imgdata.params.user_qual = 3;   // AHD
            break;
    }

    switch (cm) {
        case DngColorMode::CAMERA:
            lr.imgdata.params.use_camera_wb = 1;
            break;
        case DngColorMode::AUTO:
            lr.imgdata.params.use_auto_wb = 1;
            break;
        case DngColorMode::D65:
            // 固定日光:不动相机 WB,user_mul 全 1
            lr.imgdata.params.user_mul[0] = 1.0f;
            lr.imgdata.params.user_mul[1] = 1.0f;
            lr.imgdata.params.user_mul[2] = 1.0f;
            lr.imgdata.params.user_mul[3] = 1.0f;
            break;
    }
}

} // namespace

int dng_decode_to_rgb48(const uint8_t *buf, size_t size,
                        DngQuality q, DngColorMode cm,
                        std::vector<uint16_t> &out_rgb,
                        int &w, int &h, std::string &err,
                        int *raw_w_out, int *raw_h_out, DngMap map,
                        bool want_half_size) {
    out_rgb.clear();
    w = 0;
    h = 0;
    if (raw_w_out) *raw_w_out = 0;
    if (raw_h_out) *raw_h_out = 0;
    if (!buf || size == 0) {
        err = "DNG: 空 buffer";
        return -1;
    }

#if PRF_LIBRAW_REUSE
    // ===== P1-2:worker-local LibRaw 实例复用(方案 P1-2 / 复核报告 §8.2)=====
    // 每个工作线程一个常驻 LibRaw:同一解码 worker 线程跨帧复用,只有线程
    // 局部实例为空时才构造,消除每帧 make_unique + 析构 与随之而来的
    // ~125MB 缓冲反复 malloc/free 抖动。
    //
    // 帧间状态清理(源码依据,LibRaw 0.21.4):
    //   recycle 语义 = src/utils/init_close_utils.cpp:131 LibRaw::recycle():
    //     recycle_datastream()(utils_libraw.cpp:278,只删 open_buffer 自建的
    //     内部 datastream)+ free image/raw_alloc/thumbnail/meta_data/
    //     histogram/oprofile/color.profile/ph1_*/xmpdata + ZERO imgdata 的
    //     sizes/idata/color/lens/other/rawdata/shootinginfo/thumbnail/
    //     thumbs_list/MN/libraw_internal_data;**不重置 imgdata.params**。
    //     → 成功帧把常驻内存从 ~125MB 回落到对象级;params 跨 recycle 存活,
    //     但每帧先用「构造默认快照」复位、再由 apply_libraw_params() 设置
    //     当前档位字段(见下方 tlr_params0 注释),因此 WB/exposure/color/
    //     size/metadata 不会跨帧污染。
    //   成功路径仍在解码后调用 lr->recycle()(契约 §10.3),open_buffer/
    //   unpack/dcraw_process 调用顺序与原实现完全一致。
    //
    // 失败路径(方案 P1-2「失败、取消、尺寸异常后丢弃并重建实例,而不是复用
    // 不确定状态」):除函数尾部成功路径外的一切出口 —— open_buffer/unpack/
    // dcraw_process 返回码非 0、尺寸异常、无 image、尺寸溢出、抛异常 —— 都在
    // 作用域退出时 reset 线程局部实例,下次调用重新构造。守卫用「默认丢弃、
    // 成功才保留」的写法,失败 return 与 catch 分支都自动覆盖,无需逐处改。
    // 参数复位(与 recycle 不重置 params 配套,方案 P1-2「帧间不污染」):
    // apply_libraw_params() 只设置当前档位涉及的字段 + N-08 的显式
    // want_half_size(half_size 在 FAST 档或显式要求时置 1,否则置 0;
    // user_mul 仅 D65 档设置)。新建实例
    // 时这些未设置字段取自构造函数默认值,复用实例却会把上一帧的值带过来
    // —— 实测:FAST 帧之后跟 BALANCED/FINE 帧会残留 half_size=1 走半尺寸
    // 解算,尺寸与内容都漂。因此构造时快照「构造默认 params」,之后每帧先
    // 恢复快照再走 apply_libraw_params,保证复用路径与 PRF_LIBRAW_REUSE=0
    // 的每帧新建路径逐字节等价(host bench_reuse A/B 实测确认)。
    thread_local std::unique_ptr<LibRaw> tlr;
    thread_local libraw_output_params_t tlr_params0{};
    thread_local bool tlr_params0_valid = false;
    if (!tlr) {
        tlr = std::make_unique<LibRaw>();
        tlr_params0 = tlr->imgdata.params;   // 构造默认值快照
        tlr_params0_valid = true;
    }
    if (tlr_params0_valid) tlr->imgdata.params = tlr_params0;
    LibRaw *lr = tlr.get();
    struct LibRawReuseGuard {
        std::unique_ptr<LibRaw> &slot;
        bool keep = false; // 仅成功路径(已 recycle 复位)置 true
        ~LibRawReuseGuard() { if (!keep) slot.reset(); }
    } lr_reuse_guard{tlr};
#else
    // PRF_LIBRAW_REUSE=0:回退原行为 —— 每帧新建/析构一个 LibRaw。
    auto lr = std::make_unique<LibRaw>();
#endif

    try {
        apply_libraw_params(*lr, q, cm, map, want_half_size);

        int rc = lr->open_buffer((void *)buf, size);
        if (rc != LIBRAW_SUCCESS) {
            err = std::string("DNG: LibRaw 打开失败: ") + lr->strerror(rc);
            return -1;
        }

        const uint32_t raw_w = (uint32_t)lr->imgdata.sizes.width;
        const uint32_t raw_h = (uint32_t)lr->imgdata.sizes.height;
        if (raw_w == 0 || raw_h == 0 || raw_w > 65536 || raw_h > 65536) {
            lr->recycle();
            err = "DNG: LibRaw 报出的尺寸异常(" + std::to_string(raw_w) + "x" +
                  std::to_string(raw_h) + ")";
            return -1;
        }
        if (raw_w_out) *raw_w_out = (int)raw_w;
        if (raw_h_out) *raw_h_out = (int)raw_h;

        rc = lr->unpack();
        if (rc != LIBRAW_SUCCESS) {
            err = std::string("DNG: LibRaw unpack 失败: ") + lr->strerror(rc);
            lr->recycle();
            return -1;
        }

        rc = lr->dcraw_process();
        if (rc != LIBRAW_SUCCESS) {
            err = std::string("DNG: LibRaw dcraw_process 失败: ") + lr->strerror(rc);
            lr->recycle();
            return -1;
        }

        // dcraw_process 之后 imgdata.image 是 [h][w][4](每像素 4 个 ushort 交错)。
        // sRGB 输出时前 3 个就是 R/G/B,第 4 个是拾遗通道(相机 4 色 CFA 才有意义),
        // 契约 §10.3 明确"取前 3 通道"。
        const int fw = (int)lr->imgdata.sizes.width;
        const int fh = (int)lr->imgdata.sizes.height;
        const ushort(*img)[4] = (const ushort(*)[4])lr->imgdata.image;
        if (!img) {
            err = "DNG: LibRaw 没有输出 image";
            lr->recycle();
            return -1;
        }

        if (fw <= 0 || fh <= 0 ||
            static_cast<size_t>(fw) > std::numeric_limits<size_t>::max() /
                static_cast<size_t>(fh)) {
            lr->recycle();
            err = "DNG: 输出尺寸溢出";
            return -1;
        }
        const size_t pixels = static_cast<size_t>(fw) * static_cast<size_t>(fh);
        if (pixels > std::numeric_limits<size_t>::max() / 3) {
            lr->recycle();
            err = "DNG: 输出尺寸溢出";
            return -1;
        }
        out_rgb.resize(pixels * 3);
        for (int y = 0; y < fh; y++) {
            const ushort *src = (const ushort *)(img + (size_t)y * (size_t)fw);
            uint16_t *dst = out_rgb.data() + (size_t)y * (size_t)fw * 3;
            for (int x = 0; x < fw; x++) {
                dst[3 * x + 0] = (uint16_t)src[4 * x + 0];
                dst[3 * x + 1] = (uint16_t)src[4 * x + 1];
                dst[3 * x + 2] = (uint16_t)src[4 * x + 2];
            }
        }
        w = fw;
        h = fh;

        lr->recycle();      // 契约 §10.3:不 recycle 会按帧累积内存
#if PRF_LIBRAW_REUSE
        lr_reuse_guard.keep = true; // 帧间状态已复位:下帧复用同一实例
#endif
        return 0;
    } catch (const std::exception &e) {
        err = std::string("DNG: LibRaw 异常: ") + e.what();
        return -1;
    } catch (...) {
        err = "DNG: LibRaw 未知异常";
        return -1;
    }
}

} // namespace proresforge
