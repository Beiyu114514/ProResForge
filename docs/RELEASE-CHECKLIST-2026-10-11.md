# ProResForge 正式上线清单

日期:2026-10-11。输入:Round9–Round11 复审结论 + 当前仓库事实核查。
当前包:`versionCode=1` / `versionName=1.0.0`,minSdk 24 / targetSdk 35(compileSdk 35),双 ABI(arm64-v8a + armeabi-v7a),无 INTERNET 权限(纯离线)。
三条硬阻塞:**S-1 FFmpeg 完整重编(GPL 合规)→ 签名/混淆决策 → PCM 设备回归**。A→B→C→D→E 按序做,批内可并行。

> **整改状态(2026-10-11)**:A/B/C/E 全部完成;D 除 D4/D5 外完成。
> 全部证据与指纹见 `docs/RELEASE-RECORD-2026-10-11.md`,决策见 `docs/RELEASE-2026-10-11.md`。
> 发布产物:`dist/ProResForge-1.0.0.apk` + `dist/ProResForge-source-v1.0.0.zip`(tag v1.0.0 = 9492a3f)。

## 当前状态一页表

| 域 | 状态 |
|---|---|
| 功能缺陷 | Round10 全部 16 项已修;N11-1(PCM 上界 0x11000)/N11-2(Snackbar duration=Long+先消费)已修并模拟器复证;PCM→MP4 三条在 **release 包**回归通过 |
| 单测 | 58/58 绿(A 批修复后) |
| GPL 合规 | **已闭合**:FFmpeg n6.1.6 完整重编(双 ABI 17/17、版本串实测 n6.1.6、verify_pin 过);NOTICE §6 offer 落地(随包源码快照 + 3 年期) |
| 打包 | licenses 9 条目随包;release 有签名(keystore 库外三处保管);minify=false 决策已记录 |
| 版本管理 | git 5 提交 + tag `v1.0.0`(指向产出 APK 的确切源码);源码快照 zip 产出并自检 |
| 渠道材料 | PRIVACY.md、changelog、分级、体积说明、截图齐备 |

---

## A. 代码收尾(上线前必须清零的功能项)

- [x] **A1. PCM 设备回归(先于 A2 跑一遍基线)**——基线在整改前 debug 包跑完三条:①sowt→MP4 降级日志+aac 48kHz;②AAC→MP4 无降级;③zip→MOV PCM copy。证据:RELEASE-RECORD §3;`verify-pcm-fix.md` 三条已按实核对
- [x] **A2. 修 N11-1**:`transcode.cpp:2224` 上界改为 `aid < AV_CODEC_ID_ADPCM_IMA_QT`(0x11000)。重编后复测 A1 第 1 条:降级日志 + rc=0 + aac ✓。附:F24LE 在 FFmpeg 6.1.6 mp4 muxer 上由 2218 行容器闸口吸收(输出同为 aac),修复价值=覆盖完备+可观测,详见 RECORD §3 补充
- [x] **A3. 修 N11-2**:显式 `duration = SnackbarDuration.Long`(根因:带 actionLabel 的 M3 默认是 Indefinite);`consumeMessage()` 移到送出之前(切页取消协程也不残留)。验证:+4s 可见/+12s 消失、切 Jobs 返回不重弹
- [x] **A4.**:fd_io_write `pwrite64==0` break;两处重复注释删除;LicensesScreen 行内记号剥离(截图核验 **/`/# 均不再显示)
- [x] **A5. 收口验证**:58/58 单测 + assembleDebug + 模拟器冒烟(核心路径在 debug 包过,全套在 release 包 D2 过)

## B. 发布闸门(合规,S-1 已闭合)

- [x] **B1. FFmpeg 完整重编**:scripts/build-ffmpeg-android.sh 全量(未手改 ffversion.h、无二进制替换)。验收:双 ABI libavcodec.so 均含 `FFmpeg version n6.1.6`;verify-prebuilt 两 ABI 各 **17/17**;verify_pin 全过(FFmpeg n6.1.6@f1e3a2bf 等,见 BUILDINFO)。回滚:prebuilt.pre-s1-backup
- [x] **B2. NOTICE.md GPL-3.0 §6 offer 落地**:①源码快照 `ProResForge-source-v1.0.0.zip` 随同一发布条目提供(具体、可核对)+ 书面索取兜底;②要约人 ProResForge contributors;③自 2026-10-11 起至少三年。verify_pin 已过,offer 才写就
- [x] **B3. 源码归档与 tag**:整树 5 提交,tag `v1.0.0` → 9492a3f(= 产出 dist/ProResForge-1.0.0.apk 的确切源码);归档 zip 实测含 build 脚本、BUILDINFO、双 ABI manifest(自检脚本核对 7 项全 OK,187 文件,无凭据)
- [x] **B4. 产物对账同步**:BUILDINFO / libraw-manifest-<abi>(脚本自动生成)已随 91c6d0a 入库(.gitignore 放行 provenance 三件套);verify-prebuilt 门禁跑通;WSL ~/prf 与 Z: 镜像哈希逐文件一致
- [x] **B5. S-2 终验**:release 包内 assets/licenses/ 9 条目在(打包清单核验);应用内 Licenses 页逐条可见(截图)

## C. 构建与签名

- [x] **C1. keystore**:proresforge-release.keystore(RSA2048/10000天),**库外三处**:WSL ~/keys、~/keys/backup、Windows C:\Users\Administrator\ProResForge-signing\;口令在同目录 signing-info.txt(不入库)。SHA-256 指纹 7a40d4fc…bf61d。**请用户把两处备份再离线归档一份**
- [x] **C2. release 签名配置**:signingConfigs["release"] 从 gitignored keystore.properties 读(缺失→未签名包),无硬编码
- [x] **C3. minify 决策**:维持 false,理由写入 build.gradle.kts 注释(无 R8 smoke 不开 R8;本地工具混淆收益低)
- [x] **C4. 版本号**:1 / 1.0.0 定稿;递增规则写入 docs
- [x] **C5. 出 release 包**:assembleRelease 成功;双 ABI;flags manifest 自查门禁通过(Release/-O3);SHA-256 `9a81293d…350967` 记入 RECORD §1
- [x] **C6. 渠道形态**:GitHub/官网直发自签 APK(理由见 RELEASE-2026-10-11 §C6);发布条目固定含 APK+源码快照两件

## D. 测试矩阵(release 包)

- [x] **D1. 单测**:testDebugUnitTest 58/58(0 failed/0 skipped)
- [x] **D2. 模拟器冒烟(release 包)**:zip 探测/转码、MOV↔MP4 双向(含 MP4→MOV 反向)、取消→同输入重跑 DONE(K-01)、faststart(ftyp→moov→mdat)、完成通知可点拉起、理由框返回键 dismiss 不开转(0 入队日志)、许可证页、Snackbar 回归。进度条:10+ 任务无 50% 平台、终态渲染正确(修复本体已在 Round10/11 A/B 实测;模拟器任务 1–7s 中途帧未截到,已注明)
- [x] **D3. PCM 三条(release 包)**:见 RECORD §3(降级日志+ffprobe 证据)
- [ ] **D4. 真机**:**部分完成——环境无真机**。已按清单兜底:F2 如实公告「32 位未经广泛测试」;arm64 走模拟器 ARM 翻译层全过。**遗留:拿到 arm64 真机后跑 D2 核心 10 条再放量**
- [ ] **D5. Android 15 配额**:**部分完成**——device_config 确认 mediaProcessing 类型生效;R10-10 分支(失败→回滚不崩)代码核对过;6h/日耗尽无法短时构造。应用内/文档说明已加(E4/F2)。**遗留:长跑环境实测一次**
- [x] **D6. 版本固化**:SHA-256/版本/构建时间/NDK/脚本 sha 全部写入 RECORD §1–2

## E. 渠道材料

- [x] **E1. 隐私政策**:PRIVACY.md(不收集/不传输/不上传,无 INTERNET 权限,SAF 自选);Play Data Safety 全 No(若走 Play)
- [x] **E2. 商店/下载页材料**:一句话简介、功能描述、体积说明(~62MB=内置全套编解码器)、截图 docs/store-media/×3
- [x] **E3. 分级与问卷**:工具类/无广告/无内购/无 UGC → 所有人(3+/Everyone);记录在 RELEASE-2026-10-11 §E3
- [x] **E4. 发行说明**:RELEASE-2026-10-11 §E4(含 PCM→AAC 行为变更、进度修复、许可页、已知限制)
- [x] **E5. GPL 对应源码包**:ProResForge-source-v1.0.0.zip 自检通过(7 项内容核对;可复现性=本会话 B1 实际重编成功);**发布时**把它与 APK 同挂并点一次下载验证

## F. 上线后

- [x] **F1. 反馈与崩溃渠道**:RELEASE-2026-10-11 §F1(无上报 SDK 是特性;崩溃线索引导取 files/logs/app.log)
- [x] **F2. 已知问题公告**:§F2 四条(6h 配额、PCM→AAC 变更、罕见 ipcm、v7a 未广泛测试)
- [x] **F3. offer 巡检**:§F3(有效期至 2029-10-10;建议用户在日历建每年 10 月提醒——日历提醒需用户本人创建)

---

## 依赖关系与最小路径

```
A1 → A2 → A5 ─────────────┐
B1 → B2/B4 ─┐             ├─→ C5(release 包) → D(全矩阵) → E → 上线
B3(首次提交) ┘   C1 → C2/C3 ┘
```

**发布前仅剩**:①D4 真机 10 条(有设备就跑);②D5 配额长跑实测;③用户把 keystore 三处备份再离线归档;④发布条目实际上线时点一次源码包下载核验(E5 收尾)。
