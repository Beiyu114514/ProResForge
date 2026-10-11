# ProResForge 第三方组件与开源许可声明(THIRD-PARTY NOTICES)

最后更新:2026-10-07。本文件与各许可证全文随 APK 分发(`assets/licenses/`),在
应用内「历史 → 开源许可」可直接查看。

## 1. 本应用

ProResForge 转码炉(包名 `com.proresforge.app`)以 **MIT 许可证**发布,版权所有
(c) 2025 ProResForge contributors。MIT 全文随仓库根目录 `LICENSE` 分发。

## 2. 随包 native 组件(静态/动态链接)

当前发布的二进制以 `--enable-gpl --enable-version3` 配置构建(x264/x265 为 GPL
组件),按 **GPL-3.0** 整体分发。版本与 provenance 见 `prebuilt/BUILDINFO` 与
`prebuilt/<abi>/libraw-manifest-<abi>`(app 源码仓库内)。

| 组件 | 版本(产物版本头实测) | 许可证 | 文本文件 |
|---|---|---|---|
| FFmpeg(libavformat/avcodec/avfilter/avutil/swscale/swresample/postproc) | n6.1.6(libavcodec 60.31.102) | LGPL-2.1-or-later;启用 GPL 组件后整体按 **GPL-3.0**(含 version3) | GPL-3.0.txt |
| x264 | stable @ b35605ac(X264_BUILD 165) | **GPL-2.0-or-later** | GPL-2.0.txt |
| x265 | tag 4.2(X265_BUILD 216) | **GPL-2.0-or-later** | GPL-2.0.txt |
| libvpx(VP9) | tag v1.17.0(vpx.pc 1.17.0) | BSD-3-Clause(另含部分 BSD 类许可文件,见其源码树) | BSD-3-Clause.txt |
| LibRaw(DNG 解码) | tag 0.21.4 | **LGPL-2.1-or-later 或 CDDL-1.0** 双许可(本分发选用 LGPL-2.1) | LGPL-2.1.txt、CDDL-1.0.txt |
| zlib(NDK sysroot,LNG zip Deflate) | NDK r27 附带 | Zlib | Zlib.txt |

### 源码获取方式

- FFmpeg:`https://ffmpeg.org/download.html`,tag `n6.1.6`
  (commit `f1e3a2bf7a2f2cde936d1ed97f09a26853d20125`,2026-06-20);
- x264:`https://code.videolan.org/videolan/x264`,commit `b35605ace3ddf7c1a5d67a2eb553f034aef41d55`
  (stable 分支,X264_BUILD 165);
- x265:`https://bitbucket.org/multicoreware/x265_git`,tag `4.2`
  (commit `e444744c03978c1fb4e037168967020cf2648427`);
- libvpx:`https://chromium.googlesource.com/webm/libvpx`,tag `v1.17.0`
  (commit `6df3ec34557879fff673706f4a1d9fbd0f3a6f0e`);
- LibRaw:`https://www.libraw.org`,tag `0.21.4`(构建经 LibRaw-cmake,`https://github.com/LibRaw/LibRaw-cmake`,
  commit `eb98e4325aef2ce85d2eb031c2ff18640ca616d3`,线程安全 target `raw_r`);
- 本应用与上述全部组件的构建脚本、pin 表与产物对账记录见 app 源码仓库
  (`scripts/build-ffmpeg-android.sh`、`scripts/verify-prebuilt.sh`、`prebuilt/BUILDINFO`)。

### GPL-3.0 §6 书面源码 offer

本应用按 **GNU General Public License v3(含 --enable-version3)** 分发,包含
FFmpeg、x264、x265 的二进制。据此,你可以在遵守 GPLv3 的前提下获得**对应完整
源码**(Corresponding Source,含全部构建脚本、版本 pin 表与 provenance 记录),
方式为以下之一:

1. **随包获取(首选)**:对应源码打包为 `ProResForge-source-v1.0.0.zip`,
   与本 APK 在**同一发布条目**中一并分发(即:你下载本 APK 的页面/渠道同时
   提供该源码包,二者一一对应,均由 tag `v1.0.0` 产出);
2. **书面索取**:上述发布条目未附带源码包时,可自本 NOTICE 随附的发布页面
   载明的联系方式向分发者书面索取,分发者收取不超过物理传输成本的费用提供之。

**要约人**:ProResForge contributors。
**有效期**:本 offer 自 2026-10-11 起**至少三年内**对上述任一分发副本持续有效
(GPL-3.0 §6(b):随每个副本附带的 offer 对其所有接收者有效,为期三年)。

源码包内容自检:解开后应含 `scripts/build-ffmpeg-android.sh`(可复现整套
FFmpeg/x264/x265/libvpx/LibRaw 交叉编译)、`app/src/main/cpp/prebuilt/BUILDINFO`
与 `app/src/main/cpp/prebuilt/<abi>/libraw-manifest-<abi>`(pin 与 provenance
记录)。版本对应关系见上节「源码获取方式」。

## 3. dcraw 归属

LibRaw 内含源自 **dcraw**(Dave Coffin)的代码,按 **CDDL-1.0** 授权;
LibRaw 上游以 CDDL-1.0/LGPL-2.1 双许可形式再发布。文本见 CDDL-1.0.txt。

## 4. AndroidX / Kotlin 运行时

androidx.core、androidx.activity、androidx.lifecycle、androidx.documentfile、
Compose(UI/Graphics/Material3/Foundation)、com.google.android.material、
kotlinx.coroutines、kotlinx.serialization 均按 **Apache License 2.0** 授权,
版权各归其作者(版权归 Android Open Source Project / JetBrains s.r.o. 等)。
文本见 Apache-2.0.txt。

> 注:构建配置此前对 `META-INF/{AL2.0,LGPL2.1}` 的排除只影响依赖 jar 内散落的
> 单行许可文件;本目录携带完整文本,第 4 节即其对应声明。
