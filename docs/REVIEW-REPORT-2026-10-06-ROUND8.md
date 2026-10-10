# ProResForge 第八轮只读复审报告

审查日期:2026-10-06 夜间(Asia/Shanghai)
审查方式:只读静态复核 + 只读校验脚本复跑(check-contracts / verify-prebuilt / check-native-flags)。
本轮**未修改任何源码、未执行 Gradle 构建**;文中引用的构建/测试证据均为工作区现有产物与日志(时间戳 22:46–22:48,发生在本轮之前)。
审查基线:`\\192.168.100.232\win\ProResForge` 当前快照(工作区仍**无 git 仓库**,所有「变更归属」不可审计,见 §5)。

## 结论

1. 上一轮(REVIEW-REPORT-2026-10-07.md)提出的 P1-01 / P1-02 / P1-03 / P2-01 / P2-03 已在当前源码中逐行复核确认闭环(§2.1,附行号);P2-02 对「运行中任务」闭环,对「排队期取消」仍有残留(§3, L-新D)。
2. 门禁体系当前全绿:契约脚本 rc=0、provenance manifest 两 ABI rc=0、Release flags 4/4 条 `-O3 -DNDEBUG`、发布默认 `PRF_AVFRAME_POOL=0` 已落(§2.3)。
3. **仍不建议作为稳定版发布**(与上一轮结论一致),阻断项集中在:无 arm64 真机 Release 端到端验收(F 段未做)、无 release 产物/签名/版本递增、无 CI 与仪器化测试、预览 mem sink 无上限(§4)。

## 2. 本轮复核确认

### 2.1 上一轮问题(源码证据)

| 编号 | 状态 | 证据 |
|---|---|---|
| P1-01 普通容器解码错误静默 | ✅ 已闭环 | `app/src/main/cpp/transcode.cpp:2178-2183`(av_read_frame 非 AVERROR_EOF → loop_error);`:2186-2210`(EOF flush send(NULL) 的 EAGAIN 循环,真实错误 → loop_error);`:2239-2253`(视频 send_packet 失败 → loop_error);`:2287-2302`(音频 send_packet 同) |
| P1-02 编码器 flush send 错误被忽略 | ✅ 已闭环 | `app/src/main/cpp/transcode.cpp:2335-2362`(send_frame(NULL) EAGAIN → 先排空再重送;receive 负值 → `result=ret; goto done`);`:2363-2382`、`:2384-2425`(音频同构);注释明确「旧代码 break 后 result 仍 0」 |
| P1-03 BSF receive 任意负值仅 break | ✅ 已闭环 | `app/src/main/cpp/transcode.cpp:1275-1290` `drain_audio_bsf` 把 EAGAIN/EOF 语义化,其余负值 `av_packet_unref + return ret`;三处调用方(`:2217-2228`、`:2259-2275`、`:2277`)均把非 EAGAIN/EOF 错误送 loop_error |
| P2-01 DNG ZIP64/条目边界 | ✅ 已闭环 | `app/src/main/cpp/dngzip.cpp:122-125` checked_add_u64;`:296-349`(EOCD/ZIP64 locator/EOCD 与 cd_off/cd_size/cd_end 范围校验;L-2 偏移越界报错在 `:315-325`);`:364-372`(循环以 cd_end 为界);`:399-407`(entry_size/next_pos checked 且 ≤ cd_end);`:464-486`(ZIP64 extra 读失败即失败、sentinel 字段缺失即失败);`:570-616`(本地头范围/签名、data_off 溢出、csize ≤ avail、usize ≤ 512MiB);`:622-655`(stored csize==usize);`:656-700`(avail_out=usize、total_out==usize、consumed_csize==csize、未完整结束即失败) |
| P2-02 Service onDestroy 历史异步 best-effort | ◐ 运行中任务已闭环;排队期残留见 L-新D | `app/src/main/java/com/proresforge/app/engine/TranscodeService.kt:386-418`:独立 finalizeScope(不随 serviceScope.cancel)+ JobBus.finish 终态守卫 + runCatching recordFinish + `finalized.await(2000ms)` |
| P2-03 clearFinished 未入锁 | ✅ 已闭环 | `app/src/main/java/com/proresforge/app/engine/JobBus.kt:116-123`:cancel/clearFinished/begin/finish 共用 `synchronized(lock)` |
| P2-04 preview mem_write 无上限 | ❌ 未修 | `app/src/main/cpp/preview.cpp:96-106`:`c->sink->insert(c->sink->end(), buf, buf+buf_size); return buf_size;` 无容量上限/分配异常防护;mem_seek 只回 AVSEEK_SIZE |
| P2-05 无 native/仪器化/CI 测试 | ❌ 未动 | `app/src/test` 仅 3 文件 22 例;无 `app/src/androidTest`;仓库无任何 CI 配置;native 验证仍靠 `tmp/` 手跑 harness |

### 2.2 其余旧欠项状态

| 编号 | 状态 | 证据/说明 |
|---|---|---|
| L-16 取消文案 | ✅ 已修 | `app/src/main/java/com/proresforge/app/ui/JobsScreen.kt:183` `"cancelled" -> stringResource(R.string.job_state_cancelled)` |
| L-17 「HDR」硬编码 | ❌ 未修 | `app/src/main/java/com/proresforge/app/ui/HomeScreen.kt:263` `FactRow(label = "HDR", …)` —— 全屏唯一不走 stringResource 的标签 |
| L-19/L-20 buildSpec 降级 | ✅ 已修 | `TranscodeService.kt` runJob:buildSpec 异常 → 任务失败(不再静默降档);`FFmpeg.kt:143-244` buildSpec 与契约一致 |
| L-21 clearCache 死代码 | ✅ 已清 | 全仓无调用点 |
| L-23 model.kt KDoc | ✅ 已对齐 | 无失效成员引用 |
| L-6 8MB 栈不降级 | ✅(但见 L-新C) | `app/src/main/cpp/dngframeq.h:240-268`:pthread_attr_setstacksize/setdetachstate 失败 → m_err + return false |
| L-新B startTranscode 中途失败不回收空输出 | ❌ 未修 | `app/src/main/java/com/proresforge/app/ui/HomeViewModel.kt:424-441`:循环内 `outputUriFor` 已创建文档后,若后续文件失败/异常 → 已建空文档无人清理 |
| L-新C dngframeq 线程创建全失败仍降级 | ❌ 未修 | `app/src/main/cpp/dngframeq.h:270-278` `pthread_create` 失败仅 `break; // 安静降级`;`:293-301` `thread_count()==0` → `m_no_workers=true`(消费线程亲自解码)—— 与 L-6「拒绝用默认 1MB 栈跑 LibRaw」自述矛盾 |
| L-新D 排队期取消历史丢失 | ❌ 未修 | `TranscodeService.kt:198-204`:排队期取消任务的 `jobStore.recordFinish(rec)` 仍在已取消的 serviceScope 内;`JobStore.kt:56` recordFinish 首挂起点 `withContext(Dispatchers.IO)` 可被取消 → 该任务历史丢失 |
| M-8 依赖未 pin | ✅ 已解决 | 见 §2.3 |
| M-10 契约漂移 | ✅ 已闭环 | `scripts/check-contracts.sh` rc=0 |
| M-5b allowBackup | ✅ 已修 | `AndroidManifest.xml:14-15` allowBackup=false + dataExtractionRules |
| M-新B prebuilt 未 pin | ✅ 已被 manifest 门禁取代 | 见 §2.3 |

### 2.3 门禁与 pin(本轮只读复跑)

- `bash scripts/check-contracts.sh` → **rc=0**(JNI 6 / CMake 源 4 / 运行时库 7 / spec 27 键 / profile=2 仅历史行)。
- `bash scripts/verify-prebuilt.sh <abi-dir> --ndk-bin …`(arm64-v8a、armeabi-v7a,NDK `/home/ubuntu/android-ndk-r27`=27.0.12077973)→ **两 ABI rc=0**;manifest 的 `script_sha256=8339285f…` 与当前 `scripts/build-ffmpeg-android.sh` 实测 SHA-256 逐字一致。
- `bash scripts/check-native-flags.sh`(两 ABI 的 Release `app/.cxx/Release/342z9661/<abi>/compile_commands.json`,mtime 21:32)→ **PASS**(各 4/4 条命令含 `-O3 -DNDEBUG`,无禁用项)。
- `app/build/native-flags-manifest.txt`(13:32:06Z 写入):`cmake_arguments=-DCMAKE_BUILD_TYPE=Release -DPRF_PRORES_THREADS=3 -DPRF_AVFRAME_POOL=0`,`release_cxx_flags=-O3 -DNDEBUG` → 发布默认 pool=0 已落。
- 源码 pin(`scripts/build-ffmpeg-android.sh:62-69`):FFmpeg `n6.1.1`、x264 SHA `b35605ac…`、x265 `4.1`+SHA `1d117bed…`、libvpx `v1.15.0`+SHA `9f9b7e9b…`、LibRaw `0.21.4`+libraw-cmake SHA `eb98e432…`;`verify_pin()`(`:192-204`)对 clone 后 HEAD 强制复核。Kotlin 侧 `gradle/libs.versions.toml` 全部精确版本。→ **M-8 已闭环**。
- provenance 现状:两 ABI `libraw-manifest-<abi>` 已存在(生成于 2026-10-06T14:30:07Z,`manual=false`),`libraw.a` 实测 SHA-256 与 manifest 一致。注意:文档 §9.1 中「当前副本无有效 manifest / 两 ABI 未从源码重建」的描述已被该 22:30 的落地取代,**文本过期**。

## 3. 本轮新增发现

- **R8-01(中)可复现供给链未固化**:`app/src/main/cpp/prebuilt/` 被 `.gitignore` 忽略(设计如此),`prebuilt/README.md` 明确说明缺失时构建会停在 CMake FATAL;manifest 本身是对「当前副本」的 provenance 证据,但**发布流程中没有一处规定**由谁、在何时、以何参数运行 `scripts/build-ffmpeg-android.sh`,也没有归档构建日志与两 ABI `libraw.a` 的 sha256。建议:在发布清单中加入「重建 + 归档 manifest/BUILDINFO/sha256/日志」一步,或提供受控的 prebuilt 分发归档。
- **R8-02(中)文档对源码快照已漂移**:`docs/PERF-OPTIMIZATION-IMPLEMENTATION-2026-10-06.md` §7 记 `app/build.gradle.kts` 440 行/md5 `1363d2e7…`,实测 **485 行/md5 `593913ebf434834885b387211626784f`**(mtime 21:29,新增段A/段D 门禁与 helpers);`app/src/main/java/com/proresforge/app/engine/FFmpeg.kt` mtime 22:46、md5 `75647ad9…`,未出现在 §7 清单。无 git、无副本 → **21:29/22:46 两次变更无法溯因**(22:46 改动经 dex 对比确认不是引入 `rwt`,更像注释/行号位移)。建议:入 git,或把 md5 清单改为脚本生成。
- **R8-03(低)门禁口径三处不一**:字段数文档 §9.1 说 16、`verify-prebuilt.sh` 16、`app/build.gradle.kts` `PRF_LIBRAW_MANIFEST_REQUIRED_FIELDS` 15(无 `threads_lib`)、`CMakeLists.txt` prf-libraw-gate 11;且 Gradle 的 GradleException 文案仍写「当前 prebuilt 是 2026-10-06 手工替换产物…门禁拒绝是预期行为」——现状已不是手工替换产物,**任何校验失败都会附上这句误导**。建议统一字段集合并更新文案。
- **R8-04(低)仓库卫生**:`.gitignore` 未覆盖 `.kotlin/`(根目录已存在);根目录仍有 16:51 python heredoc 引号事故留下的两个坏名文件(0 B / 4 B,文件名含引号与换行),`.gitignore` 不挡、且是最新快照的一部分。
- **R8-05(低)异常静默点**:`app/src/main/java/com/proresforge/app/data/OutputDir.kt:89-102` `takePersistableUriPermission` 的 SecurityException 静默 return(无日志);`AppLog.kt` 轮转与 native 侧 `O_APPEND` 句柄的交互仅在注释里声明「基本不会交错」。
- **R8-06(信息)dist 现状**:`dist/` 仅 debug 包(20261006-2248 及其同名副本,64,009,808 B);无 release APK;release 无 signingConfig、`isMinifyEnabled=false`(`app/build.gradle.kts:419` 起)、`versionCode=1`(`:386`)。

## 4. 发布阻断清单(建议按序)

1. **F 段 arm64 真机 Release 验收未执行**(§9.6 的 11 条协议):Release APK、≥60 帧 4K zip、interleaved ≥4 轮、1080p 900 帧 <20 min、4K 900 帧 <90 min、峰值 ≤400/≤500 MB、PSNR≥40 dB、LibRaw 竞态探针、pool 去留判据、1/2/3/4 prores 档位、失败注入。
2. 修 P2-04(preview mem sink 上限)+ L-新C(线程创建失败语义)+ L-新D(排队期取消落库)+ L-新B(空输出回收)。
3. P2-05:把 `tmp/` 手跑 harness 固化进仓库脚本/CI(至少 host 侧),补 instrumented 冒烟。
4. 发布工程:签名配置、`versionCode` 递增、R8 评估、release 产物入 `dist/`。
5. 卫生与文档:R8-02/03/04、L-17。

## 5. 审查边界

- 本轮**未执行 Gradle/CMake 构建与单元测试**;§2.3 复跑的三个脚本均为只读检查(不写构建产物)。
- 工作区无 git 仓库,无法做 diff/历史归因;文中 md5/行号均基于当前快照,后续任何改动都会使本报告的定位漂移。
- 上一轮报告的行号(如 `preview.cpp:98-104`)与本轮(`preview.cpp:96-106`)存在少量偏移,引用以本报告行号为准。
- 数据层(Kotlin)本轮通读 OutputDir / LutFile / JobStore / MediaProbe / AppLog / SettingsStore / JobBus / TranscodeService / HomeViewModel:未发现新的高危缺陷;R8-05 为仅有的两个小口子。
