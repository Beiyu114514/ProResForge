# 进度条问题检查（2026-10-08）

设备：emulator-5554（sdk_gphone64_x86_64, 1080×2400, density 2.625, 深色主题）。检查只读，未改任何源码。

## 结论（一句话）

作业卡（Jobs 页）里的「百分比」和「n/total 帧计数」来自两个不同阶段的计数：百分比取 filtergraph **已输出**帧的 pts，帧计数取 **已推入**帧的计数，后者领先 1–2 帧。长视频上看不出来（2/375 ≈ 0.5%），短序列（DNG zip 4 帧）上每步偏 25%，于是出现「跑完 4/4 才 50%」「前半程一直 0%」。给这两个数兜底让它们同源的公式是死代码。另有三个小问题：排队态显示确定态空条（0%）、100% 时其实还在写盘、进度节流让短作业只刷新 2–3 次。

## 一、DNG 序列 zip（4 帧）实测

素材：`/sdcard/Download/dngzip-test-4f.zip`（4 帧 4096×3072，选 25 fps → 时长 160 ms）。Jobs 页用 `uiautomator dump` 约 1 s 一次抓取，四次独立运行表现一致（r1–r12、p1–p10 两轮数据如下合并）：

| 采样 | 卡片文案（第一张卡） | 第二张卡 |
|---|---|---|
| 起步 | `Running · current job` + `0%` | `Queued` + `Queued · -` + `0%` |
| 中途 | `Transcoding · 2/4` + `0%` | `Queued` + `0%` |
| 中途 | `Transcoding · 3/4` + `25%` | 同上 |
| 中途 | `Transcoding · 4/4` + `50%` | 同上 |
| 收尾 | `Transcoding · 4/4` + `100%`（仍是 Running，不是 Done） | 同上 |
| 完成 | `Done`（Duration / Size / Open / Share） | `Done` |

即：计数已经 2/4、3/4、4/4 时，进度条分别只有 0%、25%、50%——正好每步落后一格；100% 要到 decode 排空后才出现。

## 二、长视频对照（375 帧）

素材：宿主 ffmpeg 生成 `testsrc2=size=640x360:rate=25:duration=15`，推到 `/sdcard/Download/long30.mp4`。同一次作业：

| 采样 | 卡片文案 |
|---|---|
| t1 | `Transcoding · 2/375` + `0%` |
| t2 | `Transcoding · 91/375` + `24%`（91/375 = 24.3% ✓ 自洽） |
| t3 | `Transcoding · 256/375` + `68%`（256/375 = 68.3% ✓） |
| t4 | `Transcoding · 363/375` + `96%`（✓） |
| t5 | `Done` |

结论：同样的 1–2 帧落后在长素材里只占 0.5%，看不出来；问题不是「百分比算错」，而是「两个数不同源 + 短素材把固定落后放大」。

## 三、通知（运行中 `dumpsys notification --noredact`）

- 刚入队：`id=1001 channel=transcode category=progress prog=0 max=100 text='Queued'`
- 运行中（Jobs 卡显示 `3/4` + `25%` 的同一时刻）：`prog=25 max=100 text='Transcoding 25%'`
- 结束：进度通知被移除；完成通知每个作业一条（`id=1148 / id=3481`，`channel=transcode_done`、`category=status`、`actions=[Open]`）

通知与卡片同源：`app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:592-599` 把同一个 `c` 传给 `JobBus.progress` 和 `Notifications.progress`。

## 四、根因（代码定位）

1. 同一次上报里两个来源：
   - `app/src/main/cpp/transcode.cpp:1362` `p->rep((int)(pct + 0.5), "decode", p->frames);`
   - `pct` 来自 **已输出** 的 sink 帧 pts：`app/src/main/cpp/transcode.cpp:1344-1348`（`t = p->sframe->pts` → `us/duration_us`）。
   - `p->frames` 来自 **push 计数**：`app/src/main/cpp/transcode.cpp:1371-1386`，唯一自增点 `:1385 p->frames++;`（`push_frame` 内、drain 之前）。
   - filtergraph 有 1 帧前瞻（要看到下一帧 pts 才吐当前帧），加上计数先加后 drain → 实测 `counter = i + 2`（i 为已输出帧序号）。4 帧素材每步偏 25%。
2. 让两者同源的兜底是**死代码**：`app/src/main/cpp/transcode.cpp:1350` `if (pct < 0.0 && p->total > 0) pct = 100.0 * (double)p->frames / (double)p->total;` —— `Pipe::total`（`:1257` 声明 `int64_t total = -1;`）全仓库从未赋值；被赋值的只有 `p->rep.total`（`:2435 vp.rep.total = (int64_t)dngzip.frame_count();`、`:2445 vp.rep.total = nb > 0 ? nb : -1;`）⇒ `p->total > 0` 恒假，该分支永不进入。（顺带：UI 的 `totalFrames` 就是 `Reporter::total`，见 `app/src/main/cpp/transcode.cpp:669-678`。）
3. 100% ≠ 完成：decode 排空后 `app/src/main/cpp/transcode.cpp:2923 vp.rep(100, "decode", vp.frames);` 先报 100%，mux/写盘收尾在随后（`:3084 vp.rep(0, "finalize", vp.frames);`）；而 `app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:585-588` 的单调 max（`maxPct = maxOf(maxPct, pct.coerceIn(0, 100))`）把 100% 钉住、也吞掉 finalize 的 0 ⇒ 用户看到 `Transcoding · 4/4` + 100% 停 1–2 秒（有时是 `Finalizing` + 100%，stage 文案见 `app/src/main/java/com/proresforge/app/ui/JobsScreen.kt:199-210`）。
4. 排队态是确定态空条：`app/src/main/java/com/proresforge/app/engine/JobBus.kt:66/:173` 给未开始/刚开始的任务 `pct = 0` ⇒ `app/src/main/java/com/proresforge/app/ui/JobsScreen.kt:139-144` 渲染 `app/src/main/java/com/proresforge/app/ui/components.kt:154-195` 的确定态 0%（`:169-176` 的不确定分支要求 `pct !in 0..100`，真实数据下走不到）。而该分支文案只是「…」（`app/src/main/res/values-en/strings.xml:100`）。
5. 节流放大了观感：`app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:567-600`（`THROTTLE_MS = 1000`，且要求 `pct - lastPct >= 1` 且间隔 ≥ 1 s）⇒ 4 帧作业全程只有 2–3 次刷新，所以「4/4 + 50%」这类错配会被看到 1–2 秒。
6. 队列本身是串行的：`app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:38`（注释）、`:172-178`（`firstOrNull { it.value.state == JobState.QUEUED }` 循环取一个跑完再下一个）⇒ 单条共享进度通知 `id=1001` 不会互相覆盖（这是设计，不是 bug）。

## 五、修复建议（未实施，待确认）

- A（核心）：让百分比与帧计数同源。最小改动可在 `app/src/main/cpp/transcode.cpp:1362` 用 push 计数计算百分比（`frames/total`，需先给 `Pipe::total` 赋上 `p->rep.total` 的值），pts 只作兜底；或保留 pts 但取 `max(pts 值, frames/total)`。
- B：排队态给 `pct = -1`（`app/src/main/java/com/proresforge/app/engine/JobBus.kt:66/:173`）或让 `ProgressRow` 按 state 判定，并把 `progress_indeterminate` 的「…」换成可读文案（如 `Queued`/`等待中`）。
- C：非终态封顶 99%、`DONE` 才 100%（或 finalize 阶段显示不确定条），避免「已 100% 但还在写盘」。
- D：短作业放宽节流（剩余帧数很少时允许每次上报），否则 4 帧素材只有 2–3 次跳动。

## 六、复现方法

1. 设备 `/sdcard/Download/` 放 4 帧 DNG zip 与任意短视频；长视频可用 ffmpeg 造：`ffmpeg -f lavfi -i "testsrc2=size=640x360:rate=25:duration=15" -c:v libx264 -pix_fmt yuv420p long30.mp4`。
2. App：Home 选入 → Start → 切 Jobs 页。
3. 每 ~1 s `uiautomator dump /sdcard/x.xml` + `adb pull`，主机侧正则 `text="([^"]*)"[^>]*?bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"` 取卡片文案（设备侧管道会 segfault，务必回主机解析）。
4. 运行中 `dumpsys notification --noredact`（写文件后 pull），看 `id=1001` 的 `android.progress` / `android.text`。

## 七、未做 / 未测

- 未改源码（native 与 Kotlin 均未动）；本文件仅为检查记录。
- 只在 emulator-5554 上验证，未上真机；未覆盖 H.265 / ProRes / VP9 输出（共用 `drain_graph` 逻辑，理论上一致）与音频路径的百分比。
- 未测取消（Cancel）、失败（error）时的进度表现；未测 Home 页（Transcode）在作业运行中的刷新。