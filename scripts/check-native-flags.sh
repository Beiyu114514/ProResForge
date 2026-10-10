#!/usr/bin/env bash
# check-native-flags.sh —— 段D 验收:扫 compile_commands.json,确认 Release 编译
# 命令的优化级别是 -O3(且带 -DNDEBUG),并且没有 -O0/-O1/-O2,也没有 -ffast-math。
#
# 背景:本仓库此前出过 Debug/-O0 产物(4→3 通道循环 -O0 36.1ms vs -O3 5.6ms),
# 而 CMake 里旧的 `if(CMAKE_CXX_FLAGS_RELEASE MATCHES "-O")` 会把外部塞进来的
# -O0 当成"已优化"放行(P2-01)。compile_commands.json 是最终真正喂给编译器的
# 命令行,扫它比扫 CMake 变量更接近事实。
#
# 用法:
#   check-native-flags.sh [compile_commands.json 路径] [选项]
#     --expect-release   只检查条目里含 "Release" 的编译命令(默认)
#     --all              全量检查(不按 Release 过滤)
#     --help             显示本帮助
#
# 判定(每条被检查的命令):
#   1) 必须同时含 -O3 与 -DNDEBUG
#   2) 不得含 -O0 / -O1 / -O2(也不含 -Os / -Oz / -Ofast)
#   3) 不得含 -ffast-math
# 退出码:0=全部通过;1=存在违规;2=用法/输入错误。
#
# 注意:本脚本只看 C/C++ 编译命令;链接命令(输出 .so/.a)自动跳过。
# 输出是逐条人类可读明细,FAIL 时给出 offending 文件与命令片段。

set -u

LC_ALL=C
export LC_ALL

usage() {
    sed -n '2,23p' "$0"
    exit 0
}

MODE="release"          # release(只查 Release 条目)| all(全量)
CCJSON="./compile_commands.json"
declare -a POSITIONAL=()

for _a in "$@"; do
    case "${_a}" in
        --help|-h)        usage ;;
        --expect-release) MODE="release" ;;
        --all)            MODE="all" ;;
        -*)               echo "FAIL 未知参数: ${_a}(用 --help 看用法)" >&2; exit 2 ;;
        *)                POSITIONAL+=("${_a}") ;;
    esac
done

if [[ ${#POSITIONAL[@]} -gt 1 ]]; then
    echo "FAIL 只接受一个 compile_commands.json 路径: ${POSITIONAL[*]}" >&2
    exit 2
fi
if [[ ${#POSITIONAL[@]} -eq 1 ]]; then
    CCJSON="${POSITIONAL[0]}"
fi

if [[ ! -f "${CCJSON}" ]]; then
    echo "FAIL compile_commands.json 不存在: ${CCJSON}"
    echo "     先生成它(Gradle 的 externalNativeBuild 会在"
    echo "     app/.cxx/<variant>/<abi>/compile_commands.json 落一份;"
    echo "     手工 cmake 可加 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)。"
    exit 2
fi

# 解析 JSON:只用 grep/sed,不依赖 python/jq(本机 Git Bash 的 python3 是
# Microsoft Store 占位符,exit 49 且无输出)。CMake 生成的 compile_commands.json
# 是扁平数组,每项的 "file"/"command" 都是单行字符串,用 ERE 抠出来再反转义即可。
#   "command"\s*:\s*"(\\.|[^"\\])*"   —— 兼容转义引号/反斜杠
json_values() {  # json_values <key> → 每行一个已反转义的值
    grep -oE "\"$1\"[[:space:]]*:[[:space:]]*\"(\\\\.|[^\"\\\\])*\"" "${CCJSON}" \
        | sed -e 's/^"'$1'"[[:space:]]*:[[:space:]]*"//' -e 's/"$//' \
        | sed -e 's/\\"/"/g' -e 's/\\\\/\\/g' -e 's/\\\//\//g'
}

CMDS_FILE="$(json_values command)"
# grep 在没有匹配时返回 1,但 MAP 为空下面会报错退出,所以这里不 set -e 也好
FILES_FILE="$(json_values file)"
N_CMDS="$(printf '%s\n' "${CMDS_FILE}" | grep -c . || true)"

if [[ -z "${CMDS_FILE}" || "${N_CMDS}" -eq 0 ]]; then
    echo "FAIL 没能从 ${CCJSON} 解析出任何 \"command\" 条目"
    echo "     确认它是 CMake 生成的 compile_commands.json(-DCMAKE_EXPORT_COMPILE_COMMANDS=ON)。"
    exit 2
fi

TOTAL=0
CHECKED=0
SKIPPED=0
UNDECIDED=0
FAILS=0
declare -a BAD_LINES=()

mapfile -t CMDS <<< "${CMDS_FILE}"
mapfile -t FILES <<< "${FILES_FILE}"

echo "== check-native-flags: ${CCJSON} (mode=${MODE}, ${#CMDS[@]} 条 command) =="

for idx in "${!CMDS[@]}"; do
    cmd="${CMDS[$idx]}"
    src="${FILES[$idx]:-<unknown>}"
    [[ -n "${cmd}" ]] || continue
    TOTAL=$((TOTAL+1))
    # 判定是不是「编译命令」:含 -c 且不以 .so/.a 为输出
    if [[ "${cmd}" != *" -c "* ]]; then
        SKIPPED=$((SKIPPED+1)); continue
    fi
    if [[ "${cmd}" == *"-o "*".so"* || "${cmd}" == *"-o "*".a"* ]]; then
        SKIPPED=$((SKIPPED+1)); continue
    fi

    # Release 判定:命令里有 -DNDEBUG / CMAKE_BUILD_TYPE=Release 即视为 Release;
    # 认不出来时按 mode 决定(release 模式跳过并计数,all 模式一律严查)
    is_release=0
    if [[ "${cmd}" == *"-DNDEBUG"* || "${cmd}" == *"CMAKE_BUILD_TYPE=Release"* ]]; then
        is_release=1
    fi

    if [[ "${MODE}" == "release" && ${is_release} -eq 0 ]]; then
        # 看起来是 Debug/-g 条目:release 模式下不判,单独计数提示
        if [[ "${cmd}" == *"-O0"* || "${cmd}" == *" -g"* || "${cmd}" == *"-O1"* \
              || "${cmd}" == *"-O2"* || "${cmd}" == *"Debug"* ]]; then
            UNDECIDED=$((UNDECIDED+1))
            echo "-- [跳过] ${src}"
            echo "   认不出是 Release 条目(命令里没有 -DNDEBUG / CMAKE_BUILD_TYPE=Release),已跳过"
            echo "   如想连它一起严查,加 --all"
            continue
        fi
    fi
    CHECKED=$((CHECKED+1))

    bad_list=()
    if [[ "${cmd}" != *"-O3"* ]]; then bad_list+=("缺 -O3"); fi
    if [[ "${cmd}" != *"-DNDEBUG"* ]]; then bad_list+=("缺 -DNDEBUG"); fi
    for bad in -O0 -O1 -O2 -Os -Oz -Ofast; do
        if [[ "${cmd}" == *"${bad}"* ]]; then bad_list+=("含 ${bad}"); fi
    done
    if [[ "${cmd}" == *"-ffast-math"* ]]; then bad_list+=("含 -ffast-math"); fi

    if [[ ${#bad_list[@]} -gt 0 ]]; then
        FAILS=$((FAILS+1))
        BAD_LINES+=("${src}|$(IFS=,; echo "${bad_list[*]}")|${cmd:0:200}")
    fi
done

echo
echo "-- 条目总数 ${TOTAL},编译命令 ${CHECKED},跳过(链接/非编译)${SKIPPED},无法判定 Release ${UNDECIDED}"
if [[ ${FAILS} -gt 0 ]]; then
    echo
    echo "FAIL: ${FAILS} 条编译命令不符合 Release 优化要求(-O3 + -DNDEBUG,无 -O0/-O1/-O2/-ffast-math):"
    for b in "${BAD_LINES[@]}"; do
        IFS='|' read -r bfile breason bcmd <<< "${b}"
        echo "  × ${bfile}"
        echo "    原因: ${breason}"
        echo "    命令片段: ${bcmd}"
    done
    echo
    echo "     排查:app/src/main/cpp/CMakeLists.txt 的 Release flags 段(段D)已强制 -O3;"
    echo "     若这里仍看到 -O0/-O1/-O2,说明有外部来源覆盖了 flags:"
    echo "       · app/build.gradle.kts externalNativeBuild 的 cFlags/cppFlags/arguments"
    echo "       · CMakeCache.txt 里的 CMAKE_CXX_FLAGS_RELEASE 残留(需要清 .cxx 缓存重配)"
    exit 1
fi

echo "PASS: 全部 ${CHECKED} 条编译命令都是 -O3 + -DNDEBUG,且不含 -O0/-O1/-O2/-Os/-Oz/-Ofast/-ffast-math"
exit 0
