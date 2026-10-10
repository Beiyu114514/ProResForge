#!/usr/bin/env bash
# check-contracts.sh —— 契约一致性自检(P2-05 收尾:「在 CI 中加入简单一致性脚本」)。
#
# 一次性把「代码才是契约、文档是投影」这个约束变成可执行检查,防止文档再次漂移:
#   CMakeLists 的源文件清单 / JNI 入口数 / FFmpeg runtime 库清单 /
#   spec 键数 / ProRes profile 写法,都必须与 docs/INTERFACES.md 的说法一致。
#
# 用法(仓库根目录):bash scripts/check-contracts.sh
# 退出码:0 = 全部一致;1 = 有任一不一致(逐项打印 [FAIL])。

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

fail=0
ok()   { echo "[ok]   $*"; }
bad()  { echo "[FAIL] $*"; fail=1; }

nativelib="app/src/main/cpp/native-lib.cpp"
cmake="app/src/main/cpp/CMakeLists.txt"
iface="docs/INTERFACES.md"
gradle="app/build.gradle.kts"
strspec="app/src/main/cpp/strspec.h"
proguard="app/proguard-rules.pro"

for f in "$nativelib" "$cmake" "$iface" "$gradle" "$strspec" "$proguard"; do
    [[ -f "$f" ]] || { bad "找不到文件: $f"; exit 1; }
done

# ---------------------------------------------------------------------------
# 1) JNI 入口:native-lib.cpp 的定义数与 INTERFACES 列举数一致(历史上 4→5→6 漏过两次)
# ---------------------------------------------------------------------------
jni_names=$(grep -o 'Java_com_proresforge_app_engine_FFmpeg_[A-Za-z0-9_]*' "$nativelib" | sort -u)
jni_defs=$(grep -cE '^Java_com_proresforge_app_engine_FFmpeg_[A-Za-z0-9_]*\(' "$nativelib")
if [[ "$jni_defs" -eq 6 ]]; then
    ok "JNI 入口 6 个: $(echo "$jni_names" | tr '\n' ' ')"
else
    bad "JNI 定义数异常: $jni_defs(期望 6;scripts/check-contracts.sh 与 INTERFACES §2 要同步改)"
fi
for n in nativeRun nativeCancel nativeInfo nativePreview initNativeLogging clearPendingCancel; do
    if ! grep -q "Java_com_proresforge_app_engine_FFmpeg_${n}(" "$nativelib"; then
        bad "native-lib.cpp 缺 JNI 函数 ${n}(INTERFACES §2 契约要求它在)"
    fi
    # Kotlin 侧 external 声明也要在(FFmpeg.kt)
    if ! grep -q "external fun ${n}(" app/src/main/java/com/proresforge/app/engine/FFmpeg.kt; then
        bad "FFmpeg.kt 缺 external fun ${n}()(C 侧有定义,Kotlin 侧漏声明)"
    fi
done
# proguard 注释要覆盖全部 6 个
for n in nativeRun nativeCancel nativeInfo nativePreview initNativeLogging clearPendingCancel; do
    grep -q "$n" "$proguard" || bad "proguard-rules.pro 注释漏 JNI 符号 $n"
done

# ---------------------------------------------------------------------------
# 2) native 源文件:CMakeLists 的 .cpp 清单 vs INTERFACES §1
# ---------------------------------------------------------------------------
cmake_srcs=$(awk '/set\(NATIVE_SOURCES/,/\)/' "$cmake" | grep -o '[a-z0-9_-]*\.cpp' | sort -u | tr '\n' ' ')
for s in native-lib.cpp preview.cpp transcode.cpp dngzip.cpp; do
    grep -q "$s" "$cmake" || bad "CMakeLists.txt 源清单缺 $s"
done
iface_srcs=$(grep -o '[a-z0-9_-]*\.cpp' "$iface" | sort -u)
missing=""
for s in $cmake_srcs; do
    echo "$iface_srcs" | grep -qx "$s" || missing="$missing $s"
done
[[ -z "$missing" ]] && ok "CMake 源清单与 INTERFACES §1 一致:$cmake_srcs" \
                   || bad "INTERFACES.md 未提到 CMake 源文件:$missing"
grep -q '4 个' "$iface" && ok "INTERFACES.md 声明 4 个 .cpp" \
                       || bad "INTERFACES.md 的 .cpp 计数不是 4(当前: $(grep -c '\.[hc]pp' "$iface") 处提及)"

# ---------------------------------------------------------------------------
# 3) FFmpeg runtime 库:CMakeLists(7)≡ build.gradle(7)≡ INTERFACES 提 postproc。
#    库名白名单提取(注释里也会出现这些名字,但取集合后不影响比较)。
# ---------------------------------------------------------------------------
runtime_re='avformat|avcodec|avfilter|avutil|swscale|swresample|postproc|avdevice'
cmake_libs=$(awk '/set\(FFMPEG_LIBS/,/\)/' "$cmake" | grep -oE "$runtime_re" | sort -u | tr '\n' ' ')
gradle_libs=$(awk '/ffmpegRuntimeLibs = listOf\(/,/\)/' "$gradle" | grep -oE "\"($runtime_re)\"" | tr -d '"' | sort -u | tr '\n' ' ')
if [[ "$cmake_libs" == "$gradle_libs" ]]; then
    ok "runtime 库清单一致($(echo "$cmake_libs" | wc -w) 个): $cmake_libs"
else
    bad "CMake($cmake_libs)与 build.gradle($gradle_libs)的 runtime 库清单不一致"
fi
[[ "$(echo "$cmake_libs" | wc -w)" -eq 7 ]] \
    || bad "FFMPEG_LIBS 数量不是 7(缺的库必须在链接图里,否则运行时 dlopen 失败)"
echo "$cmake_libs" | grep -qw postproc || bad "FFMPEG_LIBS 缺 postproc(libavfilter 的 DT_NEEDED,缺了设备上 dlopen 即失败)"
grep -q 'postproc' "$iface" || bad "INTERFACES.md runtime 列表未提 postproc"

# ---------------------------------------------------------------------------
# 4) ProRes profile 写法:契约必须「按名取」;`profile=2` 只允许出现在「早期契约写错」
#    那一行历史说明里(行内含「早期」),别的地方再出现就是漂移回潮。
# ---------------------------------------------------------------------------
stale_profile=0
while IFS= read -r hit; do
    if ! grep -q '早期' <<<"$hit"; then
        echo "[FAIL] INTERFACES.md 出现非历史的 profile=2: $hit" >&2
        stale_profile=1
    fi
done < <(grep -n 'profile=2' "$iface")
if [[ $stale_profile -eq 0 ]]; then
    ok "INTERFACES.md 的 profile=2 只出现在历史说明行(当前契约按名取 hq/4444)"
else
    fail=1
fi

# ---------------------------------------------------------------------------
# 5) spec 键:strspec.h 的键变量 ≡ kAll[] 数组 ≡ 自身注释的「共 N 个」,
#    且每个键名都在 docs/INTERFACES.md §3 表里出现(键名漂移 = Kotlin 写、native 不认)。
#    kAll[] 按条目(而不是物理行)计,防止折行导致少数。
# ---------------------------------------------------------------------------
key_lines=$(grep -cE 'inline const char \*SpecKeys::k' "$strspec")
kall_items=$(awk '/inline const char \*const SpecKeys::kAll\[/{f=1;next} f&&/\};/{f=0} f' "$strspec" | grep -o 'SpecKeys::k[A-Za-z0-9]*' | wc -l)
[[ "$key_lines" -eq "$kall_items" ]] \
    || bad "strspec.h 自相矛盾: 键变量 $key_lines 个 vs kAll[] $kall_items 个"
strspec_claims=$(grep -o '共 [0-9]* 个' "$strspec" | grep -o '[0-9]*' | sort -u)
for c in $strspec_claims; do
    [[ "$c" == "$key_lines" ]] || bad "strspec.h 注释声称「共 $c 个」但实际键 $key_lines 个"
done
missing_keys=""
for k in $(grep -oE 'inline const char \*SpecKeys::k[A-Za-z0-9]* = "[a-z0-9]+"' "$strspec" | grep -oE '"[a-z0-9]+"' | tr -d '"'); do
    grep -q "\`$k\`" "$iface" || missing_keys="$missing_keys $k"
done
if [[ -z "$missing_keys" ]]; then
    ok "spec 键 $key_lines 个:strspec 注释/kAll/INTERFACES §3 三方一致"
else
    bad "INTERFACES.md §3 表缺这些 spec 键:$missing_keys"
fi

# ---------------------------------------------------------------------------
echo "----"
if [[ $fail -eq 0 ]]; then
    echo "契约自检通过:代码与 docs/INTERFACES.md 说法一致。"
else
    echo "契约自检未通过:按 [FAIL] 行修正代码或文档(契约以代码为准)。"
fi
exit $fail
