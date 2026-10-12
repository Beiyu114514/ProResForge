# ProResForge v1.0.0 发布记录(机器可核对)

日期:2026-10-11。决策与材料见 `docs/RELEASE-2026-10-11.md`。
本文件记录构建产物指纹与测试证据,对应上线清单 D6。

## 1. 发布产物

| 产物 | 值 |
|---|---|
| 文件 | `dist/ProResForge-1.0.0.apk`(release 签名) |
| SHA-256 | `9a81293d3d2d278123c201427803c184848f2d6357e3c99090eaf14095350967` |
| 大小 | 53,730,729 字节(51.2 MiB) |
| 版本 | versionCode=1, versionName=1.0.0(minSdk 24 / targetSdk 35 / compileSdk 35) |
| ABI | arm64-v8a + armeabi-v7a(apksigner/aapt dump badging: `native-code: 'arm64-v8a' 'armeabi-v7a'`) |
| 签名 | CN=ProResForge, O=ProResForge contributors;SHA-256 `7a40d4fc75f4bd4eb4dea645584adf694373760f0e5957cf59a11b08746bf61d`(与 C1 keystore 指纹一致;apksigner verify 通过) |
| 构建时间 | 2026-10-11 09:50 (UTC+8) |
| 构建命令 | `:app:assembleRelease`(WSL Ubuntu-24.04,NDK r27=27.0.12077973,AGP + gradlew --no-daemon) |
| native flags | `app/build/native-flags-manifest.txt`: buildType=Release,`-DCMAKE_BUILD_TYPE=Release -DPRF_PRORES_THREADS=3 -DPRF_AVFRAME_POOL=0`(prfWriteNativeFlagsManifest 自查门禁通过) |
| 源码快照 | `dist/ProResForge-source-v1.0.0.zip`(git archive tag `v1.0.0` = 提交 9492a3f,含 build-ffmpeg-android.sh、BUILDINFO、双 ABI libraw-manifest、README、PRIVACY.md;不含任何 keystore/凭据)。**2026-10-13 重打**:首发 zip 在 `core.autocrlf=true` 的 Windows 机上 archive,脚本被转 CRLF、sha256(`416e600f…`)与 BUILDINFO pin(`683d3454…`)不符;已用 `git -c core.autocrlf=false archive` 重打为 LF 版(sha256 `3e731122…f1182ae`,zip 内脚本 sha 与 pin 一致)并替换 Release 资产;仓库加 `.gitattributes`(text=auto eol=lf)防复发 |
| GPL offer | NOTICE.md §6:随发布条目提供源码快照 + 书面索取兜底;要约人 ProResForge contributors;有效期至 2029-10-10(≥3 年) |

## 2. FFmpeg/prebuilt provenance(B1/B4)

- `scripts/build-ffmpeg-android.sh` 全量重编,脚本 sha256 `683d34540d3847231c73547fde51bf8be36e112379c00c648b17abdc02e2bb4b`
- pin(全部 verify_pin 通过):FFmpeg tag n6.1.6 @ `f1e3a2bf7a2f2cde936d1ed97f09a26853d20125`;x264 stable @ `b35605ace3ddf7c1a5d67a2eb553f034aef41d55`(X264_BUILD 165);x265 tag 4.2 @ `e444744c03978c1fb4e037168967020cf2648427`(X265_BUILD 216);libvpx tag v1.17.0 @ `6df3ec34557879fff673706f4a1d9fbd0f3a6f0e`;LibRaw tag 0.21.4;LibRaw-cmake @ `eb98e4325aef2ce85d2eb031c2ff18640ca616d3`
- 版本头实测:双 ABI libavcodec 60.31.102;`libavcodec.so` 内嵌串 **`FFmpeg version n6.1.6`**(此前为 n6.1)
- 编码器在位(CONFIG_..._ENCODER=yes):libx264 / libx265 / libvpx-vp9 / prores × 双 ABI
- `verify-prebuilt.sh`:**arm64-v8a 17/17 PASS、armeabi-v7a 17/17 PASS**(LibRaw provenance:raw_r 线程安全 target、manual=false、libraw_nothreads=absent、sha 三重复算;tls_probe=absent 为信息性提示,判据四项全命中)
- 回滚备份:`~/prf/app/src/main/cpp/prebuilt.pre-s1-backup/`(重编前整套旧产物)、`tmp/prebuilt-s1.tgz`(新产物归档)

## 3. 测试证据(D1–D3,D5)

### D1 单测
`:app:testDebugUnitTest` **58/58 PASS,0 failed,0 skipped**(A 批修复后,WSL ~/prf)。

### D2 模拟器冒烟(release 包,Android 15 x86_64 模拟器 medium_phone,ARM 翻译跑 arm64)
| 项 | 结果 | 证据 |
|---|---|---|
| DNG zip 探测 | PASS | 首页 Media info:dng·zip 4096×3072 4 帧 rgb48le + Audio pcm_s24le/pcm_f24le(来自 zip 内 audio.wav) |
| DNG zip → ProRes MOV | PASS | `job begin: zip-pcm24.zip → zip-pcm24_PF_prores (2).mov (PRORES)`,`nativeRun rc=0`,DONE 7.0s |
| MOV → MP4 | PASS | test-pcm-s24le.mov → mp4,rc=0,DONE |
| **MP4 → MOV(反向)** | PASS | test-aac_PF_h264.mp4 → test-aac_PF_h264_PF_prores.mov,rc=0,DONE 1.5s |
| 取消 → 同输入重跑(K-01) | PASS | `job end: zip-f24le.zip state=CANCELLED dur=2622ms` → 同输入重跑 `state=DONE dur=6911ms rc=0` |
| faststart 导出可播 | PASS | 输出 mp4 顶层 box 序 `ftyp→moov→free→mdat`(moov 在前);ffprobe 可读 h264+aac |
| 进度条回归 | PASS(间接) | 本次 10+ 任务无一处停留 50%(全部推进到 DONE/CANCELLED 终态,终态卡片渲染正确);Round10/11 已 A/B 实测过修复本体。模拟器任务 1–7s,中途帧未截到 |
| 完成通知 | PASS | `NotificationRecord(channel=transcode_done actions=1)`,点击拉起应用;进度渠道 `transcode` 同在 |
| 通知理由框 dismiss 不开转 | PASS | 撤销 POST_NOTIFICATIONS → Start → 弹「Allow transcode notifications?」→ **按返回键关闭 → logcat 0 条入队/服务记录**(注:「Not now」按钮按设计=跳过通知并开始转码) |
| 许可证页 | PASS | 历史 → Licenses:NOTICE.md 正常渲染,#/粗体/行内记号已剥离(A4);9 个 assets/licenses/ 条目随包(B5) |
| Snackbar(N11-2 回归) | PASS | 入队提示 +4s 可见、+12s 自动消失(duration=Long);切 Jobs 返回不重弹 |

### D3 PCM 三条(release 包)
| 场景 | 结果 | 证据 |
|---|---|---|
| ProRes(PCM sowt/s24le)→ MP4 | PASS | 日志 `transcode: PCM audio cannot copy to MP4, auto fallback to AAC`;`nativeRun rc=0`;输出 ffprobe:`codec_name=aac sample_rate=48000` |
| ProRes(AAC)→ MP4 | PASS | 全程**无**降级日志,输出 `codec_name=aac`(copy 保持) |
| DNG zip → ProRes(MOV) | PASS | 输出 `prores(apch) + pcm_s24le(in24)` copy 保持 |

补充(N11-1 的 F24LE 场景):zip 内 audio.wav 为 PCM_F24LE(手工构造,wavdec 特有 24-in-32 float 路径)→ MP4:FFmpeg 6.1.6 的 mp4 muxer 对 f24le `avformat_query_codec` 不过,故在 transcode.cpp:2218 的容器不匹配闸口即静默转 AAC(输出 aac,rc=0,可播)。A2 的上界修复(<0x11000)使整块 PCM(含 F24LE/VIDC/SGA)在**任何** muxer 声称接受 PCM 时都会走显式降级+日志路径;修前 vs 修后的用户可见行为一致(均出 aac),差别是修复后覆盖完备且可观测。

### D5 Android 15 mediaProcessing 配额
- 模拟器 `device_config`:`android.content.pm.introduce_media_processing_type=true`(mediaProcessing FGS 类型已生效)。
- 6h/日配额的耗尽行为(startForeground 失败 → 回滚 + 不崩,R10-10 分支)**未实测**——需要真实消耗 6 小时配额,本会话无法构造;该分支在 Round10 经代码核对。应用侧文案已在 changelog/E4 与 F2 已知问题中说明「当日配额用尽请次日再试」。**遗留:首月内找一台长跑环境实测一次。**

## 4. 版本管理(B3)

```
e1976d8  Round11 基线(输入态)
91c6d0a  S-1/B1+B4: FFmpeg n6.1.6 完整重编,prebuilt provenance 入库
e6ec3cb  A 批:N11-1/N11-2/A4 + B2 NOTICE offer
9492a3f  C 批:签名接线/决策记录 + E 材料 + F 说明   ← tag v1.0.0 指向此提交
```

## 5. 环境备注(复现要点)

- 源码在 Z:(SMB 共享);WSL 侧构建目录 `~/prf`(drvfs 挂载 `/mnt/zshare` 不稳定,本会话用 tar+\\wsl.localhost 通道同步;重挂载命令:`wsl -u root mount -t drvfs Z:\ /mnt/zshare`)。
- NDK sysroot 对 armeabi-v7a 的 zlib 目录名是旧式 `arm-linux-androideabi`,脚本找 `armv7a-...`——本机已加软链接 `sysroot/usr/lib/armv7a-linux-androideabi → arm-linux-androideabi`(换 NDK 需重做)。
- Windows git 全局 `core.autocrlf=true` 会把 clone 出的 shell 脚本变 CRLF(跨平台重编必踩),clone 一律加 `-c core.autocrlf=false`。
- 测试素材构造:ProRes 测试 MOV 用 ffmpeg lavfi 生成;PCM_F24LE 无编码器,用 Python 按 wavdec 规则(fmt tag=1 + bits=24 + block_align=ch*4)构造 WAV 打进 DNG zip(audio.wav 入口名,dngzip 约定)。
