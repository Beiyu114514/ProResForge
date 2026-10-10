# ProResForge 正式上线清单

日期：2026-10-11。输入：Round9–Round11 复审结论 + 当前仓库事实核查。
当前包：`versionCode=1` / `versionName=1.0.0`，minSdk 24 / targetSdk 35（compileSdk 35），双 ABI（arm64-v8a + armeabi-v7a），无 INTERNET 权限（纯离线）。
三条硬阻塞：**S-1 FFmpeg 完整重编（GPL 合规）→ 签名/混淆决策 → PCM 设备回归**。A→B→C→D→E 按序做，批内可并行。

## 当前状态一页表

| 域 | 状态 |
|---|---|
| 功能缺陷 | Round10 全部 16 项已修；PCM→MP4 降级代码正确但设备回归未做；N11-1（PCM_F24LE 漏网）/N11-2（Snackbar 永不消失）待修 |
| 单测 | 58/58 绿（当前树） |
| GPL 合规 | FFmpeg 二进制发行串 `n6.1` ≠ pin `n6.1.6`，NOTICE offer 是占位措辞 —— **未闭合** |
| 打包 | licenses 已进 APK、MIT.txt 已修；release 无签名、minify 关、无 keystore |
| 版本管理 | **git 0 提交**，无 tag，无可归因的源码快照 |
| 渠道材料 | 无隐私政策、无发行说明 |

---

## A. 代码收尾（上线前必须清零的功能项）

- [ ] **A1. PCM 设备回归（先于 A2 跑一遍基线）**——按 `verify-pcm-fix.md` 清单在模拟器/真机过三条：
  1. ProRes(PCM) MOV → MP4 H.264/H.265：日志出现 `PCM audio cannot copy to MP4, auto fallback to AAC`，`nativeRun rc=0`，输出音轨 `ffprobe` 为 aac 48kHz；
  2. ProRes(AAC) → MP4：**无**降级日志（copy 保持）；
  3. DNG zip → ProRes(MOV)：音轨仍是 PCM copy。
  勾掉 `verify-pcm-fix.md` 的复选框并回填证据（logcat 片段 + ffprobe 输出）。
- [ ] **A2. 修 N11-1**：`transcode.cpp:2224` 的 PCM 上界改为 `aid < AV_CODEC_ID_ADPCM_IMA_QT`（0x11000，覆盖整个 PCM 块，含 F24LE/VIDC/SGA）。重编 native 后重跑 A1 第 1 条。
- [ ] **A3. 修 N11-2**：入队 Snackbar（`HomeScreen.kt:142-152`）——`showSnackbar` 显式 `duration = SnackbarDuration.Long`（或 Indefinite + dismiss 动作），并把 `vm.consumeMessage()` 提前到送出之后；验证「切 Jobs 再回来不重弹、4–10 秒自动消失」。
- [ ] **A4.（建议，小）**：`fd_io_write` 对 `pwrite64==0` 加 break（`transcode.cpp:346-356`）；删两处重复注释行（`HomeScreen.kt:139-140`、`Notifications.kt:35-36`）；LicensesScreen 行内 Markdown 记号剥离（U-17 遗留）。
- [ ] **A5. 收口验证**：单测全绿 + `:app:assembleDebug` 通过 + 模拟器冒烟（DNG zip 探测/预览/转码、取消→重跑、faststart 导出、双通知渠道）。

## B. 发布闸门（合规，S-1 是硬阻塞）

- [ ] **B1. FFmpeg 完整重编**：跑 `scripts/build-ffmpeg-android.sh`（**不手改 ffversion.h、不做二进制字符串替换**）。验收：两个 ABI 的 `libavcodec.so` 里出现 `FFmpeg version n6.1.6`；`verify-prebuilt` 两 ABI 各 17/17；`verify_pin` 把二进制与 pin 的 commit 对上。重编换掉整套 prebuilt，失败/验证不过要能单独回滚（不要和 A 批功能改动绑死在同一次提交）。
- [ ] **B2. NOTICE.md 的 GPL-3.0 §6 书面 offer 落地**：现状是占位措辞（「见随本应用发布渠道提供的源码包/仓库链接」）。必须写明：①对应源码的**具体 URL**（或书面索取的邮政/邮箱地址）；②要约人（主体名）；③**至少三年**的有效期承诺。在 B1 的 `verify_pin` 通过之前，不得把任何 commit 写成随包源码。
- [ ] **B3. 源码归档与 tag**：git 当前 0 提交——先把整树提交，再打发布 tag（如 `v1.0.0`）。规则：**tag 指向的树 = 产出这个 APK 的确切源码**；B2 的 offer URL 指向该 tag 的归档（zip/tar.gz，须含 `scripts/build-ffmpeg-android.sh`、pin 记录与 `prebuilt/*/libraw-manifest-*`）。
- [ ] **B4. 产物对账同步**：重编后 `prebuilt/BUILDINFO`、两个 `libraw-manifest-<abi>`、构建脚本 SHA-256 全部回填为新值；跑一遍 `verify-prebuilt` 门禁。
- [ ] **B5. S-2 终验**：新打的 release 包内确认 9 个 `assets/licenses/` 条目存在；应用内 Licenses 页逐条可见（2. 随包 native 组件列表与 NOTICE 一致）。

## C. 构建与签名（出可分发包的前提）

- [ ] **C1. 生成 keystore**（库外保管：**不进仓库**、密码与 keystore 另做备份；丢了就无法对已有用户升级）。推荐：`keytool -genkeypair -v -keystore proresforge-release.keystore -alias proresforge -keyalg RSA -keysize 2048 -validity 10000`。
- [ ] **C2. release 签名配置**：`app/build.gradle.kts` release 块加 `signingConfig`，凭据从 `keystore.properties`（gitignore）或环境变量读，**不得硬编码**进构建脚本。
- [ ] **C3. minify 决策（二选一，记录理由）**：
  - 维持 `isMinifyEnabled = false`：接受不混淆（本地工具类应用可接受），但 APK 更大、反编译更容易；
  - 开 R8：必须先补 JNI keep 规则（`FFmpeg` 的 native 方法、被 JNI 反射触碰的类、`JobSpec`/`JobRecord` 序列化字段——现有 `proguard-rules.pro` 只有 9 行，肯定不够），然后跑 D2 全套冒烟。**没有冒烟就不开 R8**（Round10 既定结论）。
- [ ] **C4. 版本号定稿**：`versionCode`/`versionName`（1.0.0 / 1 起步即可）；以后每次分发递增 versionCode。
- [ ] **C5. 出 release 包**：`:app:assembleRelease`，确认产物含双 ABI、CMake Release 参数自查门禁通过（`prfWriteNativeFlagsManifest`）；记录 **release APK 的 SHA-256** 进 docs。
- [ ] **C6. 渠道形态决策**：Google Play 新应用要求 **AAB**（`:app:bundleRelease`，Play 负责签名可用 Play App Signing）或官网/GitHub 直发 APK（用 C1 自签）。二选一后，D/E 的对应条目跟随渠道要求。

## D. 测试矩阵（在 **release 包** 上做，不是 debug 包）

- [ ] **D1. 单测**：`testDebugUnitTest` 全绿（当前基线 58/58）。
- [ ] **D2. 模拟器冒烟（release 包）**：DNG zip 探测/预览/连续转码；MOV↔MP4 双向；**取消→同输入重跑 DONE**（K-01 回归）；faststart 导出可播；进度条无 50% 平台、终态不满格（进度修复回归）；完成通知 Open/Share；通知理由框 dismiss 不开转；许可证页。
- [ ] **D3. PCM 三条回归**（A1 清单）在 release 包重跑一遍。
- [ ] **D4. 真机**：至少一台 arm64 真机过 D2 核心 10 条（进度条修复、Snackbar、通知此前只有模拟器证据）；**若保留 armeabi-v7a**，找 32 位设备/系统镜像冒烟一次——Round10 只对 32 位路径做过源码核对，二进制从未在 32 位环境验证；如果找不到 32 位测试环境，考虑首发只带 arm64-v8a（改 `abiFilters`）。
- [ ] **D5. Android 15 前台服务配额**：`mediaProcessing` 6h/日配额耗尽时的行为（`startForeground` 失败 → 回滚 + 不崩，R10-10 分支）实测一次；应用内/文档说明「当日配额用尽请次日再试」。
- [ ] **D6. 版本固化**：release 包 SHA-256、版本号、构建时间写入 docs（沿用 Round9/Round11 的记录格式）。

## E. 渠道材料

- [ ] **E1. 隐私政策**：应用无 INTERNET 权限、全部本地处理、文件访问走 SAF 用户自选——声明「不收集、不传输、不上传任何数据」。Play 的 Data Safety 表全部选 No（无收集）；直发渠道把政策页挂在下载页。
- [ ] **E2. 商店/下载页材料**：图标、截图、功能描述；体积 ~60MB 的说明（内置 FFmpeg/x264/x265/LibRaw 全套编解码器）。
- [ ] **E3. 分级与问卷**：内容分级、目标年龄（工具类，无广告无内购无用户生成内容）；Play 还需隐私政策 URL + Data Safety。
- [ ] **E4. 发行说明（changelog）**：必须包含用户可感知的行为变化——**含 PCM 音频的 ProRes 转 MP4 时音轨自动转 AAC**（原为失败）；进度条修复；许可证页。GPL 归因指引指向应用内 Licenses。
- [ ] **E5. GPL 对应源码包**：B2 offer 指向的链接真实可下载、内容与 tag 一致（自检：下载一遍、解开能按 README/构建脚本复现）。

## F. 上线后

- [ ] **F1. 反馈与崩溃渠道**：当前无任何崩溃上报（无网络权限是特性，不该为上报开例外）——明确人工渠道（issue 链接/邮箱），崩溃线索引导用户取 `files/logs/app.log`。
- [ ] **F2. 已知问题公告**：6h/日配额限制；PCM→AAC 自动降级行为；个别播放器对 mp4 内 `ipcm`（未降级路径之外的罕见 PCM）兼容性；32 位设备未经广泛测试（若首发含 v7a）。
- [ ] **F3. 源码 offer 有效期巡检**：日历提醒每年核对一次 offer 链接可达（GPL 承诺三年）。

---

## 依赖关系与最小路径

```
A1 → A2 → A5 ─────────────┐
B1 → B2/B4 ─┐             ├─→ C5(release 包) → D(全矩阵) → E → 上线
B3(首次提交) ┘   C1 → C2/C3 ┘
```

最小可上线路径：**A1–A3、B1–B5、C1–C6、D1–D6、E1–E5** 全部勾完；A4/F 为建议项。
最大的不确定性在 **B1 重编**（一次失败会换掉整套 prebuilt，需独立验证与回滚余地）和 **D4 的 32 位验证环境**（找不到就首发只带 arm64）。
