#!/usr/bin/env bash
#
# build-ffmpeg-android.sh —— 为 ProResForge 交叉编译 FFmpeg 6.1 及其依赖
#
# 用法:
#   NDK=/path/to/android-ndk ./scripts/build-ffmpeg-android.sh [arm64|arm|all]
#
#   例:
#     NDK=~/android-ndk-r26d ./scripts/build-ffmpeg-android.sh arm64   # 只编 arm64-v8a
#     NDK=~/android-ndk-r26d ./scripts/build-ffmpeg-android.sh all     # 两个 ABI 全编(默认)
#
#   依赖(host 侧):git make yasm perl python3 cmake pkg-config
#     * x265 只能用 cmake 构建,所以 cmake(>= 3.15)必需;
#     * LibRaw 只能用 cmake 构建(LibRaw/LibRaw.git 源码 + LibRaw/LibRaw-cmake.git
#       脚本),依赖 NDK sysroot 自带的 zlib(不自建);
#     * libx264 / libx265 / libvpx 的探测走 pkg-config,产物装到同一 prefix 后用
#       PKG_CONFIG_PATH 找到(所以编译顺序必须是 x264 → x265 → libvpx → FFmpeg);
#     * yasm / perl / python3 / nasm 只在编 x86_64 时硬性需要,ARM 构建缺失仅告警。
#
#   NDK 探测顺序:NDK → ANDROID_NDK_HOME → ANDROID_NDK_ROOT → NDK_HOME → ~/android-ndk-*
#   兼容 NDK r25 / r26 / r27(用 toolchains/llvm/prebuilt/<host>/bin 下按 API 版本命名的 clang)。
#
#   源码仓与构建树默认放 ~/.prf-ffmpeg-build/(不能提交 git,也不该污染仓库;
#   想换位置用 PRF_BUILD_DIR=/path 覆盖),
#   最终产物装到 app/src/main/cpp/prebuilt/<abi>/{include,lib}(该目录被仓库根
#   .gitignore 忽略、不入 git,理由见 app/src/main/cpp/prebuilt/README.md)。
#   构建结束后在 prebuilt/ 根写一份 BUILDINFO(pin 的 commit / NDK / 脚本 sha256),
#   便于核对这份产物到底由什么版本构建。
#
#   耗时:8 核机器全量(arm64 + armeabi-v7a)约 20~40 分钟。
#
#   幂等:源码目录已存在则跳过 clone;LibRaw 侧「产物存在且 per-ABI provenance
#        manifest 全部命中」才跳过该 ABI(见 4b.1 段A 门禁 —— 修订前的"文件在就
#        跳"挡不住单线程 raw / 手工拷库);想强制重建可只删
#        app/src/main/cpp/prebuilt/<abi>/lib/libraw.a。
#
set -euo pipefail

# ---------------------------------------------------------------------------
# 基本变量
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SRC="${PRF_BUILD_DIR:-${HOME:-/tmp}/.prf-ffmpeg-build}"  # 源码 + 构建中间产物
DEST="${ROOT}/app/src/main/cpp/prebuilt"           # 安装目标(契约 §6)
API_LEVEL=24                                       # 契约 §6 / Android minSdk 24

# ---------------------------------------------------------------------------
# 版本 pin —— 可复现构建(AUDIT-2026-10-06 M-8/N-4)
#
#   修订前:FFmpeg 跟 n6.1 分支 tip(移动目标,实测产物 libavcodec 60.31.102,而
#   6.1.0 tag 的版本头是 60.31.100);x264/x265/libvpx/LibRaw-cmake 全部跟上游
#   默认分支(clone_once 第三参数 "-"),即"今天 clone 到 tip、明天 clone 到另一个
#   tip"。现在每个源码都 pin 到发布 tag 或提交 SHA(x264 上游一个 tag 都没有,
#   只能 pin stable 分支 + 提交;LibRaw-cmake 同),clone 后由 verify_pin() 复核,
#   不一致直接 die,防止上游强推 / 镜像漂移悄悄改变产物。
#
#   有意升级:改 tag/SHA、重跑脚本、提交产物与 BUILDINFO 一起更新。换机构建前
#   先同步这里的 SHA(verify_pin 失败信息会写清 HEAD 与期望值)。
#   全部可用同名环境变量覆盖(内网镜像 / 临时试新版)。
# ---------------------------------------------------------------------------
# S-1(2026-10-07):pin 与现存产物版本头逐项对齐并经上游核实:
#   FFmpeg n6.1.6 = libavcodec 60.31.102(与 prebuilt include/ 实测一致,ffmpeg.org 核对);
#   x264 b35605ac = X264_BUILD 165(实测一致);x265 4.2 = X265_BUILD 216(实测一致,
#   旧 pin 4.1@1d117bed 是 BUILD 215,与产物矛盾);libvpx v1.17.0 = vpx.pc 1.17.0(实测一致)。
# 注:现存的 FFmpeg/x264/x265/vpx 二进制由更早的脚本版本构建;版本头与下表一致,
# commit 级归属以下一次完整重建为准(见 prebuilt/BUILDINFO 的 provenance 说明)。
FFMPEG_TAG="${FFMPEG_TAG:-n6.1.6}"   # 2026-06-20(libavcodec 60.31.102)
X264_REF="${X264_REF:-stable}"       # 上游无 tag,只能 pin 分支 + 提交 SHA
X264_SHA="${X264_SHA:-b35605ace3ddf7c1a5d67a2eb553f034aef41d55}"   # stable head 2025-06-08, X264_BUILD 165
X265_TAG="${X265_TAG:-4.2}"          # X265_BUILD 216(4.1 是 215,与产物不符)
X265_SHA="${X265_SHA:-e444744c03978c1fb4e037168967020cf2648427}"
VPX_TAG="${VPX_TAG:-v1.17.0}"        # 2026-08-21,与产物 vpx.pc 1.17.0 一致
VPX_SHA="${VPX_SHA:-6df3ec34557879fff673706f4a1d9fbd0f3a6f0e}"
LIBRAW_CMAKE_SHA="${LIBRAW_CMAKE_SHA:-eb98e4325aef2ce85d2eb031c2ff18640ca616d3}"  # master head 2024-04-15

# 源码地址(可用环境变量覆盖,便于内网镜像)
FFMPEG_REPO="${FFMPEG_REPO:-https://git.ffmpeg.org/ffmpeg.git}"
FFMPEG_MIRROR="${FFMPEG_MIRROR:-https://github.com/FFmpeg/FFmpeg.git}"
X264_REPO="${X264_REPO:-https://code.videolan.org/videolan/x264.git}"
X264_MIRROR="${X264_MIRROR:-https://github.com/mirror/x264.git}"
X265_REPO="${X265_REPO:-https://bitbucket.org/multicoreware/x265_git.git}"
X265_MIRROR="${X265_MIRROR:-https://github.com/videolan/x265.git}"
VPX_REPO="${VPX_REPO:-https://chromium.googlesource.com/webm/libvpx}"
VPX_MIRROR="${VPX_MIRROR:-}"
# LibRaw(DNG 解码:解马赛克 + 相机 WB + 色彩矩阵)。TAG 建议不动,升级前先跑
# M0 样本核对(docs/DNGZIP-PROGRES-PLAN.md)。
#   主源: LibRaw/LibRaw.git(库源码);LibRaw 官方 README.cmake 里说明 git 快照
#   不带 CMakeLists(只有发行包有),CMake 脚本在 LibRaw-cmake.git,所以要两个仓库
#   配合: -S 指向 LibRaw-cmake,-D LIBRAW_PATH 指向 LibRaw 源码。
LIBRAW_REPO="${LIBRAW_REPO:-https://github.com/LibRaw/LibRaw.git}"
LIBRAW_MIRROR="${LIBRAW_MIRROR:-https://github.com/LibRaw/LibRaw.git}"
LIBRAW_TAG="${LIBRAW_TAG:-0.21.4}"
LIBRAW_CMAKE_REPO="${LIBRAW_CMAKE_REPO:-https://github.com/LibRaw/LibRaw-cmake.git}"
LIBRAW_CMAKE_MIRROR="${LIBRAW_CMAKE_MIRROR:-https://github.com/LibRaw/LibRaw-cmake.git}"
# Compression=8 的老基线 JPEG DNG 才需要 libjpeg-turbo(默认关;MotionCam 实测
# Compression=7 无损 JPEG,LibRaw 内置 LJpeg 解码器,不依赖 libjpeg)。
LIBRAW_JPEG="${LIBRAW_JPEG:-OFF}"

log()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m[warn] %s\033[0m\n' "$*" >&2; }
die()  { printf '\033[1;31m[error] %s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 参数
# ---------------------------------------------------------------------------
ARG="${1:-all}"
case "${ARG}" in
    arm64|arm64-v8a)       ABIS=(arm64-v8a) ;;
    arm|armv7|armeabi-v7a) ABIS=(armeabi-v7a) ;;
    x8664|x86_64)          ABIS=(x86_64) ;;
    all)                   ABIS=(arm64-v8a armeabi-v7a) ;;
    *) die "未知 ABI: ${ARG}(可选 arm64 / arm / x86_64 / all)" ;;
esac

X86_BUILD=0
for a in "${ABIS[@]}"; do
    if [[ "${a}" == "x86_64" ]]; then X86_BUILD=1; fi
done

# ---------------------------------------------------------------------------
# 依赖检查
# ---------------------------------------------------------------------------
for t in git make cmake pkg-config; do
    command -v "${t}" >/dev/null 2>&1 || die "缺少依赖: ${t} —— 请先安装(如 apt install ${t})"
done
if [[ ${X86_BUILD} -eq 1 ]]; then
    # x264 / libvpx / x265 的 x86 汇编需要 yasm/nasm;perl、python3 同样只在这种情况下必需
    for t in yasm nasm perl python3; do
        command -v "${t}" >/dev/null 2>&1 || die "缺少依赖: ${t}(x86_64 编解码库汇编需要)"
    done
else
    for t in yasm nasm perl python3; do
        if ! command -v "${t}" >/dev/null 2>&1; then
            warn "未安装 ${t}(本次 ARM 构建不需要)"
        fi
    done
fi

# ---------------------------------------------------------------------------
# NDK 探测
# ---------------------------------------------------------------------------
if [[ -z "${NDK:-}" ]]; then
    for cand in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" "${NDK_HOME:-}"; do
        if [[ -n "${cand}" && -d "${cand}" ]]; then NDK="${cand}"; break; fi
    done
fi
if [[ -z "${NDK:-}" ]]; then
    for cand in "${HOME}"/android-ndk-*; do
        if [[ -d "${cand}" ]]; then NDK="${cand}"; break; fi
    done
fi
if [[ -z "${NDK:-}" || ! -d "${NDK}" ]]; then
    die "未找到 Android NDK;请用 NDK=/path/to/ndk 指定"
fi
NDK="$(cd "${NDK}" && pwd)"

TC_HOST="linux-x86_64"
case "$(uname -s)" in
    Linux)  TC_HOST="linux-x86_64" ;;
    Darwin) TC_HOST="darwin-x86_64" ;;
    *) die "不支持的构建机系统: $(uname -s)" ;;
esac
TC="${NDK}/toolchains/llvm/prebuilt/${TC_HOST}"
[[ -x "${TC}/bin/clang" ]] || die "NDK 工具链不存在: ${TC}/bin/clang(需要 r25 及以上)"

log "NDK        = ${NDK}"
log "toolchain  = ${TC}"
log "源码目录   = ${SRC}"
log "安装目录   = ${DEST}"
log "目标 ABI   = ${ABIS[*]}"

JOBS="$(nproc 2>/dev/null || echo 4)"

# clone_once <dir> <url> <branch|-> <mirror|->
clone_once() {
    local dir="$1" url="$2" branch="$3" mirror="$4"
    if [[ -d "${dir}/.git" ]]; then
        echo "[skip] 源码已存在: ${dir}(如需重新 clone 请先删除)"
        return 0
    fi
    mkdir -p "$(dirname "${dir}")"
    local args=(clone --depth 1)
    if [[ "${branch}" != "-" ]]; then args+=(-b "${branch}"); fi
    log "clone ${url}"
    if ! git "${args[@]}" "${url}" "${dir}"; then
        if [[ "${mirror}" == "-" ]]; then die "clone 失败: ${url}"; fi
        warn "主源失败,改用镜像 ${mirror}"
        git "${args[@]}" "${mirror}" "${dir}" || die "clone 失败: ${url} / ${mirror}"
    fi
}

# verify_pin <dir> <sha | --ref:期望tag>
#   clone 之后复核 HEAD 与 pin 一致:sha 模式比对 git rev-parse HEAD;
#   --ref:X 模式比对 git describe --tags --exact-match(用于拿不到 push SHA 的镜像)。
#   做不到校验(源码目录不是 git 仓)或校验失败都直接 die —— pin 的意义就在于此
#   (AUDIT M-8:换机 clone 到别的 tip,产物随版本漂移,且无人能发现)。
verify_pin() {
    local dir="$1" want="$2" got
    if [[ "${want}" == --ref:* ]]; then
        local ref="${want#--ref:}"
        got="$(git -C "${dir}" describe --tags --exact-match 2>/dev/null || true)"
        [[ -n "${got}" ]] || die "pin 校验失败: ${dir} 的 HEAD 不在任何 tag 上(期望 ${ref})"
        [[ "${got}" == "${ref}" ]] \
            || die "pin 校验失败: ${dir} HEAD tag=${got},期望 ${ref}"
    else
        got="$(git -C "${dir}" rev-parse HEAD 2>/dev/null || true)"
        [[ -n "${got}" ]] || die "pin 校验失败: ${dir} 不是可用的 git 仓库"
        [[ "${got}" == "${want}" ]] \
            || die "pin 校验失败: ${dir} HEAD=${got},期望 pin ${want}(换机请先同步 pin)"
    fi
}

# ---------------------------------------------------------------------------
# LibRaw provenance(来源)manifest —— 段A / AUDIT-2026-10-06 P1-01 + P2-02
#
#   背景:prebuilt/<abi>/lib/libraw.a 以前只看"文件在不在"。存在性检查挡不住两种情况:
#     a) 单线程版(LibRaw-cmake target raw,带 LIBRAW_NOTHREADS)被当成 libraw.a 装上;
#     b) 有人手工把 lib/ 拷进来(prebuilt/BUILDINFO 就记录过一次 manual 替换,
#        没有任何 pin/NDK/脚本脚本记录能证明它是线程安全版)。
#   两种都会在设备上表现为"多 worker 并发解同一帧,输出每次都不一样"(函数内 static
#   位缓冲被互踩),而存在性检查一句抱怨都没有。
#
#   现在:每个 ABI 一个 manifest
#       prebuilt/<abi>/libraw-manifest-<abi>   (key=value 文本,允许 # 注释行)
#   本段写它,install 成功之后立刻写;scripts/verify-prebuilt.sh 读它,CMake / Gradle
#   (app/src/main/cpp/CMakeLists.txt、app/build.gradle.kts)在消费 prebuilt 之前读它。
#   字段含义/生成方式/门禁行为见 app/src/main/cpp/prebuilt/README.md。
#
#   注意:本段函数独立于 build_abi() 的局部变量,方便 stub / 单测直接调用
#   (tmp/ad-stub/ 的验收就是这么跑的)。
# >>> prf-libraw-manifest-begin

# ndk_revision —— NDK 的 Pkg.Revision(记进 manifest;换 NDK 时必须能发现)
ndk_revision() {
    local rev=""
    [[ -f "${NDK}/source.properties" ]] \
        && rev="$(sed -n 's/^Pkg.Revision *= *//p' "${NDK}/source.properties" | head -1)"
    printf '%s\n' "${rev:-unknown}"
}

# nm_symbols_sha <lib> <llvm-nm> —— 存档内全量 defined symbol 规范化 sha256
#   流水线必须与 scripts/verify-prebuilt.sh:normalize_symbols_sha() 一字不差,
#   否则"复算一致"这句话没有意义。llvm-nm 的存档输出是
#     canon_600.cpp.o:            <- 成员头行(第 3 列取不到,NF<3,被过滤)
#     000000000000a000 T __Z7...   <- 符号行:第 3 列就是 mangled 名
#   排序 + `sort -u` 让符号顺序不影响 sha;符号集合变化(换源码/换参数/换工具链/
#   单线程版把 TLS 换成函数内 static 也会改变符号)必然改变 sha。
nm_symbols_sha() {
    local lib="$1" nm="$2"
    "${nm}" --defined-only "${lib}" 2>/dev/null \
        | awk 'NF>=3 {print $3}' | LC_ALL=C sort -u | sha256sum | awk '{print $1}'
}

# ar_members_sha <lib> <llvm-ar> —— 存档成员清单排序去重后 sha256
#   (混入 / 漏掉一个 .o 都会变;`raw` 与 `raw_r` 的成员目录不同、名字相同,靠
#    members sha 只能看成员清单,真正区分 raw/raw_r 的是下面 libraw_nothreads
#    与 symbols_sha256(_TLS 相关符号与函数内 static 版本不同)。)
ar_members_sha() {
    local lib="$1" ar="$2"
    "${ar}" t "${lib}" 2>/dev/null | LC_ALL=C sort -u | sha256sum | awk '{print $1}'
}

# mget <manifest> <key> —— 读一个 key 的值(键不存在 / 重复出现都返回空)
#   manifest 是 key=value 文本(# 注释行与空行跳过),CMake file(STRINGS)、bash、
#   Kotlin 三边都能解析,所以这里刻意不用 jq 之类的额外依赖。
mget() {
    local f="$1" k="$2" v n
    v="$(sed -n "s/^${k}=//p" "${f}" 2>/dev/null | head -1)"
    n="$(sed -n "s/^${k}=/x/p" "${f}" 2>/dev/null | wc -l)"
    [[ "${n}" -eq 1 ]] || return 1
    printf '%s\n' "${v}"
}

# tls_probe <lib> <llvm-nm> —— 线程安全的旁证(信息性,见 manifest 字段注释)
#   LibRaw 的 LibRaw_TLS(per-instance 线程状态)只在线程安全编译单元里被引用/
#   定义:decoders_dcraw.cpp:22-31 在非 NOTHREADS 分支把 bitbuf 换成
#   tls->getbits.bitbuf。实测 2026-10-06 那份 prebuilt 的 arm64 libraw.a:
#   2446 条符号里 0 条 LibRaw_TLS、0 条 pthread_* —— 与单线程(LIBRAW_NOTHREADS)
#   版一致,这正是段A 要挡的东西。
#   注意:这是旁证,不单独当硬门禁 —— 真凭据是「本脚本构建 + flags 里没有
#   LIBRAW_NOTHREADS + 存档 sha 一致」三者合一;LibRaw 升级可能改变 TLS 的符号
#   呈现方式,那时 tls_probe=absent 只提示人工复核,不该把合法的 raw_r 判死。
tls_probe() {
    local lib="$1" nm="$2"
    if "${nm}" "${lib}" 2>/dev/null | grep -q 'LibRaw_TLS'; then echo present; else echo absent; fi
}

# libraw_provenance_ok —— 段A 门禁判定(纯函数:不删文件、不构建、不 die)
#   用法: libraw_provenance_ok <prefix> <abi> <api_level> <ndk_revision>
#             <libraw_tag> <libraw_cmake_sha> <script_sha256> <llvm-nm> <llvm-ar>
#   stdout:每条不符原因一行(空输出 = 全部命中);返回码 0 = 可复用,1 = 必须重建。
#   build_abi() 的 install 段据此决定 skip / force-rebuild;tmp/ad-stub 的单测
#   直接调用本函数验证「删库重建」判定(不真跑 NDK 构建)。
#   判据(任一不符即强制重建):
#     1) 库 / 头 / manifest 三样齐全;
#     2) manual=false、libraw_nothreads=absent、raw_r_target=raw_r;
#     3) abi / api_level / ndk_revision / libraw_tag / libraw_cmake_sha /
#        script_sha256 与本次调用方给出的一致(换 NDK、换 pin、改脚本都会触发);
#     4) raw_r_cmake_flags 里没有 LIBRAW_NOTHREADS(出现即单线程版特征);
#     5) libraw_sha256 等于 lib/libraw.a 的真实 sha256;
#     6) symbols_sha256 / members_sha256 复算一致(存档内容被动过就变)。
#   没有 llvm-nm/llvm-ar 时降级丢掉第 6 条(仅警告),其余照判。
libraw_provenance_ok() {
    local prefix="$1" abi="$2" api_level="$3" ndk_rev="$4"
    local libraw_tag="$5" libraw_cmake_sha="$6" script_sha="$7"
    local nm_bin="$8" ar_bin="$9"
    local mfile="${prefix}/libraw-manifest-${abi}"
    local raw_a="${prefix}/lib/libraw.a"
    local -a bad=()
    local pair k want act v cur

    if [[ ! -f "${raw_a}" || ! -f "${prefix}/include/libraw/libraw.h" ]]; then
        printf '%s\n' "libraw.a / libraw.h 缺失(${raw_a})"
        return 1
    fi
    if [[ ! -f "${mfile}" ]]; then
        printf '%s\n' "manifest 缺失(${mfile})—— 无法证明该 libraw.a 是线程安全 raw_r 构建的产物"
        return 1
    fi

    for pair in "manual:false" \
                "libraw_nothreads:absent" \
                "raw_r_target:raw_r" \
                "abi:${abi}" \
                "api_level:${api_level}" \
                "ndk_revision:${ndk_rev}" \
                "libraw_tag:${libraw_tag}" \
                "libraw_cmake_sha:${libraw_cmake_sha}" \
                "script_sha256:${script_sha}"; do
        k="${pair%%:*}"; want="${pair#*:}"
        act="$(mget "${mfile}" "${k}" || true)"
        if [[ -z "${act}" ]]; then
            bad+=("manifest 缺字段 ${k}(或该键重复出现)")
        elif [[ "${act}" != "${want}" ]]; then
            bad+=("${k} 不符: manifest=${act},本次=${want}")
        fi
    done

    v="$(mget "${mfile}" raw_r_cmake_flags || true)"
    if [[ -z "${v}" ]]; then
        bad+=("manifest 缺字段 raw_r_cmake_flags")
    elif [[ "${v}" == *LIBRAW_NOTHREADS* ]]; then
        bad+=("raw_r_cmake_flags 里出现 LIBRAW_NOTHREADS:${v}")
    fi

    v="$(mget "${mfile}" libraw_sha256 || true)"
    cur="$(sha256sum "${raw_a}" | awk '{print $1}')"
    if [[ -z "${v}" ]]; then
        bad+=("manifest 缺字段 libraw_sha256")
    elif [[ "${v}" != "${cur}" ]]; then
        bad+=("libraw.a sha256 不符: manifest=${v},实际=${cur}(库文件被换过?)")
    fi

    if [[ -x "${nm_bin}" && -x "${ar_bin}" ]]; then
        local want_syms want_mem cur_syms cur_mem
        want_syms="$(mget "${mfile}" symbols_sha256 || true)"
        want_mem="$(mget "${mfile}" members_sha256 || true)"
        cur_syms="$(nm_symbols_sha "${raw_a}" "${nm_bin}")"
        cur_mem="$(ar_members_sha "${raw_a}" "${ar_bin}")"
        if [[ -z "${want_syms}" || -z "${want_mem}" ]]; then
            bad+=("manifest 缺字段 symbols_sha256 / members_sha256")
        else
            if [[ "${want_syms}" != "${cur_syms}" ]]; then
                bad+=("符号集 sha256 不符: manifest=${want_syms},实际=${cur_syms}(存档内容被动过?)")
            fi
            if [[ "${want_mem}" != "${cur_mem}" ]]; then
                bad+=("成员清单 sha256 不符: manifest=${want_mem},实际=${cur_mem}(存档内容被动过?)")
            fi
        fi
    else
        warn "[${abi}] 宿主没有可用的 llvm-nm / llvm-ar,降级:符号/成员 sha 不复算"
    fi

    if [[ ${#bad[@]} -eq 0 ]]; then
        return 0
    fi
    printf '%s\n' "${bad[@]}"
    return 1
}

# write_libraw_manifest —— 生成 per-ABI provenance manifest
#   用法: write_libraw_manifest <prefix> <abi> <api_level> <ndk_revision>
#             <libraw_tag> <libraw_cmake_sha> <raw_r_cmake_flags>
#             <script_sha256> <llvm-nm> <llvm-ar>
#   * raw_r_cmake_flags 是「实际使用」的 flags(不含 LIBRAW_NOTHREADS;build_abi 里
#     拼 raw_r_flags 的地方就是它的出处);
#   * 只写文件、不碰 libraw.a;调用方负责在 install 校验通过之后才调它。
write_libraw_manifest() {
    local prefix="$1" abi="$2" api_level="$3" ndk_rev="$4"
    local libraw_tag="$5" libraw_cmake_sha="$6" raw_r_flags="$7" script_sha="$8"
    local nm_bin="$9" ar_bin="${10}"
    local out="${prefix}/libraw-manifest-${abi}"
    local raw_a="${prefix}/lib/libraw.a" raw_h="${prefix}/include/libraw/libraw.h"
    local syms_sha members_sha raw_sha tls

    [[ -f "${raw_a}" ]] || die "write_libraw_manifest: 缺 ${raw_a}(先装好 raw_r 再写 manifest)"
    [[ -f "${raw_h}" ]] || die "write_libraw_manifest: 缺 ${raw_h}"
    [[ "${raw_r_flags}" != *LIBRAW_NOTHREADS* ]] \
        || die "write_libraw_manifest: flags 里出现 LIBRAW_NOTHREADS(${raw_r_flags})"
    [[ -x "${nm_bin}" && -x "${ar_bin}" ]] \
        || die "write_libraw_manifest: 需要 NDK llvm-nm / llvm-ar(nm=${nm_bin} ar=${ar_bin})"

    # key=value 行不容忍空白:LIBRAW_PATH 里的源码目录可能带空格,压成一行
    raw_r_flags="$(printf '%s' "${raw_r_flags}" | tr -d '[:space:]')"
    raw_sha="$(sha256sum "${raw_a}" | awk '{print $1}')"
    syms_sha="$(nm_symbols_sha "${raw_a}" "${nm_bin}")"
    members_sha="$(ar_members_sha "${raw_a}" "${ar_bin}")"
    tls="$(tls_probe "${raw_a}" "${nm_bin}")"
    [[ -n "${raw_sha}" && -n "${syms_sha}" && -n "${members_sha}" ]] \
        || die "write_libraw_manifest: sha 探针失败(raw=${raw_sha} syms=${syms_sha} members=${members_sha})"
    if [[ "${tls}" != "present" && "${tls}" != "absent" ]]; then tls="unknown"; fi

    cat > "${out}" <<EOF
# ${out} —— 由 scripts/build-ffmpeg-android.sh 自动生成,勿手改
# 字段含义 / 生成方式 / 门禁行为见 app/src/main/cpp/prebuilt/README.md。
abi=${abi}
api_level=${api_level}
ndk_revision=${ndk_rev}
libraw_tag=${libraw_tag}
libraw_cmake_sha=${libraw_cmake_sha}
raw_r_target=raw_r
libraw_nothreads=absent
raw_r_cmake_flags=${raw_r_flags}
threads_lib=pthread
script_sha256=${script_sha}
libraw_sha256=${raw_sha}
symbols_sha256=${syms_sha}
members_sha256=${members_sha}
tls_probe=${tls}
build_time_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
manual=false
EOF
    if [[ "${tls}" == "absent" ]]; then
        warn "[${abi}] tls_probe=absent:libraw.a 里没有 LibRaw_TLS 符号 —— 请复核这次 raw_r 构建(README「仍需 HOST_TEST 覆盖」)"
    fi
    log "${abi} 已写 provenance manifest: ${out}"
}
# <<< prf-libraw-manifest-end

# ---------------------------------------------------------------------------
# 单个 ABI
#   $1 = Android ABI 目录名(arm64-v8a / armeabi-v7a / x86_64)
# ---------------------------------------------------------------------------
build_abi() {
    local abi="$1"
    local prefix="${DEST}/${abi}"
    local triplet arch cpu vpx_target extra_cflags

    case "${abi}" in
        arm64-v8a)
            triplet="aarch64-linux-android"; arch="aarch64"
            vpx_target="arm64-android-gcc"; cpu=""
            extra_cflags="" ;;
        armeabi-v7a)
            triplet="armv7a-linux-androideabi"; arch="arm"
            vpx_target="armv7-android-gcc"; cpu="armv7-a"
            extra_cflags="-march=armv7-a -mfloat-abi=softfp -mfpu=neon" ;;
        x86_64)
            triplet="x86_64-linux-android"; arch="x86_64"
            vpx_target="x86_64-android-gcc"; cpu=""
            extra_cflags="" ;;
        *) die "未知 ABI: ${abi}" ;;
    esac

    # 版本化 clang(r25/r26/r27 都有 <triplet>24-clang;--target 与 sysroot 都内置)
    local CC_BIN="${TC}/bin/${triplet}${API_LEVEL}-clang"
    local CXX_BIN="${TC}/bin/${triplet}${API_LEVEL}-clang++"
    if [[ ! -x "${CC_BIN}" ]]; then CC_BIN="${TC}/bin/${triplet}-clang"; fi
    if [[ ! -x "${CXX_BIN}" ]]; then CXX_BIN="${TC}/bin/${triplet}-clang++"; fi
    if [[ ! -x "${CC_BIN}" ]]; then
        die "找不到 clang: ${TC}/bin/${triplet}${API_LEVEL}-clang"
    fi
    local AR_BIN="${TC}/bin/llvm-ar"
    local RANLIB_BIN="${TC}/bin/llvm-ranlib"
    local NM_BIN="${TC}/bin/llvm-nm"
    local STRIP_BIN="${TC}/bin/llvm-strip"
    local SYSROOT="${TC}/sysroot"

    # FFmpeg 产物已存在则跳过它的编译 —— 但不整体 return:DNG zip 支持引入后,
    # 老产物目录里没有 libraw.a,必须让下面的 LibRaw 段继续跑(它有各自的 skip)。
    local ff_skip=0
    if [[ -f "${prefix}/lib/libavcodec.so" && -f "${prefix}/include/libavcodec/avcodec.h" ]]; then
        ff_skip=1
        log "[skip] ${abi} FFmpeg 产物已存在: ${prefix}(想重建请先删除该目录)"
    fi
    mkdir -p "${prefix}"

    # 公共编译选项:契约 §6 要求 -fPIC -O3 -fno-strict-aliasing
    local common_cflags="-fPIC -O3 -fno-strict-aliasing"
    if [[ -n "${extra_cflags}" ]]; then
        common_cflags="${common_cflags} ${extra_cflags}"
    fi
    local cpu_flag=""
    if [[ -n "${cpu}" ]]; then cpu_flag="--cpu=${cpu}"; fi

    # -----------------------------------------------------------------------
    # 1) x264 —— 静态库,只用于 H.264 软编
    # -----------------------------------------------------------------------
    local x264_dir="${SRC}/x264"
    clone_once "${x264_dir}" "${X264_REPO}" "${X264_REF}" "${X264_MIRROR}"
    verify_pin "${x264_dir}" "${X264_SHA}"
    if [[ ! -f "${prefix}/lib/libx264.a" ]]; then
        log "[${abi}] 编译 libx264"
        (
            cd "${x264_dir}"
            make distclean >/dev/null 2>&1 || true
            # NDK r23+ 已删除 gcc 命名包装器(xxx-gcc),cross-prefix 找不到编译器,
            # 必须显式传 CC/AR/RANLIB/STRIP/STRINGS(x264 configure 支持这些 env 覆盖;
            # STRINGS 缺失会直接死在 endian test)
            CC="${CC_BIN}" CXX="${CXX_BIN}" \
            AR="${AR_BIN}" RANLIB="${RANLIB_BIN}" STRIP="${STRIP_BIN}" \
            STRINGS="${TC}/bin/llvm-strings" \
            ./configure \
                --prefix="${prefix}" \
                --host="${triplet}" \
                --cross-prefix="${TC}/bin/${triplet}-" \
                --enable-static --enable-pic \
                --disable-cli --disable-opencl \
                --extra-cflags="${common_cflags}" \
            || { tail -40 config.log 2>/dev/null; exit 1; }
            make -j"${JOBS}"
            make install
        )
    else
        log "[skip] ${abi} libx264.a 已安装"
    fi

    # -----------------------------------------------------------------------
    # 2) x265 —— 静态库,H.265 软编;multilib 打开(8/10/12 位 → main10 可用)
    #    x265 是 C++,用 NDK 自带的 cmake toolchain;ANDROID_STL 必须用 c++_static,
    #    否则最终 .so 会多出一个 libc++_shared.so 的运行时依赖。
    # -----------------------------------------------------------------------
    clone_once "${SRC}/x265" "${X265_REPO}" "${X265_TAG}" "${X265_MIRROR}"
    verify_pin "${SRC}/x265" "${X265_SHA}"
    local x265_src="${SRC}/x265/source"
    # x265 的 .S 走裸 add_custom_command(clang++ 直接编),不带 --target,NDK r19+
    # 的 clang++ 会按构建机 x86_64 编,必报 "unknown target CPU"。给 arm64 分支补
    # 目标三元组;SVE/dotprod/I8MM 汇编依赖 GNU as 特性,NDK 只有集成汇编器,关掉
    # (NEON 基础汇编保留)。补丁只打一次(x265 源码树两个 ABI 共用)。
    if ! grep -q "PRORESFORGE_TARGET_PATCH" "${x265_src}/CMakeLists.txt"; then
        # macOS/BSD sed 的 -i 必须带备份后缀(GNU 的可省),统一用 -i.bak 再删备份,
        # 否则脚本在 macOS 上直接报 "sed: -i may not be used with stdin" 中断
        sed -i.bak "s|elseif(ARM64 OR CROSS_COMPILE_ARM64)|elseif(ARM64 OR CROSS_COMPILE_ARM64) # PRORESFORGE_TARGET_PATCH\n        list(APPEND ARM_ARGS \"--target=${triplet}${API_LEVEL}\")|" \
            "${x265_src}/CMakeLists.txt"
        rm -f "${x265_src}/CMakeLists.txt.bak"
    fi
    if [[ ! -f "${prefix}/lib/libx265.a" ]]; then
        log "[${abi}] 编译 libx265"
        local x265_build="${SRC}/x265/build/${abi}"
        rm -rf "${x265_build}"
        mkdir -p "${x265_build}"
        cmake -S "${x265_src}" -B "${x265_build}" -G "Unix Makefiles" \
            -DCMAKE_TOOLCHAIN_FILE="${NDK}/build/cmake/android.toolchain.cmake" \
            -DANDROID_ABI="${abi}" \
            -DANDROID_PLATFORM="android-${API_LEVEL}" \
            -DANDROID_STL=c++_static \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="${prefix}" \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
            -DENABLE_SHARED=OFF \
            -DENABLE_CLI=OFF \
            -DENABLE_PIC=ON \
            -DENABLE_MULTILIB=ON \
            -DENABLE_NEON_DOTPROD=OFF \
            -DENABLE_NEON_I8MM=OFF \
            -DENABLE_SVE=OFF \
            -DENABLE_SVE2=OFF \
            -DENABLE_SVE2_BITPERM=OFF \
            -DENABLE_TESTS=OFF
        cmake --build "${x265_build}" -j"${JOBS}"
        make -C "${x265_build}" install
    else
        log "[skip] ${abi} libx265.a 已安装"
    fi
    # x265 只在完整 tag clone 时才装 .pc;而我们用 --depth 1 clone,拿不到它的 tag
    # 描述、ffmpeg configure 又靠 pkg-config 找 x265 —— 手写一份(幂等)。
    # Version 取源码 git describe 的真实输出(修订前写死 "4.1",与实际 clone 到哪个
    # tag 无关,随产物分发时会骗人,AUDIT M-8);X265_BUILD 从装好的
    # x265_config.h 读出来附在 Description 里,便于逐一核对产物版本。
    # 静态库的 C++ 运行时由 ffmpeg configure 的 --extra-libs -lc++_static 承担,
    # 这里不用列。
    if [[ ! -f "${prefix}/lib/pkgconfig/x265.pc" ]]; then
        mkdir -p "${prefix}/lib/pkgconfig"
        local x265_ver x265_build_no
        x265_ver="$(git -C "${SRC}/x265" describe --tags --always 2>/dev/null || echo unknown)"
        x265_build_no="$(sed -n 's/^#define X265_BUILD[[:space:]]*\([0-9]\+\).*/\1/p' \
            "${prefix}/include/x265_config.h" 2>/dev/null || true)"
        [[ -n "${x265_build_no}" ]] || x265_build_no="?"
        cat > "${prefix}/lib/pkgconfig/x265.pc" <<EOF
prefix=${prefix}
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: x265
Description: H.265/HEVC video encoder (X265_BUILD ${x265_build_no})
Version: ${x265_ver}
Libs: -L\${libdir} -lx265
Libs.private: -lm -ldl
Cflags: -I\${includedir}
EOF
    fi

    # -----------------------------------------------------------------------
    # 3) libvpx —— 静态库,VP9 编码/解码,含 10 位 highbitdepth
    # -----------------------------------------------------------------------
    local vpx_dir="${SRC}/libvpx"
    clone_once "${vpx_dir}" "${VPX_REPO}" "${VPX_TAG}" "${VPX_MIRROR}"
    verify_pin "${vpx_dir}" "${VPX_SHA}"
    if [[ ! -f "${prefix}/lib/libvpx.a" ]]; then
        log "[${abi}] 编译 libvpx"
        (
            cd "${vpx_dir}"
            make distclean >/dev/null 2>&1 || true
            CC="${CC_BIN}" CXX="${CXX_BIN}" \
            AR="${AR_BIN}" RANLIB="${RANLIB_BIN}" \
            NM="${NM_BIN}" STRIP="${STRIP_BIN}" \
            ./configure \
                --target="${vpx_target}" \
                --prefix="${prefix}" \
                --enable-pic \
                --disable-examples --disable-tools --disable-docs \
            --enable-vp9-encoder --enable-vp9-decoder --enable-vp9-highbitdepth \
            --disable-vp8-encoder --disable-vp8-decoder \
            --disable-neon-asm \
            --extra-cflags="${common_cflags}" \
            || { tail -40 config.log 2>/dev/null; exit 1; }
            make -j"${JOBS}"
            make install
        )
    else
        log "[skip] ${abi} libvpx.a 已安装"
    fi

    # 三个静态库的 .pc 装好后,ffmpeg 的 pkg-config 探测才能找到它们
    export PKG_CONFIG_PATH="${prefix}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

    # -----------------------------------------------------------------------
    # 4) FFmpeg 6.1 —— 共享库,装到同一 prefix
    # -----------------------------------------------------------------------
    local ff_dir="${SRC}/ffmpeg"
    clone_once "${ff_dir}" "${FFMPEG_REPO}" "${FFMPEG_TAG}" "${FFMPEG_MIRROR}"
    verify_pin "${ff_dir}" "--ref:${FFMPEG_TAG}"

    if [[ ${ff_skip} -eq 1 ]]; then
        log "[skip] ${abi} 不重复编译 FFmpeg(保留已安装产物)"
    else
    log "[${abi}] 编译 FFmpeg ${FFMPEG_TAG}"

    (
        cd "${ff_dir}"
        make distclean >/dev/null 2>&1 || true

        # 说明(两处与委托清单/契约字面不同,原因见注释):
        #  * 契约 §6 写的是 --enable-network=no,但 ffmpeg configure 的 --enable-*=值
        #    形式是"组件清单选择"而非布尔赋值(只告警、不生效),关网络必须 --disable-network。
        #  * --enable-jni:configure 6861 行明确要求 mediacodec 依赖 jni,否则直接 die。
        #  * --extra-libs:x265 静态库是 C++,C 驱动(clang,ffmpeg 所有链接都用它)不会
        #    自动补 C++ 运行时,且 operator new/delete 在 libc++abi.a 里、
        #    不在 libc++_static.a 里 —— 必须 -lc++_static -lc++abi 成对出现;
        #    -lm 供 x265 的 exp2/log10 等;这样最终 .so 不会多依赖 libc++_shared.so。
        ./configure \
            --prefix="${prefix}" \
            --enable-cross-compile \
            --target-os=android \
            --arch="${arch}" \
            ${cpu_flag} \
            --cc="${CC_BIN}" \
            --cxx="${CXX_BIN}" \
            --ar="${AR_BIN}" \
            --ranlib="${RANLIB_BIN}" \
            --nm="${NM_BIN}" \
            --strip="${STRIP_BIN}" \
            --sysroot="${SYSROOT}" \
            --extra-cflags="${common_cflags}" \
            --extra-cxxflags="${common_cflags}" \
            --extra-libs="-landroid -lmediandk -lc++_static -lc++abi -lm -ldl" \
            --enable-pic \
            --enable-small \
            --enable-shared \
            --disable-static \
            --enable-gpl \
            --enable-version3 \
            --enable-jni \
            --enable-mediacodec \
            --enable-libx264 \
            --enable-libx265 \
            --enable-libvpx \
            --enable-swscale \
            --enable-avfilter \
            --disable-network \
            --disable-vulkan \
            --disable-doc \
            --disable-ffmpeg \
            --disable-ffplay \
            --disable-ffprobe \
            > ffbuild-config.log 2>&1 \
        || { tail -60 ffbuild-config.log; die "FFmpeg configure 失败(详见 ${ff_dir}/ffbuild-config.log)"; }

        make -j"${JOBS}" > ffbuild-make.log 2>&1 \
        || { tail -60 ffbuild-make.log; die "FFmpeg make 失败(详见 ${ff_dir}/ffbuild-make.log)"; }
        make install > ffbuild-install.log 2>&1 \
        || { tail -60 ffbuild-install.log; die "FFmpeg make install 失败"; }
        # make install 会把 doc/examples 的上游示例源码一起装到 share/ffmpeg/examples/
        # (两 ABI 各 28 个 .c:产物目录变源码快照、平添许可复杂度);proresforge 一个都
        # 不用,直接删(L-N1)。
        rm -rf "${prefix}/share"
    )
    fi

    # -----------------------------------------------------------------------
    # 4.5) 校验编码能力:libx264/x265/vp9/prores 必须在 configure 的产物里
    #
    # 为什么必须查:configure 探测不到外部库时只 warn 不 die(「--enable-libx264」
    # 照样往下走),x264/x265/vpx 静态库也装好了 —— 只要某一步链错,proresforge
    # 就会拿到一个「能 demux 但不能 H.264/H.265/VP9 编码」的 .so,只有真机转码
    # 才暴露。验收依据用 ${ff_dir}/ffbuild/config.mak 而不是事后 nm:
    # 发布用的 .so 被 strip 过,llvm-nm 只剩 171 个公共 API 符号,
    # ff_libx264_encoder 这种内部注册符号查不到(实测 arm64 产物)。
    # -----------------------------------------------------------------------
    local ff_cfg="${ff_dir}/ffbuild/config.mak"
    if [[ ! -f "${ff_cfg}" ]]; then
        die "找不到 FFmpeg 配置产物 ${ff_cfg}(configure 未成功?)"
    fi
    local enc_bad=0
    for enc in libx264 libx265 libvpx-vp9 prores; do
        if grep -q "^CONFIG_$(echo "${enc}" | tr 'a-z-' 'A-Z_')_ENCODER=yes" "${ff_cfg}"; then
            log "[${abi}] 编码器在位: ${enc}"
        else
            warn "FFmpeg 未启用 ${enc} 编码器(CONFIG_..._ENCODER != yes)"
            enc_bad=1
        fi
    done
    if [[ ${enc_bad} -ne 0 ]]; then
        die "${abi} 编码能力不完整,继续只会得到「一半的产物」(详见 ${ff_cfg})"
    fi

    # -----------------------------------------------------------------------
    # 4b) LibRaw —— 静态库,DNG 解码(解马赛克/相机 WB/色彩矩阵 → 16bit sRGB)
    #
    #   为什么需要它:FFmpeg 6.1 的 tiff.c 只能吐出 CFA Bayer 帧(没有解马赛克、
    #   没有相机 WB、没有 DNG opcodes),DNG tiled 路径还是 AVERROR_PATCHWELCOME;
    #   FFmpeg 7.1 的 libraw demuxer 会违反契约 §6 的 6.1 锁定 —— 因此自己链 LibRaw。
    #
    #   源码分两个仓库(LibRaw 官方 README.cmake 说明):
    #     * LibRaw/LibRaw.git        —— 库源码(自带 CMakeLists 只在 0.20+ 的
    #                                  发行包里有,git 快照里没有,见 README.cmake)
    #     * LibRaw/LibRaw-cmake.git  —— 社区维护的 CMake 脚本,必须用
    #                                  LIBRAW_PATH 指向前者源码目录
    #   CONFIGURE 里的 -S 指向 LibRaw-cmake,-D LIBRAW_PATH 指向 LibRaw 源码。
    #
    #   线程安全(重要):
    #     app/src/main/cpp/dngzip.cpp 的 dng_decode_to_rgb48 由多 worker 并发调用
    #     (transcode.cpp:130 dng_parallel_params() 默认 nthreads = min(4, cores-1)),
    #     因此这里必须构建/安装 LibRaw-cmake 的**线程安全 target raw_r**
    #     (同一个源码、同一套编译参数,唯一差别是不加 LIBRAW_NOTHREADS 这个
    #      PRIVATE define),装出来的 libraw.a 才是线程安全版。
    #     LibRaw 0.21.4 的 src/decoders/decoders_dcraw.cpp:22-31:
    #       #ifdef LIBRAW_NOTHREADS
    #         static unsigned bitbuf = 0; ...        ← 进程级 static
    #       #else
    #       #define bitbuf tls->getbits.bitbuf       ← 每实例 LibRaw_TLS
    #       #endif
    #     单线程版把 getbithuff / ph1_bithuff / cielab(cbrt,xyz_cam) /
    #     pana_data / sony_decrypt / vng_interpolate / AAHD::gammaLUT 全部编成
    #     函数内 static,两个 worker 同时解同一帧时互踩位缓冲与查表缓存,输出每次
    #     都不一样(host 侧已用 2/4 线程探针实测 sha 每 run 必变)。
    #     LibRaw-cmake 的两个 target 产物理所当然都叫 libraw.a / libraw_r.a:
    #     target raw 加 LIBRAW_NOTHREADS → libraw.a;target raw_r 不加 → libraw_r.a。
    #     而 app/src/main/cpp/CMakeLists.txt 链接的是固定文件名 libraw.a,所以构建
    #     完 raw_r 后把它的产物复制成 libraw.a 安装(app 侧契约不变)。
    # -----------------------------------------------------------------------
    local raw_dir="${SRC}/libraw"
    local raw_cmake_dir="${SRC}/libraw-cmake"
    clone_once "${raw_dir}" "${LIBRAW_REPO}" "${LIBRAW_TAG}" "${LIBRAW_MIRROR}"
    # LibRaw-cmake 上游没有 tag,与 x264 一样只能 pin 提交 SHA(clone -b <sha>,GitHub 支持)
    clone_once "${raw_cmake_dir}" "${LIBRAW_CMAKE_REPO}" "${LIBRAW_CMAKE_SHA}" "${LIBRAW_CMAKE_MIRROR}"
    verify_pin "${raw_cmake_dir}" "${LIBRAW_CMAKE_SHA}"

    # NDK sysroot 的 zlib:llvm 布局为 <sysroot>/usr/lib/<triplet>/libz.{a,so}
    local zlib_a="${SYSROOT}/usr/lib/${triplet}/libz.a"
    local zlib_so="${SYSROOT}/usr/lib/${triplet}/libz.so"
    if [[ ! -f "${zlib_a}" && ! -f "${zlib_so}" ]]; then
        die "NDK sysroot 里找不到 libz(${zlib_a});DNG zip 解压需要 zlib"
    fi

    # -----------------------------------------------------------------------
    # 4b.1) 段A provenance(来源)门禁 —— 决定「复用」还是「强制重建」
    #   背景(REPAIR-PLAN-2026-10-06 §3.1 / AUDIT-2026-10-06 P1-01 + P2-02):
    #   修订前只看 ${prefix}/lib/libraw.a 在不在:
    #     a) 在 → 跳过本 ABI 的 LibRaw 构建。存在 ≠ 线程安全:LibRaw-cmake 的
    #        target raw(单线程,带 LIBRAW_NOTHREADS)产物也叫 libraw.a,手工把
    #        lib/ 拷进来同样能过(prebuilt/BUILDINFO 就记录过一次 manual 替换);
    #     b) 不在 → 重建。
    #   现在:per-ABI manifest 就是「这个 libraw.a 由哪次构建产生」的证明,
    #   缺证明 / 证明对不上 → 删掉该 ABI 的 libraw.a + manifest,强制回到源码
    #   重打线程安全 target raw_r。宁可多编 20 分钟,也不要一份「看起来在、
    #   其实可能是单线程」的静态库(症状见上面"线程安全(重要)":多 worker 并发
    #   解同一帧,输出每次都不一样)。一致性检查逐条列在下面,全部命中才复用。
    #
    #   行为探针(2T/4T 多轮回合同帧对比)属于真机/宿主机验收,归 HOST_TEST /
    #   F 段:本脚本不伪造这类证据,README 里写明「仍需 HOST_TEST/F 段覆盖」。
    #   判定逻辑在 libraw_provenance_ok()(本文件上面的 prf-libraw-manifest 段,
    #   tmp/ad-stub 单测直接调用它);这里只按返回值决定 skip / force-rebuild。
    # -----------------------------------------------------------------------
    local script_sha ndk_rev mfile gate_out gate_rc=0
    script_sha="$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')"
    ndk_rev="$(ndk_revision)"
    mfile="${prefix}/libraw-manifest-${abi}"

    gate_out="$(libraw_provenance_ok "${prefix}" "${abi}" "${API_LEVEL}" "${ndk_rev}" \
        "${LIBRAW_TAG}" "${LIBRAW_CMAKE_SHA}" "${script_sha}" "${NM_BIN}" "${AR_BIN}")" \
        || gate_rc=$?

    if [[ ${gate_rc} -eq 0 ]]; then
        log "[skip] ${abi} LibRaw ${LIBRAW_TAG}: provenance manifest 命中(raw_r 线程安全 target),复用 ${prefix}/lib/libraw.a"
    else
        local r first=1
        while IFS= read -r r; do
            [[ -n "${r}" ]] || continue
            if [[ ${first} -eq 1 ]]; then reason="${r}"; first=0; fi
            warn "  · ${r}"
        done <<EOF
${gate_out}
EOF
        warn "[force-rebuild] ${abi} LibRaw: ${reason}"
        warn "[force-rebuild] 删掉 ${prefix}/lib/libraw.a 与 manifest,回源码重打线程安全 raw_r —— 不复用「来源说不清」的产物"
        rm -f "${prefix}/lib/libraw.a" "${mfile}"

        local raw_build="${raw_cmake_dir}/build/${abi}"
        rm -rf "${raw_build}"
        mkdir -p "${raw_build}"
        # LibRaw-cmake 的 target 名叫 raw / raw_r(静态):raw 带 LIBRAW_NOTHREADS
        # (产物 libraw.a,单线程版),raw_r 不带(产物 libraw_r.a,线程安全版)。
        # 这里只构建线程安全的 raw_r —— 理由见上面"线程安全(重要)"注释。
        # ENABLE_JPEG 在 LibRaw-cmake 里不存在(那个脚本只有 ENABLE_JASPER 等),
        # 所以 JPEG 用 CMAKE_DISABLE_FIND_PACKAGE_JPEG 关;NDK 本身也不带
        # libjpeg/libjasper,find_package 失败时 CMake 只会警告、不会中止。
        local jpeg_disable=ON
        if [[ "${LIBRAW_JPEG}" == "ON" || "${LIBRAW_JPEG}" == "1" ]]; then
            jpeg_disable=OFF
        fi
        # 本次实际使用的构建参数(逐字写进 manifest 的 raw_r_cmake_flags)。
        # 注意这里**不出现** LIBRAW_NOTHREADS:一旦有人改了上面 cmake 参数加了
        # 它,verify-prebuilt.sh 立刻 FAIL,CMake / Gradle 门禁同样拦得住。
        local raw_r_flags
        raw_r_flags="ANDROID_ABI=${abi};ANDROID_PLATFORM=android-${API_LEVEL};ANDROID_STL=c++_static;CMAKE_BUILD_TYPE=Release;CMAKE_POSITION_INDEPENDENT_CODE=ON;BUILD_SHARED_LIBS=OFF;CMAKE_DISABLE_FIND_PACKAGE_JPEG=${jpeg_disable};CMAKE_DISABLE_FIND_PACKAGE_Jasper=ON;CMAKE_DISABLE_FIND_PACKAGE_OpenMP=ON;ENABLE_EXAMPLES=OFF;ENABLE_OPENMP=OFF;ENABLE_LCMS=OFF;ENABLE_RAWSPEED=OFF;ENABLE_JASPER=OFF;ENABLE_X3FTOOLS=OFF;ENABLE_6BY9RPI=OFF;LIBRAW_PATH=${raw_dir};target=raw_r"
        cmake -S "${raw_cmake_dir}" -B "${raw_build}" -G "Unix Makefiles" \
            -DCMAKE_TOOLCHAIN_FILE="${NDK}/build/cmake/android.toolchain.cmake" \
            -DANDROID_ABI="${abi}" \
            -DANDROID_PLATFORM="android-${API_LEVEL}" \
            -DANDROID_STL=c++_static \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="${prefix}" \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
            -DBUILD_SHARED_LIBS=OFF \
            -DCMAKE_DISABLE_FIND_PACKAGE_JPEG="${jpeg_disable}" \
            -DCMAKE_DISABLE_FIND_PACKAGE_Jasper=ON \
            -DCMAKE_DISABLE_FIND_PACKAGE_OpenMP=ON \
            -DENABLE_EXAMPLES=OFF \
            -DENABLE_OPENMP=OFF \
            -DENABLE_LCMS=OFF \
            -DENABLE_RAWSPEED=OFF \
            -DENABLE_JASPER=OFF \
            -DENABLE_X3FTOOLS=OFF \
            -DENABLE_6BY9RPI=OFF \
            -DLIBRAW_PATH="${raw_dir}" \
            > "${raw_build}/config.log" 2>&1 \
        || { tail -60 "${raw_build}/config.log"; die "LibRaw configure 失败(详见 ${raw_build}/config.log)"; }
        # 只构线程安全 target:libraw-cmake 的 `all` 会同时构 raw(单线程,无用),
        # 这里点名 raw_r。raw 与 raw_r 是同一份源码、同一套参数,唯一差别就是
        # LIBRAW_NOTHREADS 这个 define(目标文件目录 CMakeFiles/raw_r.dir,独立编译,
        # 不会与 raw 混淆也不会复用 raw 的产物)。
        cmake --build "${raw_build}" --target raw_r -j"${JOBS}" > "${raw_build}/make.log" 2>&1 \
        || { tail -60 "${raw_build}/make.log"; die "LibRaw make 失败(详见 ${raw_build}/make.log)"; }
        # install(TARGETS raw raw_r ...) 会同时装两个 target;raw 未构建时
        # cmake --install 直接报
        #   "file INSTALL cannot find ${raw_build}/libraw.a"
        # 把 raw_r 的产物复制成 raw 期望的文件名即可(同源码同参数,只是 define
        # 不同;装出来的 ${prefix}/lib/libraw.a 就是线程安全版)。
        cp -f "${raw_build}/libraw_r.a" "${raw_build}/libraw.a"
        cmake --install "${raw_build}" > "${raw_build}/install.log" 2>&1 \
        || { tail -60 "${raw_build}/install.log"; die "LibRaw install 失败(详见 ${raw_build}/install.log)"; }
        # 上面 cp 出来的这个 libraw.a 就是线程安全版(app/src/main/cpp/CMakeLists.txt
        # 固定链这个名字,契约不变);rm 掉 raw_r 的同内容副本,避免 prebuilt 里
        # 留一份没人链的重复静态库、也避免下次 install 踩到陈旧的 raw 产物。
        sha_ok="$(sha256sum "${prefix}/lib/libraw.a" | awk '{print $1}')"
        want_rawr="$(sha256sum "${raw_build}/libraw_r.a" | awk '{print $1}')"
        if [[ "${sha_ok}" != "${want_rawr}" ]]; then
            die "LibRaw 安装异常: ${prefix}/lib/libraw.a 与 raw_r 产物不一致(install 未覆盖?)"
        fi
        rm -f "${prefix}/lib/libraw_r.a" "${prefix}/lib/libraw_r.pc"
        log "[${abi}] LibRaw 已按线程安全 target(raw_r,无 LIBRAW_NOTHREADS)安装为 lib/libraw.a"
        # 装好了 + sha 校验过了,才写 manifest —— 这就是「复用」判据的唯一来源。
        write_libraw_manifest "${prefix}" "${abi}" "${API_LEVEL}" "${ndk_rev}" \
            "${LIBRAW_TAG}" "${LIBRAW_CMAKE_SHA}" "${raw_r_flags}" \
            "${script_sha}" "${NM_BIN}" "${AR_BIN}"
    fi

    # -----------------------------------------------------------------------
    # 5) 校验产物(Android 下 ffmpeg 装出来的就是 libavcodec.so,不带版本后缀)
    # -----------------------------------------------------------------------
    local missing=0
    for l in avformat avcodec avfilter avutil swscale swresample postproc; do
        if [[ ! -f "${prefix}/lib/lib${l}.so" ]]; then
            warn "缺少 lib${l}.so"
            missing=1
        fi
    done
    if [[ ${missing} -ne 0 ]]; then
        die "${abi} 产物不完整,请检查 ${ff_dir}/ffbuild-config.log"
    fi
    # 修订前 x264/x265/vpx 缺失只 warn。这个脚本的产出是 proresforge 交叉编译产物的
    # 唯一来源,四个静态库里:libraw.a 决定 DNG 能不能解,另三个决定 H.264/H.265/VP9
    # 能不能编 —— 缺任何一个继续跑只会得到一个"一半的产物"再也没人发现。
    # 改为 die(AUDIT 收尾项 147)。
    for l in x264 x265 vpx raw; do
        if [[ ! -f "${prefix}/lib/lib${l}.a" ]]; then
            die "缺少静态库 lib${l}.a: ${prefix}/lib/lib${l}.a —— 编码能力已不完整,终止构建"
        fi
    done
    if [[ ! -f "${prefix}/lib/libraw.a" ]]; then
        die "缺少 LibRaw 静态库: ${prefix}/lib/libraw.a"
    fi
    # 段A:manifest 与 libraw.a 是一对。只有 manifest 没有库 = 上一次构建死在半路;
    # 只有库没有 manifest = 来源说不清(Gradle / CMake 门禁也会拒绝消费)。
    if [[ ! -f "${prefix}/libraw-manifest-${abi}" ]]; then
        die "缺少 LibRaw provenance manifest: ${prefix}/libraw-manifest-${abi} —— 说明 libraw.a 不是本次脚本构建的产物"
    fi
    if [[ ! -f "${prefix}/include/libraw/libraw.h" ]]; then
        die "缺少 LibRaw 头文件: ${prefix}/include/libraw/libraw.h"
    fi

    log "[${abi}] 产物大小:"
    du -sh "${prefix}/lib"     | sed 's/^/    /'
    du -sh "${prefix}/include" | sed 's/^/    /'

    echo "done: ${abi}"
}

# ---------------------------------------------------------------------------
# BUILDINFO —— "这批 prebuilt 到底由什么代码/工具链构建的"(AUDIT N-4)
#   写 DEST 根一份,内容三半:
#     1) pin 的实际 commit:源码树的 HEAD(验证过与 pin 一致,但记下来供人核对)
#     2) 装出来的版本头:grep include/ 里的版本宏 —— 这是"实际证据",
#        与 pin 不一致(比如上游偷偷改了版本号、或者谁动了源码树)会在 diff 时暴露
#     3) LibRaw per-ABI provenance manifest 摘要(段A):abi / tag / ndk / tls_probe
#        —— BUILDINFO 是给人看的汇总,机器可读的判据在同目录的
#        libraw-manifest-<abi>;两者都不允许手改。
#   每次重跑脚本都会刷新;产物变化时必须连它一起更新。
# ---------------------------------------------------------------------------
write_buildinfo() {
    local out="${DEST}/BUILDINFO"
    local ndk_rev
    ndk_rev="$(sed -n 's/^Pkg.Revision *= *//p' "${NDK}/source.properties" 2>/dev/null | head -1)"
    {
        echo "BUILDINFO —— 由 scripts/build-ffmpeg-android.sh 自动生成,勿手改"
        echo "生成时间:      $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
        echo "构建机:        $(uname -srm 2>/dev/null || echo unknown)"
        echo "脚本:          scripts/build-ffmpeg-android.sh"
        echo "脚本 sha256:   $(sha256sum "${BASH_SOURCE[0]}" 2>/dev/null | awk '{print $1}')"
        echo "NDK:           ${NDK}"
        echo "NDK revision:  ${ndk_rev:-unknown}"
        echo "API level:     ${API_LEVEL}"
        echo "源码 pin(实际 HEAD):"
        echo "  FFmpeg        tag ${FFMPEG_TAG} @ $(git -C "${SRC}/ffmpeg" rev-parse HEAD 2>/dev/null || echo '-')"
        echo "  x264          ${X264_REF} @ $(git -C "${SRC}/x264" rev-parse HEAD 2>/dev/null || echo '-')"
        echo "  x265          tag ${X265_TAG} @ $(git -C "${SRC}/x265" rev-parse HEAD 2>/dev/null || echo '-')"
        echo "  libvpx        tag ${VPX_TAG} @ $(git -C "${SRC}/libvpx" rev-parse HEAD 2>/dev/null || echo '-')"
        echo "  LibRaw        tag ${LIBRAW_TAG} @ $(git -C "${SRC}/libraw" rev-parse HEAD 2>/dev/null || echo '-')"
        echo "  LibRaw-cmake  SHA ${LIBRAW_CMAKE_SHA} @ $(git -C "${SRC}/libraw-cmake" rev-parse HEAD 2>/dev/null || echo '-')"
        echo "产物实际版本头(自 include/ 读出,非 pin 期望值):"
        local a inc v1 v2 v3 x264b x265b mf mabi mtag mndk mtls mraw
        for a in "${ABIS[@]}"; do
            inc="${DEST}/${a}/include"
            v1="$(sed -n 's/^#define LIBAVCODEC_VERSION_MAJOR[[:space:]]*\([0-9]\+\).*/\1/p' "${inc}/libavcodec/version_major.h" 2>/dev/null)"
            v2="$(sed -n 's/^#define LIBAVCODEC_VERSION_MINOR[[:space:]]*\([0-9]\+\).*/\1/p' "${inc}/libavcodec/version.h" 2>/dev/null)"
            v3="$(sed -n 's/^#define LIBAVCODEC_VERSION_MICRO[[:space:]]*\([0-9]\+\).*/\1/p' "${inc}/libavcodec/version.h" 2>/dev/null)"
            x264b="$(sed -n 's/^#define X264_BUILD[[:space:]]*\([0-9]\+\).*/\1/p' "${inc}/x264.h" 2>/dev/null)"
            x265b="$(sed -n 's/^#define X265_BUILD[[:space:]]*\([0-9]\+\).*/\1/p' "${inc}/x265_config.h" 2>/dev/null)"
            echo "  ${a}: libavcodec ${v1:-?}.${v2:-?}.${v3:-?} | X264_BUILD ${x264b:-?} | X265_BUILD ${x265b:-?}"
        done
        echo "LibRaw provenance manifest(段A,机器可读判据在 ${DEST}/<abi>/libraw-manifest-<abi>):"
        for a in "${ABIS[@]}"; do
            mf="${DEST}/${a}/libraw-manifest-${a}"
            if [[ -f "${mf}" ]]; then
                mabi="$(mget "${mf}" abi)"; mtag="$(mget "${mf}" libraw_tag)"
                mndk="$(mget "${mf}" ndk_revision)"; mtls="$(mget "${mf}" tls_probe)"
                mraw="$(mget "${mf}" libraw_sha256)"
                echo "  ${a}: abi=${mabi:-?} tag=${mtag:-?} ndk=${mndk:-?} tls_probe=${mtls:-?} libraw_a_sha256=${mraw:-?}"
            else
                echo "  ${a}: 缺 manifest —— 该 ABI 的 libraw.a 不能证明是线程安全 raw_r 构建"
            fi
        done
    } > "${out}"
    log "BUILDINFO 已写入 ${out}"
}

for abi in "${ABIS[@]}"; do
    build_abi "${abi}"
done

write_buildinfo

log "全部完成,产物目录: ${DEST}/<abi>/{include,lib}"
echo "提醒: ${DEST} 是构建产物,不要提交进 git(见 app/src/main/cpp/prebuilt/README.md)。"
# 段A:一次独立自检 —— manifest / sha / pin / NDK / 脚本一致,才发现把库换掉。
#   行为探针(2T/4T 多轮回合)不在这里,归 HOST_TEST / F 段(见 README)。
for a in "${ABIS[@]}"; do
    bash "${SCRIPT_DIR}/verify-prebuilt.sh" "${DEST}/${a}" --abi "${a}" --ndk-bin "${TC}/bin" \
        || die "自检失败:${a} 的 LibRaw provenance 校验未通过(见上面的明细)"
done
