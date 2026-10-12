---
name: Bug 报告
about: 转码失败 / 崩溃 / 输出异常
labels: bug
title: '[Bug] '
assignees: ''
---

**环境**
- ProResForge 版本:(设置 → 历史页可查,或通知栏/安装页)
- Android 版本 / 机型:
- 输入素材类型:ProRes MOV / MXF / DNG zip / 普通 MP4(可附 ffprobe 摘要,勿传原始素材)

**复现步骤**
1.
2.

**预期 / 实际行为**


**日志**(重要:应用完全离线,无自动上报,请自行附上)
```
adb shell run-as com.proresforge.app cat files/logs/app.log
```
或系统文件选择器授权 `Android/data/com.proresforge.app/files/logs/` 后取 `app.log`,
粘贴出错时间段前后的段落(含 `nativeRun rc=` / `job end: state=` 行最有价值)。

**其他**
- 是否取消了任务后重跑?
- 输出目录剩余空间是否充足?
