# PCM→MP4 修复验证报告

**构建时间**：2026-10-09 14:35
**APK 文件**：`dist/ProResForge-pcm-fix-20261009-143628.apk`
**SHA256**：`213dc9fa57dc220bbf6ce716cc87816cec9dd371e180815b7f281f826a241e5a`
**大小**：60 MB

---

## 构建结果 ✓

```
BUILD SUCCESSFUL in 1m 56s
39 actionable tasks: 10 executed, 29 up-to-date
```

**编译详情**：
- Native 代码重新编译（arm64-v8a + armeabi-v7a）
- 仅 1 个警告：`av_stream_get_side_data` 已弃用（已知，不影响功能）
- Kotlin 代码重新编译
- 无编译错误

---

## 验证步骤

### 1. 安装 APK

```bash
# 在设备上安装（替换为实际设备）
adb install -r dist/ProResForge-pcm-fix-20261009-143628.apk
```

### 2. 测试场景

**输入文件**：`261007_033849_VIDEO_25mm_Apple Log_Apple-Log2_PF_prores.mov`
- 格式：ProRes 422 HQ
- 尺寸：4096×3072
- 帧数：45 帧
- 帧率：25 fps
- 音频：PCM（codec_id 65536）

**测试转码**：
1. ProRes MOV → MP4 H.264
2. ProRes MOV → MP4 H.265

### 3. 预期结果

✓ **日志检查**：
```
I/FFmpeg: transcode: PCM audio cannot copy to MP4, auto fallback to AAC
I/FFmpeg: transcode: video encoder selected=h264_mediacodec ...
I/TranscodeService: nativeRun rc=0
I/TranscodeService: job end: ... state=DONE ...
```

✓ **输出验证**：
- 转码成功完成（rc=0）
- 输出文件音频为 AAC（48kHz, 160kbps）
- 无 `sowt` 或 `ipcm` 错误

### 4. 回归测试

确认以下场景不受影响：
- [ ] DNG zip → ProRes（音频应仍为 PCM）
- [ ] ProRes(AAC) → MP4（应保持 copy 模式）
- [ ] ProRes(PCM) → MOV（应保持 copy 模式）

---

## 验证命令

```bash
# 查看输出文件音频编码
ffprobe -v error -select_streams a:0 -show_entries stream=codec_name,sample_rate,bit_rate output.mp4

# 或使用 MediaInfo
mediainfo output.mp4 | grep -A5 "Audio"
```

---

## 下一步

1. 在真机/模拟器上安装 APK
2. 使用失败的 ProRes MOV 文件测��
3. 检查日志确认自动降级触发
4. 验证输出文件音频格式
5. 完成回归测试

