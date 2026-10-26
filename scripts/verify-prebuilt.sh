#!/usr/bin/env bash
# verify-prebuilt.sh —— prebuilt LibRaw 产物「来源(provenance)」校验器
#
# 为什么需要它(2026-10-06 审计后的加固):
#   prebuilt/<abi>/lib/libraw.a 此前只有「存在性」检查,于是两种坏情况都能静默通过:
#     * 单线程版(LibRaw-cmake target raw,带 LIBRAW_NOTHREADS)被当成 libraw.a 装上;
#     * 有人把 lib/ 整个目录手工拷进来(prebuilt/BUILDINFO 里就记录过一次
#       "manual 替换",没有任何脚本/pin/NDK 记录能证明它是线程安全版)。
#   现在每个 ABI 目录下固定有一个 per-ABI manifest
#       prebuilt/<abi>/libraw-manifest-<abi>   (key=value 文本)
#   由 scripts/build-ffmpeg-android.sh 在成功构建线程安全 target raw_r 并安装后
#   生成。本脚本逐字段复算该校验,任何一处不符 → 退出码 1。
#   manifest 字段(16 个必填 + 2 行 # 注释):
#     abi / api_level / ndk_revision / libraw_tag / libraw_cmake_sha /
#     raw_r_target=raw_r / libraw_nothreads=absent / raw_r_cmake_flags /
#     threads_lib / script_sha256 / libraw_sha256 / symbols_sha256 /
#     members_sha256 / tls_probe(present|absent,信息性)/ build_time_utc / manual=false
#   字段含义与生成方式见 app/src/main/cpp/prebuilt/README.md。
#
# 行为探针(2T/4T 多轮回合同帧解码,输出必须逐字节一致)不在这里做:那是真机/
#   HOST_TEST 验收项,归 F 段。本脚本只做 provenance / 内容级校验。
#
# 用法:
#   verify-prebuilt.sh <prefix-dir> [--ndk-bin <dir>] [--no-symbols] [--print-manifest]
#
#   <prefix-dir>      prebuilt 下的 ABI 目录(含 lib/libraw.a 与
#                     libraw-manifest-<abi>);不给 <ABI> 参数时用目录名推断。
#   --abi <abi>       显式指定 ABI(目录名不是标准 ABI 名时必须)。
#   --ndk-bin <dir>   llvm-nm / llvm-ar 所在目录。也可以传 NDK 根目录或
#                     toolchains/llvm/prebuilt/<host>,脚本会向下探测 bin/。
#                     默认依次探测:环境变量 NDK/ANDROID_NDK_HOME/ANDROID_NDK_ROOT/
#                     NDK_HOME、$HOME/android-ndk-*、NDK 默认安装路径。
#   --no-symbols      跳过 symbols_sha256 / members_sha256 的复算
#                     (校验符号需要 llvm-nm / llvm-ar;找不到工具时默认直接 FAIL,
#                      因为"能复算而不复算"等于把门禁退化成存在性检查)。
#   --print-manifest  校验前先把 manifest 原文打印出来(bash 'source' 友好格式)。
#
# 期望值覆盖(都比较"manifest 记录值"与"调用方给出的期望值"):
#   EXPECT_ABI EXPECT_API_LEVEL EXPECT_NDK_REVISION EXPECT_LIBRAW_TAG
#   EXPECT_LIBRAW_CMAKE_SHA EXPECT_SCRIPT_SHA256 EXPECT_SCRIPT
#   默认从本仓库 scripts/build-ffmpeg-android.sh 里 READ 当前 pin 与脚本自身 sha,
#   拿不到就只校验"manifest 自身自洽 + libraw.a 的真实 sha"。
#
# 退出码:0 = 全部通过(PASS);1 = 有任一项 FAIL。
set -uo pipefail

# ---------------------------------------------------------------------------
# 小工具
# ---------------------------------------------------------------------------
ts()   { date -u '+%Y-%m-%dT%H:%M:%SZ'; }
log()  { printf '%s\n' "$*"; }
warn() { printf '\033[1;33m[warn] %s\033[0m\n' "$*" >&2; }

PASS_N=0
FAIL_N=0
CHECK_N=0
pass() { printf '  \033[1;32mPASS\033[0m  %s\n' "$*"; PASS_N=$((PASS_N+1)); CHECK_N=$((CHECK_N+1)); }
fail() { printf '  \033[1;31mFAIL\033[0m  %s\n' "$*"; FAIL_N=$((FAIL_N+1)); CHECK_N=$((CHECK_N+1)); }
info() { printf '  \033[1;36mnote\033[0m  %s\n' "$*"; }

die() { printf '\033[1;31m[error] %s\033[0m\n' "$*" >&2; exit 2; }

sha256_file() {
    local f="$1" out=""
    [[ -f "${f}" ]] || return 1
    if command -v sha256sum >/dev/null 2>&1; then
        out="$(sha256sum "${f}" | awk '{print $1}')"
    elif command -v shasum >/dev/null 2>&1; then
        out="$(shasum -a 256 "${f}" | awk '{print $1}')"
    else
        return 1
    fi
    [[ -n "${out}" ]] || return 1
    printf '%s\n' "${out}"
}

# llvm-nm --defined-only 的全量 defined symbol:"<addr> <type> <mangled>" 的第 3 列,
# 排序去重后 sha256。定义必须与 build-ffmpeg-android.sh 里 nm_symbols_sha() 一致,
# 否则"复算一致"这句话没有意义(两边用同一套流水线)。
normalize_symbols_sha() {
    local nm="$1" lib="$2"
    "${nm}" --defined-only "${lib}" 2>/dev/null \
        | awk 'NF>=3 {print $3}' | LC_ALL=C sort -u | sha256sum | awk '{print $1}'
}

# llvm-ar t 的成员清单排序去重后 sha256(脚本改动/混入 raw 单线程副本会改变它)。
normalize_members_sha() {
    local ar="$1" lib="$2"
    "${ar}" t "${lib}" 2>/dev/null | LC_ALL=C sort -u | sha256sum | awk '{print $1}'
}

# ---------------------------------------------------------------------------
# 参数解析
# ---------------------------------------------------------------------------
PREFIX=""
ABI_IN=""
NDK_BIN_IN=""
DO_SYMBOLS=1
PRINT_MANIFEST=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --abi)          ABI_IN="$2"; shift 2 ;;
        --ndk-bin)      NDK_BIN_IN="$2"; shift 2 ;;
        --no-symbols)   DO_SYMBOLS=0; shift ;;
        --print-manifest) PRINT_MANIFEST=1; shift ;;
        -h|--help)
            sed -n '2,32p' "$0"; exit 0 ;;
        -*)             die "未知参数: $1(用 --help 看用法)" ;;
        *)
            [[ -z "${PREFIX}" ]] || die "只能给一个 <prefix-dir>(已给 ${PREFIX})"
            PREFIX="$1"; shift ;;
    esac
done
[[ -n "${PREFIX}" ]] || die "缺少 <prefix-dir>;用法: $0 <prefix-dir> [--abi <abi>] [--ndk-bin <dir>]"

PREFIX="${PREFIX%/}"
[[ -d "${PREFIX}" ]] || die "目录不存在: ${PREFIX}"
[[ -f "${PREFIX}/lib/libraw.a" ]] || die "${PREFIX}/lib/libraw.a 不存在(连 manifest 都不用验了)"

ABI="${ABI_IN:-$(basename "${PREFIX}")}"
case "${ABI}" in
    arm64-v8a|armeabi-v7a|x86_64|x86) ;;
    *) warn "ABI 名不常见: ${ABI}(用 --abi 显式指定)" ;;
esac

MANIFEST="${PREFIX}/libraw-manifest-${ABI}"
RAW_A="${PREFIX}/lib/libraw.a"
RAW_H="${PREFIX}/include/libraw/libraw.h"

# ---------------------------------------------------------------------------
# 脚本/pin 的期望值(优先环境变量覆盖,否则从仓库脚本里读当前 pin)
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_SCRIPT="${EXPECT_SCRIPT:-${SCRIPT_DIR}/build-ffmpeg-android.sh}"

if [[ ! -f "${BUILD_SCRIPT}" ]]; then
    for c in "${ROOT:-/nonexistent}/scripts/build-ffmpeg-android.sh" \
             "${SCRIPT_DIR}/../build-ffmpeg-android.sh" \
             "$(pwd)/scripts/build-ffmpeg-android.sh"; do
        [[ -f "${c}" ]] && BUILD_SCRIPT="${c}" && break
    done
fi

read_pin() {   # <var-name> —— 从 build 脚本读 "VAR="${VAR:-value}"" 形式的默认 pin
    local var="$1"
    [[ -f "${BUILD_SCRIPT}" ]] || return 1
    sed -n "s/^${var}=\"\${${var}:-\([^}\"]*\)}\".*/\1/p" "${BUILD_SCRIPT}" | head -1
}

EXP_API="${EXPECT_API_LEVEL:-}"
EXP_NDK="${EXPECT_NDK_REVISION:-}"
EXP_TAG="${EXPECT_LIBRAW_TAG:-$(read_pin LIBRAW_TAG || true)}"
EXP_CSHA="${EXPECT_LIBRAW_CMAKE_SHA:-$(read_pin LIBRAW_CMAKE_SHA || true)}"
EXP_SSHA="${EXPECT_SCRIPT_SHA256:-}"
if [[ -z "${EXP_SSHA}" && -f "${BUILD_SCRIPT}" ]]; then
    EXP_SSHA="$(sha256_file "${BUILD_SCRIPT}" || true)"
fi

# NDK 期望 revision:Pkg.Revision 读得到才算数(否则只校验 manifest 非空)
if [[ -z "${EXP_NDK}" ]]; then
    ndk_candidates=()
    for c in "${NDK:-}" "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" "${NDK_HOME:-}" \
             "${HOME}"/android-ndk-* /usr/local/android-sdk/ndk/* \
             "${HOME}/AppData/Local/Android/Sdk/ndk"/*; do
        [[ -n "${c}" && -d "${c}" ]] && ndk_candidates+=("${c}")
    done
    if [[ ${#ndk_candidates[@]} -gt 0 ]]; then
        for c in "${ndk_candidates[@]}"; do
            [[ -n "${c}" && -f "${c}/source.properties" ]] || continue
            r="$(sed -n 's/^Pkg.Revision *= *//p' "${c}/source.properties" | head -1)"
            [[ -n "${r}" ]] && EXP_NDK="${r}" && break
        done
    fi
fi

# ---------------------------------------------------------------------------
# 探测 llvm-nm / llvm-ar
# ---------------------------------------------------------------------------
find_tool() {   # <exe-name> —— 在常见 NDK 布局里找 llvm-archiver/nm
    local exe="$1" c sub p
    for c in "${NDK_BIN_IN:-}" "${NDK:-}" "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" \
             "${NDK_HOME:-}" "${HOME}"/android-ndk-* \
             /usr/local/android-sdk/ndk/* \
             "${HOME}/AppData/Local/Android/Sdk/ndk"/*; do
        [[ -n "${c}" && -d "${c}" ]] || continue
        for sub in "" "bin" \
                   "toolchains/llvm/prebuilt/windows-x86_64/bin" \
                   "toolchains/llvm/prebuilt/linux-x86_64/bin" \
                   "toolchains/llvm/prebuilt/darwin-x86_64/bin"; do
            for ext in "" ".exe"; do
                p="${c}/${sub}/${exe}${ext}"
                [[ "${sub}" == "" ]] && p="${c}/${exe}${ext}"
                if [[ -x "${p}" ]]; then printf '%s\n' "${p}"; return 0; fi
            done
        done
    done
    return 1
}

# ---------------------------------------------------------------------------
# 校验
# ---------------------------------------------------------------------------
log ""
log "======================================================================"
log "verify-prebuilt: ${PREFIX}"
log "  ABI       = ${ABI}"
log "  manifest  = ${MANIFEST}"
log "  libraw.a  = ${RAW_A}"
log "  校验时间  = $(ts)"
log "======================================================================"

# 1) manifest 必须存在
if [[ ! -f "${MANIFEST}" ]]; then
    fail "manifest 不存在: ${MANIFEST}"
    log ""
    log "说明:per-ABI manifest 只能由 scripts/build-ffmpeg-android.sh 在成功构建"
    log "      线程安全 target raw_r 后写入。缺 manifest = 无法证明 libraw.a 是"
    log "      raw_r(不带 LIBRAW_NOTHREADS)构建的 —— 必须重跑:"
    log "        NDK=/path/to/android-ndk ./scripts/build-ffmpeg-android.sh"
    log ""
    log "结果: FAIL(0 项通过 / 1 项失败)"
    exit 1
fi
pass "manifest 存在: ${MANIFEST}"

if [[ ${PRINT_MANIFEST} -eq 1 ]]; then
    info "manifest 原文(source 友好的 key=value):"
    while IFS= read -r l; do printf '    %s\n' "${l}"; done < "${MANIFEST}"
fi

# 2) parse(必须是 key=value;大小写键、无注释、无重复 key)
if [[ "${BASH_VERSINFO[0]:-3}" -ge 4 ]]; then
    HAVE_ASSOC=1; declare -A SEEN
else
    HAVE_ASSOC=0; SEEN_LIST=""
fi
PARSE_BAD=0
while IFS= read -r line || [[ -n "${line}" ]]; do
    [[ -z "${line}" ]] && continue
    [[ "${line:0:1}" == "#" ]] && continue      # 允许注释行(给人看的,机器跳过)
    if [[ ! "${line}" =~ ^[A-Za-z0-9_]+= ]]; then
        fail "manifest 行不是 key=value: ${line}"; PARSE_BAD=1; continue
    fi
    k="${line%%=*}"
    if [[ ${HAVE_ASSOC} -eq 1 ]]; then
        [[ -n "${SEEN[${k}]:-}" ]] && { fail "manifest 重复键: ${k}"; PARSE_BAD=1; }
        SEEN["${k}"]=1
    else
        case " ${SEEN_LIST} " in *" ${k} "*) fail "manifest 重复键: ${k}"; PARSE_BAD=1 ;; esac
        SEEN_LIST="${SEEN_LIST} ${k}"
    fi
done < "${MANIFEST}"
[[ ${PARSE_BAD} -eq 0 ]] && pass "manifest 可解析(key=value,无重复键)"

mval() {  # <key> —— 取 manifest 值(不存在或同一键重复出现时返回空)
    local key="$1" out="" n=0
    while IFS= read -r line || [[ -n "${line}" ]]; do
        [[ -z "${line}" ]] && continue
        [[ "${line:0:1}" == "#" ]] && continue
        [[ "${line%%=*}" == "${key}" ]] || continue
        n=$((n+1))
        out="${line#*=}"
    done < "${MANIFEST}"
    [[ ${n} -eq 1 ]] || return 1
    printf '%s\n' "${out}"
}

M_ABI="$(mval abi || true)";                 M_API="$(mval api_level || true)"
M_NDK="$(mval ndk_revision || true)";        M_TAG="$(mval libraw_tag || true)"
M_CSHA="$(mval libraw_cmake_sha || true)";   M_TGT="$(mval raw_r_target || true)"
M_NOTHREADS="$(mval libraw_nothreads || true)"
M_FLAGS="$(mval raw_r_cmake_flags || true)"; M_TLS="$(mval threads_lib || true)"
M_SCRIPT="$(mval script_sha256 || true)";    M_RAW="$(mval libraw_sha256 || true)"
M_SYMS="$(mval symbols_sha256 || true)";     M_MEMBERS="$(mval members_sha256 || true)"
M_TLSP="$(mval tls_probe || true)"
M_TIME="$(mval build_time_utc || true)";     M_MANUAL="$(mval manual || true)"

# 3) required 字段非空(一个都不能空:空字段 = 上游写 manifest 的逻辑漏了一项)
info "字段:"
MISSING=""
for kv in "abi" "api_level" "ndk_revision" "libraw_tag" "libraw_cmake_sha" \
          "raw_r_target" "libraw_nothreads" "raw_r_cmake_flags" "threads_lib" \
          "script_sha256" "libraw_sha256" "symbols_sha256" "members_sha256" \
          "tls_probe" "build_time_utc" "manual"; do
    case "${kv}" in
        abi)              v="${M_ABI}" ;;        api_level)        v="${M_API}" ;;
        ndk_revision)     v="${M_NDK}" ;;        libraw_tag)       v="${M_TAG}" ;;
        libraw_cmake_sha) v="${M_CSHA}" ;;       raw_r_target)     v="${M_TGT}" ;;
        libraw_nothreads) v="${M_NOTHREADS}" ;;  raw_r_cmake_flags) v="${M_FLAGS}" ;;
        threads_lib)      v="${M_TLS}" ;;        script_sha256)    v="${M_SCRIPT}" ;;
        libraw_sha256)    v="${M_RAW}" ;;        symbols_sha256)   v="${M_SYMS}" ;;
        members_sha256)   v="${M_MEMBERS}" ;;    tls_probe)        v="${M_TLSP}" ;;
        build_time_utc)   v="${M_TIME}" ;;       manual)           v="${M_MANUAL}" ;;
    esac
    if [[ -z "${v}" ]]; then
        MISSING="${MISSING} ${kv}"
    else
        printf '    %-18s = %s\n' "${kv}" "${v}"
    fi
done
if [[ -n "${MISSING}" ]]; then
    fail "manifest 缺字段(必须由脚本生成,不能手写):${MISSING}"
else
    pass "manifest 16 个必填字段全部非空"
fi

# 3b) tls_probe —— 线程安全的旁证(信息性)
#     present:libraw.a 里能见到 LibRaw_TLS 符号(线程安全编译单元引用 per-instance
#              线程状态,见 LibRaw 0.21.4 src/decoders/decoders_dcraw.cpp:22-31);
#     absent :符号里没有 LibRaw_TLS。这**不是**硬 FAIL —— 真凭据是上面
#              manual/libraw_nothreads/raw_r_target/flags/sha 几项合一;LibRaw 升级
#              可能改变 TLS 的符号呈现方式。absent 只提醒人工复核 + HOST_TEST 行为探针。
if [[ "${M_TLSP}" == "present" ]]; then
    pass "tls_probe=present(libraw.a 可见 LibRaw_TLS 符号)"
elif [[ "${M_TLSP}" == "absent" ]]; then
    warn "tls_probe=absent—— libraw.a 里没有 LibRaw_TLS 符号。
        这不是硬 FAIL(判据是上面的 manual/nothreads/target/flags/sha 四项合一),
        但请复核本次 raw_r 构建,并务必跑 HOST_TEST/F 段行为探针
        (2T/4T 多轮回合同帧解码输出 sha 必须一致)。"
else
    fail "tls_probe='${M_TLSP}' 不是 present/absent"
fi

# 4) manual 必须 false —— BUILDINFO 不再允许"手工替换"
if [[ "${M_MANUAL}" == "false" ]]; then
    pass "manual=false(脚本构建,非手工替换)"
elif [[ "${M_MANUAL}" == "true" ]]; then
    fail "manual=true —— BUILDINFO 不再允许手工替换,必须重跑 scripts/build-ffmpeg-android.sh"
else
    fail "manual='${M_MANUAL}' 不是 false(缺省视为不可信)"
fi

# 5) 单线程版特征必须缺失
if [[ "${M_NOTHREADS}" == "absent" ]]; then
    pass "libraw_nothreads=absent(构建时未定义 LIBRAW_NOTHREADS)"
else
    fail "libraw_nothreads='${M_NOTHREADS}' != absent —— 这是单线程 raw target 的特征"
fi

# 6) raw_r_target 必须是 raw_r
if [[ "${M_TGT}" == "raw_r" ]]; then
    pass "raw_r_target=raw_r(线程安全 target)"
else
    fail "raw_r_target='${M_TGT}' != raw_r"
fi

# 7) 实际使用的 flags 里不得出现 LIBRAW_NOTHREADS(双保险:即使字段写错也能抓到)
if [[ -n "${M_FLAGS}" ]]; then
    case "${M_FLAGS}" in
        *LIBRAW_NOTHREADS*) fail "raw_r_cmake_flags 里出现 LIBRAW_NOTHREADS: ${M_FLAGS}" ;;
        *)                  pass "raw_r_cmake_flags 不含 LIBRAW_NOTHREADS" ;;
    esac
else
    fail "raw_r_cmake_flags 为空"
fi

# 8) libraw.a 的存在与真实 sha256
if [[ ! -f "${RAW_A}" ]]; then
    fail "libraw.a 不存在: ${RAW_A}"
else
    pass "libraw.a 存在: ${RAW_A} ($(du -h "${RAW_A}" 2>/dev/null | awk '{print $1}'))"
    REAL_RAW="$(sha256_file "${RAW_A}" || true)"
    if [[ -n "${REAL_RAW}" && "${REAL_RAW}" == "${M_RAW}" ]]; then
        pass "libraw_sha256 与实际 lib/libraw.a 一致 (${REAL_RAW})"
    else
        fail "libraw_sha256 不符: manifest=${M_RAW:-(空)} 实际=${REAL_RAW:-(算不出)}"
    fi
fi

if [[ ! -f "${RAW_H}" ]]; then
    fail "LibRaw 头文件缺失: ${RAW_H}(库与头不同源,dngzip.cpp 编不过)"
fi

# 9) ABI / pin / 脚本 sha 与调用方期望一致(期望值缺省只做非空校验)
expect_cmp() {   # <label> <manifest-value> <expected>
    local label="$1" got="$2" want="$3"
    if [[ -z "${got}" ]]; then fail "${label}: manifest 值为空"; return; fi
    if [[ -z "${want}" ]]; then
        pass "${label} = ${got}(未提供期望值,只校验非空)"
    elif [[ "${got}" == "${want}" ]]; then
        pass "${label} = ${got}"
    else
        fail "${label} 不符: manifest=${got} 期望=${want}"
    fi
}
expect_cmp "abi"              "${M_ABI}"    "${EXPECT_ABI:-${ABI}}"
expect_cmp "api_level"        "${M_API}"    "${EXP_API}"
expect_cmp "ndk_revision"     "${M_NDK}"    "${EXP_NDK}"
expect_cmp "libraw_tag"       "${M_TAG}"    "${EXP_TAG}"
expect_cmp "libraw_cmake_sha" "${M_CSHA}"   "${EXP_CSHA}"
expect_cmp "script_sha256"    "${M_SCRIPT}" "${EXP_SSHA}"

# 10) 符号 / 成员 sha 复算(证明"这份 .a"就是当年那份:内容一动就变)
NM_BIN="$(find_tool llvm-nm || true)"
AR_BIN="$(find_tool llvm-ar || true)"
if [[ ${DO_SYMBOLS} -eq 0 ]]; then
    info "按 --no-symbols 要求跳过符号/成员复算(门禁降级为存在性+sha 校验)"
elif [[ -z "${NM_BIN}" || -z "${AR_BIN}" ]]; then
    fail "找不到 llvm-nm/llvm-ar(NDK bin),无法复算 symbols_sha256/members_sha256。
       用 --ndk-bin <NDK 工具链 bin 目录> 指定;确认要降级再加 --no-symbols。"
else
    info "探针: nm=${NM_BIN}"
    info "探针: ar=${AR_BIN}"
    REAL_SYMS="$(normalize_symbols_sha "${NM_BIN}" "${RAW_A}" || true)"
    REAL_MEMBERS="$(normalize_members_sha "${AR_BIN}" "${RAW_A}" || true)"
    if [[ -n "${REAL_SYMS}" && "${REAL_SYMS}" == "${M_SYMS}" ]]; then
        pass "symbols_sha256 复算一致 (${REAL_SYMS})"
    else
        fail "symbols_sha256 复算不符: manifest=${M_SYMS:-(空)} 计算=${REAL_SYMS:-(空)}
       说明:存档里的 defined symbol 与构建时不一致(源码/编译参数/工具链变了,或人工替换过文件)"
    fi
    if [[ -n "${REAL_MEMBERS}" && "${REAL_MEMBERS}" == "${M_MEMBERS}" ]]; then
        pass "members_sha256 复算一致 (${REAL_MEMBERS})"
    else
        fail "members_sha256 复算不符: manifest=${M_MEMBERS:-(空)} 计算=${REAL_MEMBERS:-(空)}"
    fi
fi

# 11) 汇总
log ""
log "----------------------------------------------------------------------"
log "结果: $([[ ${FAIL_N} -eq 0 ]] && echo 'PASS' || echo 'FAIL')  " \
    "(${PASS_N} 通过 / ${FAIL_N} 失败 / 共 ${CHECK_N} 项)"
log "时间: $(ts)"
log "----------------------------------------------------------------------"

# 行为探针(多线程同一帧解码,输出必须逐字节一致)不在这里做 —— 那是真机/HOST_TEST
# 验收项:2T/4T 多轮回合同帧解码 sha 一致(单线程版混入时该探针必失败)。
if [[ ${FAIL_N} -eq 0 ]]; then
    log "后续仍需覆盖:HOST_TEST/F 段的行为探针(2T/4T 多轮回合同帧解码 sha 一致)。"
    exit 0
fi
exit 1