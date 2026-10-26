// dngframeq.h
//
// DNG 序列 zip 的**并行帧解码队列**(契约 §10.3 的性能支柱)。
//
// 为什么需要它:zip 路径原来是「read_frame → LibRaw decode → sws → encode」全串行,
// 每帧 0.35s 的解码占掉 ~86% 耗时(实测 4096x3072 BALANCED:read 0.056s / decode 0.351s),
// 于是整条转码只有一个核在跑,335 帧光解码就要 ~118s(本机 x86_64;arm 设备更慢)。
//
// 设计:N-1 个工作线程并行解码(每线程自己的 LibRaw 实例,LibRaw 0.21 各实例互相独立),
// 主线程只负责「按帧号顺序取解好的帧 → 滤镜图(sws)→ 编码 → 写盘」。解码与编码
// 天然重叠,多核同时吃满。
//
// 关键约束与对应做法:
//   * **内存**:一帧在飞峰值 ≈ rgb 字节数 × 3.0(实测 4096x3072 4096*3072*3*2=72MB
//     → VmHWM 217MB,即 LibRaw 内部 raw 25MB + image 100MB + dng buf 15.7MB + rgb 72MB,
//     部分 malloc 块 recycle 后不还给内核)。因此环形队列深度 depth 与线程数都由
//     「内存预算 / 单帧峰值」推算,而不是无脑开满核(vm_budget_kb 控制)。
//   * **顺序**:ProRes 需要恒定分辨率与严格递增 pts,所以消费端严格按帧号出队
//     (槽位带 seq,拒绝读到上一轮的残留 ready)。
//   * **失败不静默**:任一帧解不开立刻把错误扩散到主线程并唤醒所有等待方
//     (契约 §10.3「失败时绝不静默跳帧」)。
//   * **取消**:g_cancel 由主循环轮询;这里 cancel() 只负责把阻塞的线程放出来,
//     正在解码的线程允许把当前帧做完再退(单帧 <1s),所以 stop() 一定要被调用到。
//   * **线程簿记必须数对容器**(2026-10-05 真机死锁的教训):POSIX 路径的线程在
//     m_threads,std::thread 退路在 m_workers。`thread_count()` 以前只数 m_workers,
//     于是恒为 0,next() 在环满时和 worker 一起睡死在 cv_prod
//     —— 335 帧一帧都交不出去,真机表现就是「卡在转码中(初始化)」。任何「有没有
//     工作线程」的判断都要走 thread_count(),不要直接摸某个容器。
//     (2026-10-07 N-05 后续:零线程不再降级成「消费线程亲自解码」,start()
//      直接返回 false —— 见 start() 内注释。)

#ifndef PRORESFORGE_DNGFRAMEQ_H
#define PRORESFORGE_DNGFRAMEQ_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 工作线程用 pthread 显式创建而不是 std::thread:std::thread 拿不到栈大小,
// 而 Android(bionic)给新建原生线程的默认栈只有 **1MB**
// (bionic/pthread_create.cpp: PTHREAD_STACK_SIZE_DEFAULT = 1024*1024)。
// dng_decode_to_rgb48() 把 sizeof(LibRaw)=767,320 的实例放在堆上,避免单个栈对象
// 吃掉默认栈的大部分空间。这里仍显式给 8MB,为 LibRaw/FFmpeg 的深调用链保留余量
// (主线程本来就在跑同样的代码)。
// std::thread 只作为非 POSIX 平台(如 Windows host 调试)的退路。
#if defined(__linux__) || defined(__ANDROID__)
#define PRFQ_USE_PTHREAD 1
#include <pthread.h>
#endif

// next() 卡住时的诊断输出。Android 下自动启用(avutil 一定在链接图里);
// host 侧测试可以用 -DPRFQ_USE_AV_LOG 自己打开(见 test_stalllog.cpp)。
#if defined(__ANDROID__) || defined(PRFQ_USE_AV_LOG)
extern "C" {
#include <libavutil/log.h>
}
#define PRFQ_AVLOG(level, ...) av_log(nullptr, level, __VA_ARGS__)
#else
#define PRFQ_AVLOG(level, ...) ((void)0)
#endif

namespace proresforge {

// 单帧解码结果:rgb48le(w*h*3 个 uint16)。rgb 的所有权随帧在队列里转移(move)。
/**
 * 一条解码出的帧:队列线程把结果写进它,主线程 swap 出来用。
 * 两个 vector 的容量都在槽位里跨轮复用 —— 335 帧少 670 次大块 mmap/munmap。
 */
struct DngFrame {
    std::vector<uint8_t> raw;      ///< zip 里读出的原始 DNG 字节(压缩态)
    std::vector<uint16_t> rgb;     ///< rgb48le,长度 w*h*3
    int w = 0;
    int h = 0;
};

class DngFrameQueue {
public:
    // 解码一帧:0 成功(<0 失败,err 带原因)。允许在多个线程上并发调用,
    // 但同一时刻不会有两个线程拿到同一个 index。
    using DecodeFn = std::function<int(size_t index, DngFrame &out, std::string &err)>;

    DngFrameQueue() = default;
    ~DngFrameQueue() { stop(); }

    DngFrameQueue(const DngFrameQueue &) = delete;
    DngFrameQueue &operator=(const DngFrameQueue &) = delete;

    /**
     * 按给定线程数/队列深度预取解码 total_frames 帧。
     * nthreads 至少 1(主线程自己也能算一个消费端,但解码在工作线程做);
     * depth 至少 1。线程创建失败时安静降级成已创建的那些(至少保住 1 条);
     * 但「连 8MB 工作线程栈都保证不了」是硬失败(见 L-6),直接返回 false。
     *
     * @return true = 已就绪(工作线程数 ≥1,或降级到消费线程亲自解码);
     *         false = 无法安全启动(last_error() 有原因),上层按「打开失败」
     *         处理,绝不能带着没线程的队列进主循环。
     */
    bool start(size_t total_frames, DecodeFn fn, int nthreads, size_t depth);

    // next() 的返回码。历史上只有 0 / >0 / <0 三档,而 >0 同时表示「被取消」
    // 和「队列已空(全部帧交付完)」,调用方只能靠 err 字符串猜,于是正常收尾
    // 可能被显成「已取消」(任务实际失败/完成却被标成取消)。现在显式区分。
    enum {
        kNextOk        = 0,   ///< 成功:帧已 move 进 out
        kNextCancelled = 1,   ///< 被 cancel():不会有更多帧交付
        kNextDrained   = 2    ///< total_frames 已全部交付(正常收尾)
    };

    /**
     * 把已解好的首帧预置进队列。仅 start() 之前调用一次(重复调用/启动后调用
     * 一律返回 false,是调用方 bug)。
     *
     * 场景:zip init 段已经 read_frame(0)+dng_decode_to_rgb48 解过 seq=0 拿
     * 尺寸。那一帧直接喂给消费者,worker 就从 seq=1 开始派帧,省掉整个
     * 任务里最贵的一帧重复解码(4096×3072 实测 ~0.4s/帧)。
     *
     * 实现:帧暂存到 start() 应用 —— m_slots/m_depth 要到 start() 才知道。
     * start() 里把它 move 进 slot0(seq=0、ready=true)并把 m_assigned 置 1。
     * total_frames==1 时 next() 第一轮即可消费它:worker 侧 m_assigned 已
     * ≥ m_total,produce_one() 立即返回 false,worker 空转退出,不会卡。
     * 取消(m_cancelled)/失败(m_failed)扩散语义完全不变。
     *
     * @return false = 已 seed 过或 start() 之后调用(忽略)。
     */
    bool seed(DngFrame &&f);

    /**
     * 按帧号顺序取下一帧解好的结果。
     * @return kNextOk(帧已 move 进 out)/ kNextCancelled / kNextDrained;
     *         <0 失败(err 有原因)。
     */
    int next(DngFrame &out, std::string &err);

    /** 请求取消:唤醒所有阻塞方,工作线程做完当前帧后退出。幂等。 */
    void cancel();

    /** join 所有工作线程。start 之后、析构之前必须调用(或由析构兜底)。 */
    void stop();

    // 真实在跑的工作线程数(消费线程不算)。
    // **POSIX 路径的线程在 m_threads,std::thread 退路在 m_workers,两边都要数**
    // —— 只数 m_workers 会得到恒 0,于是 start() 把「已经起了 4 条 pthread」误判成
    // 「一条都没有」而打开 next() 里的消费线程亲自解码降级:环满时该线程阻塞在
    // m_cv_prod 等空位,而 ready 的队首帧没人去 m_cv_cons 取,最终全部线程睡在
    // cv_prod → 一帧都交不出去。真机表现:卡在「转码中(初始化)」零进度(2026-10-05
    // 实测复现:335 帧的任务在 next() 第一次调用就死锁,gdb/看门狗确认)。
    int  thread_count() const {
        return (int)(m_workers.size()
#if defined(PRFQ_USE_PTHREAD)
                     + m_threads.size()
#endif
                     );
    }
    size_t depth() const { return m_depth; }
    /** start() 返回 false 的原因(帧解码失败的原因走 next() 的 err 出)。 */
    const std::string &last_error() const { return m_err; }

private:
    bool produce_one();
    void worker_loop();
#if defined(PRFQ_USE_PTHREAD)
    static void *thread_trampoline(void *arg)
    {
        static_cast<DngFrameQueue *>(arg)->worker_loop();
        return nullptr;
    }
#endif

    DecodeFn m_fn;
    size_t   m_total   = 0;
    size_t   m_depth   = 1;
    int      m_nthread = 1;

    struct Slot {
        DngFrame frame;
        size_t   seq   = 0;   // 属于哪一轮生产(防读到上一代残留的 ready)
        bool     ready = false;
    };
    std::vector<Slot> m_slots;

    mutable std::mutex      m_mu;
    std::condition_variable m_cv_prod;   // 环形队列有空位 / 失败 / 取消
    std::condition_variable m_cv_cons;   // 队首帧解好了 / 失败 / 取消

    size_t m_assigned = 0;   // 已派给工作线程的帧号
    size_t m_consumed = 0;   // 已交给主线程的帧号
    bool   m_failed   = false;
    bool   m_cancelled = false;
    std::atomic<bool> m_cancel_flag{false};
    std::string m_err;

    // seed() 暂存的首帧(m_slots/depth 要到 start() 才定型,故两段式落地)。
    DngFrame m_seed_frame;
    bool     m_seeded = false;   // seed() 已被调用过(只允许一次)

    std::vector<std::thread> m_workers;
#if defined(PRFQ_USE_PTHREAD)
    std::vector<pthread_t> m_threads;
#endif
    // N-06:只有「线程真的建起来」才为真。start() 里任何失败路径(attr 设置失败、
    // 零线程创建成功)都要保持 false,否则 next() 的未启动防护形同虚设。
    bool m_started = false;
};

inline bool DngFrameQueue::seed(DngFrame &&f)
{
    if (m_started || m_seeded) return false;   // 仅 start() 前、仅一次
    m_seed_frame = std::move(f);
    m_seeded = true;
    return true;
}

inline bool DngFrameQueue::start(size_t total_frames, DecodeFn fn, int nthreads, size_t depth)
{
    stop();
    m_total   = total_frames;
    m_fn      = std::move(fn);
    m_nthread = nthreads < 1 ? 1 : nthreads;
    m_depth   = depth < 1 ? 1 : depth;
    // depth 允许大于帧数(环不会绕完一圈),但不能为 0(next() 里有取模)。
    if (m_total && m_depth > m_total) m_depth = m_total;
    m_assigned = 0;
    m_consumed = 0;
    m_failed   = false;
    m_err.clear();
    m_cancel_flag.store(false, std::memory_order_relaxed);
    m_cancelled = false;

    m_slots.clear();
    m_slots.resize(m_depth);

    // seed() 落地的唯一位置: m_depth/m_slots 到这里才定型。帧进 slot0,
    // m_assigned 从 1 开始 —— worker 只会碰到 seq≥1 的帧(produce_one 的
    // 空位判定 (assigned-consumed)<depth 用的是未完成帧数,首帧占一格,
    // 语义上就是「环里已经有一帧待取」)。m_total==0/1 都安全:
    // 0 时 start 的调用方本来也拿不到帧;1 时 worker 见 m_assigned>=m_total
    // 立即退出(produce_one 返回 false),next() 第一轮就消费 slot0。
    if (m_seeded && m_total >= 1 && m_depth >= 1) {
        Slot &s0 = m_slots[0];
        s0.frame = std::move(m_seed_frame);
        s0.seq   = 0;      // next() 谓词 s.seq==m_consumed(0) 立即为真
        s0.ready = true;   // 首帧就绪,worker 从 seq=1 起派
        if (m_assigned < 1) m_assigned = 1;
    }

    // N-06:m_started 必须等到线程真的建起来才置位。放在这里(创建之前)意味着
    // 「attr 设置失败 / pthread_create 全失败」时 m_started 已经是 true,
    // next() 的「未启动」防护(以及 stop() 的复位语义)看到的都是假就绪。
    // 判 true 的位置移到下面线程创建成功之后。
    //
    // N-05 补充:创建循环里的 `break` 不是「安静降级也不是硬失败」,是否决由
    // 线程创建后的零线程检查统一裁决(见函数尾部):至少 1 条线程照常跑。
    const size_t kStack = (size_t)8 * 1024 * 1024;
#if defined(PRFQ_USE_PTHREAD)
    pthread_attr_t attr;
    bool attr_ok = (pthread_attr_init(&attr) == 0);
    if (attr_ok) {
        // L-6:栈大小/分离状态设置失败时**不能**静默退回默认栈。bionic 给新建
        // 原生线程的默认栈只有 1MB(PTHREAD_STACK_SIZE_DEFAULT = 1024*1024),
        // 而 dng_decode_to_rgb48 的调用链(LibRaw 实例 + 解码栈)实测要 ~850KB
        // 以上,1MB 余量极小,一次深递归/大 buffer 就是 SIGSEGV。这里
        // fail-fast:一条线程都不起,让上层按「内部错误」处理,也不要带着「线程数 0 → 消费线程亲自解码」的降级路径走 ——
        // 跑在主线程上,栈大小同样不受我们控制。
        if (pthread_attr_setstacksize(&attr, kStack) != 0 ||
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE) != 0) {
            pthread_attr_destroy(&attr);
            m_err = "无法设置工作线程栈属性(pthread_attr_setstacksize/setdetachstate 失败):";
            m_err += "拒绝降级到默认 1MB 栈跑 LibRaw,启动失败";
            return false;
        }
        for (int i = 0; i < m_nthread; i++) {
            pthread_t t;
            int rc = pthread_create(&t, &attr, &DngFrameQueue::thread_trampoline, this);
            if (rc == 0) {
                m_threads.push_back(t);
            } else {
                break;   // 安静降级
            }
        }
        pthread_attr_destroy(&attr);
    } else {
        m_err = "pthread_attr_init 失败,无法保证 8MB 工作线程栈,启动失败";
        return false;
    }
#else
    for (int i = 0; i < m_nthread; i++) {
        try {
            m_workers.emplace_back([this] { worker_loop(); });
        } catch (...) {
            break;
        }
    }
#endif
    // N-05:一条工作线程都没建起来 = 硬失败。旧代码在这里置 m_no_workers,
    // 让 next() 退化成「消费线程亲自解码」——而那正是 L-6 已经否掉的形状:
    // 消费线程在转码主线程上,栈大小同样不受我们控制,LibRaw 的解码链实测
    // 要 ~850KB;再叠加下面这条注释点出的问题(误判成 0 时环满会和 worker
    // 一起睡死),降级路径没有一处是正确的。正确做法:return false + m_err,
    // 让上层按「打开失败/内部错误」处理。也不能 push pthread_self() 再自 join
    // (会死锁),所以这里既不降级也不自join。
    if (thread_count() == 0) {
        m_err = "无法创建任何工作线程(pthread_create/std::thread 全部失败):";
        m_err += "拒绝降级到消费线程亲自解码,启动失败";
        return false;
    }
    // N-06:到这里至少 1 条线程在跑(见上面 N-5 的零线程硬失败),才配称「已启动」。
    m_started = true;
    return true;
}

inline void DngFrameQueue::worker_loop()
{
    while (produce_one()) {}
}

// 解一帧并挂进环形队列。返回 false = 应该退出(出错/取消/没活了)。
inline bool DngFrameQueue::produce_one()
{
    size_t seq;
    DngFrame *target;   // 目标槽位(借引用,见下面「独占」的论证)
    {
        std::unique_lock<std::mutex> lk(m_mu);
        // 等「有空位」或「没活了」或「出错/取消」。空位判定用未完成帧数
        // (assigned-consumed) 而不是 ready 数:解码中的帧也占一个槽。
        m_cv_prod.wait(lk, [this] {
            return m_failed || m_cancelled || m_assigned >= m_total ||
                   (m_assigned - m_consumed) < m_depth;
        });
        if (m_failed || m_cancelled) return false;
        if (m_assigned >= m_total) return false;
        seq = m_assigned++;
        // 派帧时保证了 (seq - consumed) < depth,所以这个槽此刻没有被任何
        // 「未消费」的帧占用,本线程可以独占它一整轮解码。
        target = &m_slots[seq % m_depth].frame;
    }

    std::string e;
    int rc;
    try {
        // 直接解进槽位:槽位里的 vector 容量是上一代留下来的(dng_decode_to_rgb48
        // 内部只 clear()+resize(),不缩容量),worker 自己不再每帧新建 72MB。
        // next() 用 swap 而非 move 把 buffer 还给槽位,两边都复用同一块。
        rc = m_fn(seq, *target, e);
    } catch (const std::exception &ex) {
        rc = -1; e = std::string("DNG 帧解码抛异常: ") + ex.what();
    } catch (...) {
        rc = -1; e = "DNG 帧解码抛未知异常";
    }
    if (rc != 0) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_failed = true;
        if (m_err.empty()) m_err = e;
        m_cv_prod.notify_all();
        m_cv_cons.notify_all();
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_mu);
        Slot &s = m_slots[seq % m_depth];
        s.seq   = seq;
        s.ready = true;
        m_cv_cons.notify_one();
    }
    return true;
}

inline int DngFrameQueue::next(DngFrame &out, std::string &err)
{
    // L-8:start() 之前或 stop() 之后调用是调用方 bug。stop() 里 m_slots.clear()
    // 之后 m_slots 为空,继续走到下面的 m_slots[m_consumed % m_depth] 就是空
    // vector 取下标 —— UB(不是普通越界,vector::operator[] 不查)。现在所有
    // 调用方都没有这条路径,但这是公共头 API,自己先把门守住。
    if (!m_started) {
        err = "dngframeq: next() 在 start() 之前调用(零工作线程时 start() 现在返回 false,见 N-05)";
        return -1;
    }
    // N-05:原「m_no_workers → 消费线程亲自解码」降级分支已删除。start() 在
    // 零线程时直接 return false,跑到这里说明至少有 1 条 worker,消费线程
    // 只取帧、不解码。
    std::unique_lock<std::mutex> lk(m_mu);
    // 用 wait_for 而不是 wait:语义完全一样(谓词循环),但万一将来再出现
    // 「没人唤醒」的回归,至少每 15s 打一条带现场数据的诊断,不会一声不吭挂住
    // —— 用户等的是几分钟的转码,静默挂死最难查(真机只能看到「卡在初始化」)。
    int stalled = 0;
    for (;;) {
        if (m_cv_cons.wait_for(lk, std::chrono::seconds(15), [this] {
                if (m_failed || m_cancelled) return true;
                if (m_consumed >= m_total) return true;
                const Slot &s = m_slots[m_consumed % m_depth];
                return s.ready && s.seq == m_consumed;
            }))
            break;
        ++stalled;
        PRFQ_AVLOG(AV_LOG_WARNING,
                   "dngframeq: 等第 %zu 帧已 %ds 无进展(线程 %d / depth %zu / 已派 %zu / 已取 %zu / 失败 %d) —— 派帧侧可能出问题\n",
                   m_consumed, stalled * 15, thread_count(), m_depth, m_assigned,
                   m_consumed, (int)m_failed);
    }

    if (m_failed) { err = m_err; return -1; }
    if (m_cancelled) { err = "已取消"; return kNextCancelled; }
    if (m_consumed >= m_total) { err = "队列已取空(正常收尾)"; return kNextDrained; }

    Slot &s = m_slots[m_consumed % m_depth];
    // **swap 而不是 move**:把 buffer 的所有权换回来,槽位里留下的是调用方上一帧
    // 用过的那块容量(72MB / 15.7MB)。这样 worker 解下一轮时直接往里 resize,不用
    // 每帧重新 mmap 一遍 —— 335 帧就省掉 335 次 72MB + 335 次 15.7MB 的
    // mmap/munmap,峰值内存也随之更稳。
    out.raw.swap(s.frame.raw);
    out.rgb.swap(s.frame.rgb);
    out.w = s.frame.w;
    out.h = s.frame.h;
    s.ready = false;
    m_consumed++;
    m_cv_prod.notify_one();   // 放一个空位出来
    return kNextOk;
}

inline void DngFrameQueue::cancel()
{
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_cancelled = true;
    }
    m_cancel_flag.store(true, std::memory_order_relaxed);
    m_cv_prod.notify_all();
    m_cv_cons.notify_all();
}

inline void DngFrameQueue::stop()
{
    if (!m_started) return;
    // 若调用方没 cancel 就 stop,先把工作线程放出来,否则 join 会卡死。
    cancel();
#if defined(PRFQ_USE_PTHREAD)
    for (pthread_t t : m_threads) pthread_join(t, nullptr);
    m_threads.clear();
#else
    for (auto &t : m_workers) {
        if (t.joinable()) t.join();
    }
    m_workers.clear();
#endif
    m_slots.clear();
    m_started = false;
    // seed() 的两段式落地:stop() 会清掉 m_seed_frame 搬进去的 slot,所以
    // m_seeded 在这里复位,好让同一个对象能再 start(本类目前只 start 一次)。
    m_seed_frame.raw.clear();
    m_seed_frame.raw.shrink_to_fit();
    m_seed_frame.rgb.clear();
    m_seed_frame.rgb.shrink_to_fit();
    m_seed_frame.w = 0;
    m_seed_frame.h = 0;
    m_seeded = false;
    // 重置回「未取消」,好让同一个对象能再次 start(本类目前只 start 一次,但保持干净)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_cancelled = false;
        m_failed = false;
    }
    m_cancel_flag.store(false, std::memory_order_relaxed);
}

} // namespace proresforge

#endif // PRORESFORGE_DNGFRAMEQ_H
