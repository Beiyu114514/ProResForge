# CMakeLists.txt —— ProResForge native 引擎
#
# target 名固定为 ffmpeg-transcode(契约 §1: libffmpeg-transcode.so,
# Kotlin 侧 System.loadLibrary("ffmpeg-transcode"))。
#
# 依赖预编译产物: app/src/main/cpp/prebuilt/<abi>/{include,lib},
# 由 scripts/build-ffmpeg-android.sh 生成(未生成时下面直接 FATAL_ERROR 提示)。

cmake_minimum_required(VERSION 3.22.1)

project(ffmpeg-transcode C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# >>> prf-release-flags-begin
# 段D 硬化(2026-10-06):旧逻辑 `if(CMAKE_CXX_FLAGS_RELEASE MATCHES "-O")` 是
# P2-01 漏洞 —— "-O0" 也含 "-O",于是外部(NDK toolchain / Gradle 缓存 / 手工
# -DCMAKE_CXX_FLAGS_RELEASE=-O0)塞进来的 -O0/-O1/-O2 会被当成"已优化"原样放行,
# Release 产物静默退回 -O0。现在:先剥掉所有 -O0/-O1/-O2(含 -Os/-Oz/-Ofast),
# 再统一强制 -O3 -DNDEBUG(多写几个 -O3 无害,但保持干净)。
# 注意必须放在 project() 之后:project() + toolchain 会用 toolchain 的
# CMAKE_CXX_FLAGS_RELEASE 覆盖 pre-project 的赋值(CMake 3.22.1 + NDK 27 实测)。
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  if(CMAKE_CXX_FLAGS_RELEASE MATCHES "-ffast-math" OR CMAKE_CXX_FLAGS_RELEASE MATCHES "-Ofast")
    message(FATAL_ERROR
        "Release flags 里含 -ffast-math/-Ofast: ${CMAKE_CXX_FLAGS_RELEASE}\n"
        "本仓库禁用(改变浮点语义,影响转码/PCM 跨平台一致性)。\n"
        "请检查 externalNativeBuild 的 cmake arguments 与 CMakePresets/缓存。")
  endif()
  # 剥掉所有优化级别标记(-O0/-O1/-O2/-O3/-Os/-Oz/-Ofast/-O 单独一个),
  # 以及重复的 -DNDEBUG(下面会统一补一个,避免 flags 串里出现两遍)。
  # 注意用的是「替换成空」而不是「追加」:CMake 里后出现的 -O 才生效,
  # 不删掉旧的 -O0 就可能被后面的 clang 参数再翻回去。
  string(REGEX REPLACE "-O([0123szf]|fast)?" "" _prf_release_flags "${CMAKE_CXX_FLAGS_RELEASE}")
  string(REGEX REPLACE "-DNDEBUG" "" _prf_release_flags "${_prf_release_flags}")
  string(STRIP "${_prf_release_flags}" _prf_release_flags)
  # 剥掉 -O1/-DNDEBUG 后可能留下的连续空格,纯粹为了 flags 串可读
  string(REGEX REPLACE "  +" " " _prf_release_flags "${_prf_release_flags}")
  if(_prf_release_flags STREQUAL "")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
  else()
    set(CMAKE_CXX_FLAGS_RELEASE "${_prf_release_flags} -O3 -DNDEBUG")
  endif()
  message(STATUS "[prf] Release CXX flags = ${CMAKE_CXX_FLAGS_RELEASE}")
endif()
# <<< prf-release-flags-end

# 预编译 FFmpeg 产物根目录
set(FFMPEG_PREBUILT "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/${ANDROID_ABI}")

if(NOT EXISTS "${FFMPEG_PREBUILT}/include")
    message(FATAL_ERROR
        "缺少 FFmpeg 预编译产物: ${FFMPEG_PREBUILT}/include\n"
        "请先执行: NDK=/path/to/android-ndk ./scripts/build-ffmpeg-android.sh "
        "(在仓库 ProResForge/ 下运行),它会把 include/lib 装到该目录。")
endif()

# ---------------------------------------------------------------------------
# 源码:新增 .cpp 请同步更新本处与 KDoc(契约 §1 的本地实现清单)
# ---------------------------------------------------------------------------
set(NATIVE_SOURCES
    native-lib.cpp
    preview.cpp
    transcode.cpp
    dngzip.cpp
)

add_library(ffmpeg-transcode SHARED ${NATIVE_SOURCES})

target_include_directories(ffmpeg-transcode PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${FFMPEG_PREBUILT}/include"
)

# ---------------------------------------------------------------------------
# 链接 FFmpeg 共享库(IMPORTED,显式指定 IMPORTED_LOCATION 便于校验存在性)
#
# 打包语义(实测 AGP 行为):APK 里只会带上「链接图中的 SHARED IMPORTED 库」;
# 用 file(COPY) 等手段放进输出目录的散文件不会被打包。libavfilter.so 的
# DT_NEEDED 依赖 libpostproc.so(共享构建默认如此),它不在链接图里就没进 APK,
# 设备上 dlopen libavfilter.so 直接失败 → 所有探测/转码报 UnsatisfiedLinkError。
# 因此 postproc 必须留在列表里(虽然代码不直接调用它)。
# ---------------------------------------------------------------------------
set(FFMPEG_LIBS avformat avcodec avfilter avutil swscale swresample postproc)

foreach(lib ${FFMPEG_LIBS})
    set(so "${FFMPEG_PREBUILT}/lib/lib${lib}.so")
    if(NOT EXISTS "${so}")
        message(FATAL_ERROR
            "缺少 FFmpeg 动态库: ${so}\n"
            "请先执行 ./scripts/build-ffmpeg-android.sh 生成预编译产物。")
    endif()
    add_library(ffmpeg-${lib} SHARED IMPORTED)
    set_target_properties(ffmpeg-${lib} PROPERTIES IMPORTED_LOCATION "${so}")
    target_link_libraries(ffmpeg-transcode PRIVATE ffmpeg-${lib})
endforeach()

# 日志 + Android 特例(mediacodec 编码器需要 libmediandk/libandroid)
find_library(LOG_LIB log)
target_link_libraries(ffmpeg-transcode PRIVATE ${LOG_LIB})

# ---------------------------------------------------------------------------
# LibRaw(DNG 序列 zip 解码,契约 §6/§10):静态库 libraw.a + 头文件 libraw/。
# 同一个脚本装到 prebuilt 布局:include/libraw/libraw.h、lib/libraw.a。
# CMake 源文件清单里加 dngzip.cpp 之后,这里必须同时给静态 IMPORTED 目标,
# 否则链接期报 undefined reference to LibRaw::*。
# zlib 用 NDK sysroot 的 libz.so(public),不引入新的 .so 打包面。
# ---------------------------------------------------------------------------
set(LIBRAW_PREBUILT "${FFMPEG_PREBUILT}")
set(RAW_A "${LIBRAW_PREBUILT}/lib/libraw.a")
set(RAW_H "${LIBRAW_PREBUILT}/include/libraw/libraw.h")

if(NOT EXISTS "${RAW_A}")
    message(FATAL_ERROR
        "缺少 LibRaw 静态库: ${RAW_A}\n"
        "DNG zip(MotionCam)转码需要它。请重新执行 "
        "NDK=/path/to/android-ndk ./scripts/build-ffmpeg-android.sh,\n"
        "它会克隆 LibRaw/LibRaw.git(源码)+ LibRaw/LibRaw-cmake.git(脚本)并以 "
        "NDK 工具链交叉编译出 libraw.a(依赖 NDK sysroot 的 zlib)。")
endif()

if(NOT EXISTS "${RAW_H}")
    message(FATAL_ERROR "缺少 LibRaw 头文件: ${RAW_H}(重新跑 scripts/build-ffmpeg-android.sh)")
endif()

# >>> prf-libraw-gate-begin
# ---------------------------------------------------------------------------
# 段A(2026-10-06)provenance 门禁:「存在性检查」证明不了线程安全 —— LibRaw
# 用 LIBRAW_NOTHREADS 编出来的 libraw.a 一样能通过上面的存在性检查,但它是
# 单线程版,批量 DNG 解码并发会崩。因此这里必须校验 per-ABI manifest:
# scripts/build-ffmpeg-android.sh 里 `libraw_provenance_ok()` 生成的
# ${prebuilt}/<abi>/libraw-manifest-<abi>(key=value)。CMake 用 file(STRINGS)
# 逐行解析,只认「键=值」且键不重复。
# 不满足 → FATAL_ERROR,提示重跑 build 脚本(它会删掉旧产物强制从源码重建 raw_r)。
# 字段语义见 prebuilt/README.md。
# ---------------------------------------------------------------------------
set(LIBRAW_MANIFEST "${FFMPEG_PREBUILT}/libraw-manifest-${ANDROID_ABI}")

if(NOT EXISTS "${LIBRAW_MANIFEST}")
    message(FATAL_ERROR
        "缺少 LibRaw provenance manifest: ${LIBRAW_MANIFEST}\n"
        "libraw.a 存在不等于线程安全(单线程 LIBRAW_NOTHREADS 构建也能过存在性检查),\n"
        "必须重跑: NDK=/path/to/android-ndk ./scripts/build-ffmpeg-android.sh\n"
        "它会校验/重建 ${ANDROID_ABI} 的 raw_r 产物并生成 manifest。")
endif()

file(STRINGS "${LIBRAW_MANIFEST}" _raw_manifest_lines REGEX "^[A-Za-z_][A-Za-z0-9_]*=")
if(_raw_manifest_lines STREQUAL "")
    message(FATAL_ERROR
        "LibRaw provenance manifest 解析失败(没有任何 key=value 行): ${LIBRAW_MANIFEST}\n"
        "请重跑 ./scripts/build-ffmpeg-android.sh 重新生成。")
endif()

set(RAW_MANIFEST_VALUES "")
set(RAW_MANIFEST_DUP_KEYS "")
foreach(_line ${_raw_manifest_lines})
    string(REGEX MATCH "^[A-Za-z_][A-Za-z0-9_]*" _k "${_line}")
    if(DEFINED _raw_m_${_k})
        list(APPEND RAW_MANIFEST_DUP_KEYS "${_k}")
    endif()
    set(_raw_m_${_k} "${_line}")
    list(APPEND RAW_MANIFEST_VALUES "${_line}")
endforeach()

if(NOT RAW_MANIFEST_DUP_KEYS STREQUAL "")
    list(JOIN RAW_MANIFEST_DUP_KEYS ", " _dups)
    message(FATAL_ERROR
        "LibRaw provenance manifest 里有重复键: ${_dups}(manifest 语义不唯一,拒绝使用)\n"
        "请重跑 ./scripts/build-ffmpeg-android.sh 重新生成: ${LIBRAW_MANIFEST}")
endif()

# key → 值的取值(解析一次,随后所有校验都走它)
function(prf_raw_manifest_get key outvar)
    foreach(_line ${RAW_MANIFEST_VALUES})
        string(REGEX MATCH "^[A-Za-z_][A-Za-z0-9_]*" _k "${_line}")
        if(_k STREQUAL key)
            string(REGEX REPLACE "^[A-Za-z_][A-Za-z0-9_]*=" "" _v "${_line}")
            set(${outvar} "${_v}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${outvar} "" PARENT_SCOPE)
endfunction()

prf_raw_manifest_get("abi"           RAW_M_ABI)
prf_raw_manifest_get("api_level"     RAW_M_API)
prf_raw_manifest_get("ndk_revision"  RAW_M_NDK)
prf_raw_manifest_get("libraw_tag"    RAW_M_TAG)
prf_raw_manifest_get("libraw_cmake_sha" RAW_M_CMAKE_SHA)
prf_raw_manifest_get("script_sha256" RAW_M_SCRIPT_SHA)
prf_raw_manifest_get("raw_r_target"  RAW_M_TARGET)
prf_raw_manifest_get("libraw_nothreads" RAW_M_NOTHREADS)
prf_raw_manifest_get("raw_r_cmake_flags" RAW_M_FLAGS)
# B-06:threads_lib 与 build.gradle.kts / scripts/verify-prebuilt.sh 的必填口径对齐
prf_raw_manifest_get("threads_lib"   RAW_M_THREADS)
prf_raw_manifest_get("libraw_sha256" RAW_M_SHA)
prf_raw_manifest_get("manual"        RAW_M_MANUAL)

if(RAW_M_ABI STREQUAL "" OR RAW_M_API STREQUAL ""
   OR RAW_M_NDK STREQUAL "" OR RAW_M_TAG STREQUAL ""
   OR RAW_M_CMAKE_SHA STREQUAL "" OR RAW_M_SCRIPT_SHA STREQUAL ""
   OR RAW_M_TARGET STREQUAL "" OR RAW_M_NOTHREADS STREQUAL ""
   OR RAW_M_FLAGS STREQUAL "" OR RAW_M_THREADS STREQUAL ""
   OR RAW_M_SHA STREQUAL "" OR RAW_M_MANUAL STREQUAL "")
    message(FATAL_ERROR
        "LibRaw provenance manifest 缺字段: ${LIBRAW_MANIFEST}\n"
        "至少需要 abi/api_level/ndk_revision/libraw_tag/libraw_cmake_sha/script_sha256/"
        "raw_r_target/libraw_nothreads/raw_r_cmake_flags/threads_lib/libraw_sha256/manual。\n"
        "请重跑 ./scripts/build-ffmpeg-android.sh。")
endif()

if(NOT RAW_M_ABI STREQUAL "${ANDROID_ABI}")
    message(FATAL_ERROR
        "LibRaw manifest 的 abi=${RAW_M_ABI} 与本次构建 ANDROID_ABI=${ANDROID_ABI} 不一致\n"
        "(per-ABI manifest 不能跨 ABI 复用):${LIBRAW_MANIFEST}\n"
        "请重跑 NDK=/path/to/android-ndk ./scripts/build-ffmpeg-android.sh 重新生成。")
endif()

if(RAW_M_MANUAL STREQUAL "true")
    message(FATAL_ERROR
        "LibRaw provenance manifest 是手工替换品(manual=true):${LIBRAW_MANIFEST}\n"
        "BUILDINFO 不再允许手工替换:必须重跑 "
        "NDK=/path/to/android-ndk ./scripts/build-ffmpeg-android.sh\n"
        "从源码重建线程安全 raw_r 产物并生成新 manifest。")
endif()

if(NOT RAW_M_MANUAL STREQUAL "false")
    message(FATAL_ERROR
        "LibRaw manifest 的 manual=${RAW_M_MANUAL} 不是 true/false,无法判定:${LIBRAW_MANIFEST}")
endif()

if(NOT RAW_M_TARGET STREQUAL "raw_r")
    message(FATAL_ERROR
        "LibRaw manifest raw_r_target=${RAW_M_TARGET}(必须是 raw_r,即 LibRaw 的线程安全 target)\n"
        "${LIBRAW_MANIFEST}\n请重跑 ./scripts/build-ffmpeg-android.sh。")
endif()

if(NOT RAW_M_NOTHREADS STREQUAL "absent")
    message(FATAL_ERROR
        "LibRaw manifest libraw_nothreads=${RAW_M_NOTHREADS}(必须是 absent)\n"
        "LIBRAW_NOTHREADS 会把 LibRaw 编成单线程版,批量 DNG 解码会崩:${LIBRAW_MANIFEST}\n"
        "请重跑 ./scripts/build-ffmpeg-android.sh。")
endif()

if(RAW_M_FLAGS MATCHES "LIBRAW_NOTHREADS")
    message(FATAL_ERROR
        "LibRaw manifest raw_r_cmake_flags 里含 LIBRAW_NOTHREADS:${RAW_M_FLAGS}\n"
        "${LIBRAW_MANIFEST}\n请重跑 ./scripts/build-ffmpeg-android.sh。")
endif()

# libraw.a 实际内容必须与 manifest 记录的 sha256 一致(库被换成别的产物就现形)
file(SHA256 "${RAW_A}" RAW_A_SHA256)
if(NOT RAW_M_SHA STREQUAL RAW_A_SHA256)
    message(FATAL_ERROR
        "LibRaw 静态库 sha256 与 provenance manifest 不符:\n"
        "  manifest : ${RAW_M_SHA}\n"
        "  libraw.a : ${RAW_A_SHA256} (${RAW_A})\n"
        "这可能意味着 libraw.a 被手工拷贝/替换过。BUILDINFO 不再允许手工替换,\n"
        "请重跑 NDK=/path/to/android-ndk ./scripts/build-ffmpeg-android.sh。")
endif()
# <<< prf-libraw-gate-end

add_library(ffmpeg-raw STATIC IMPORTED)
set_target_properties(ffmpeg-raw PROPERTIES IMPORTED_LOCATION "${RAW_A}")
target_link_libraries(ffmpeg-transcode PRIVATE ffmpeg-raw z)

# ---------------------------------------------------------------------------
# 打包说明:FFmpeg 运行时 .so 的 APK 打包完全由上面的 IMPORTED 链接图驱动,
# 这里不再做任何 file(COPY) —— AGP 不打包拷进输出目录的散文件(此前
# libpostproc.so 因此漏打,导致设备上整条 native 链 dlopen 失败)。
# 若以后新增运行时 .so,把它加进 FFMPEG_LIBS(或单独建 IMPORTED 目标并链接)。
# ---------------------------------------------------------------------------
