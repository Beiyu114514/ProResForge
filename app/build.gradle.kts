// ProResForge 应用模块构建脚本。
// 版本/插件别名全部来自 gradle/libs.versions.toml,勿在此处写死版本号。
// 注意:下面的 prf* 门禁工具函数用了 java.security/java.time —— Gradle Kotlin DSL
// 脚本里 `java.*` 全限定名解析不到(2026-10-06 实测 5 个编译错误),必须显式 import。
import java.security.MessageDigest
import java.time.ZoneOffset
import java.time.ZonedDateTime
import java.time.format.DateTimeFormatter
import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
    id("org.jetbrains.kotlin.plugin.serialization")
}

// ---------------------------------------------------------------------------
// FFmpeg 运行时 .so 进 APK(契约 §6 + scripts/build-ffmpeg-android.sh)。
//
// 编译脚本产物布局是 app/src/main/cpp/prebuilt/<abi>/{include,lib/libav*.so},
// 而 AGP 的 jniLibs 约定是 <srcDir>/<abi>/*.so —— 不接受 <abi>/lib/ 嵌套。
// 这里用 Sync 把 lib/*.so 展平成 build/generated/ffmpegJniLibs/<abi>/,
// 再把该任务本身注册为 jniLibs 源目录(Gradle 自动建立任务依赖,
// 保证 mergeJniLibs 执行前文件已就位)。脚本未跑过或产物不全时在任务开始处失败。
// 这里只打包 CMake 链接图中的运行库，避免把 prebuilt 目录中的其他共享库带进 APK。
// ---------------------------------------------------------------------------
val ffmpegRuntimeLibs = listOf(
    "avformat",
    "avcodec",
    "avfilter",
    "avutil",
    "swscale",
    "swresample",
    "postproc",
)

val syncFfmpegJniLibs by tasks.registering(Sync::class) {
    from(fileTree("src/main/cpp/prebuilt") {
        include(*ffmpegRuntimeLibs.map { "*/lib/lib$it.so" }.toTypedArray())
    })
    into(layout.buildDirectory.dir("generated/ffmpegJniLibs"))
    doFirst {
        val abis = listOf("arm64-v8a", "armeabi-v7a")
        val missing = abis.flatMap { abi ->
            ffmpegRuntimeLibs.filterNot { lib ->
                project.file("src/main/cpp/prebuilt/$abi/lib/lib$lib.so").isFile
            }.map { lib -> "$abi/lib$lib.so" }
        }
        check(missing.isEmpty()) {
            "Missing required FFmpeg runtime libraries: ${missing.joinToString()}. " +
                "Run scripts/build-ffmpeg-android.sh first."
        }
    }
    eachFile {
        val segments = relativePath.segments // [<abi>, "lib", "libavcodec.so"]
        if (segments.size >= 3) path = "${segments[0]}/${segments[2]}"
    }
    includeEmptyDirs = false
}

// ---------------------------------------------------------------------------
// 段A/段D:release 变体 native 门禁(纯 Kotlin,Windows 上不依赖 bash)
//
// 背景(audit P0/P1-01、P2-01):旧逻辑只查 prebuilt/<abi>/lib/libraw.a「存在」就跳过
// LibRaw 重建,一个用 LIBRAW_NOTHREADS 编出来的单线程产物(甚至手工拷来的)也能过检;
// Release 侧 `if(CMAKE_CXX_FLAGS_RELEASE MATCHES "-O")` 又连 -O0 都当"已优化"放行。
// 现有三层防线:
//   1. scripts/build-ffmpeg-android.sh:按 provenance manifest 决定复用还是强制重建;
//   2. app/src/main/cpp/CMakeLists.txt:configure 时按 ABI 校验同一份 manifest;
//   3. 这里(release 变体构建前):manifest 必填字段非空 / manual=false / abi 对齐 /
//      libraw.a 实际 SHA-256 对账,并清掉会污染 Release flags 的旧 .cxx 缓存,
//      再把 PRF_* 宏与优化级别落到 app/build/native-flags-manifest.txt。
// ---------------------------------------------------------------------------
val PRF_NATIVE_ABIS = listOf("arm64-v8a", "armeabi-v7a")

// per-ABI provenance manifest 必填字段。必须与 scripts/build-ffmpeg-android.sh 的
// write_libraw_manifest()、scripts/verify-prebuilt.sh(16 必填)、CMakeLists.txt 的
// prf-libraw-gate 段四处保持一致(B-06:此前 gradle 漏 threads_lib、CMake 只查 11 键)。
val PRF_LIBRAW_MANIFEST_REQUIRED_FIELDS = listOf(
    "abi", "api_level", "ndk_revision", "libraw_tag", "libraw_cmake_sha",
    "raw_r_target", "libraw_nothreads", "raw_r_cmake_flags", "threads_lib",
    "libraw_sha256", "symbols_sha256", "members_sha256",
    "script_sha256", "build_time_utc", "manual", "tls_probe",
)

// release 块里一个都不许出现的 cmake 参数(会把 Release 打回 -O0/Debug,或直接覆盖
// CMakeLists.txt 段D 归一化后的优化 flags)
val PRF_FORBIDDEN_CMAKE_ARGS = listOf(
    "-O0", "-O1", "-O2", "-Os", "-Oz", "-Ofast", "-ffast-math",
    "CMAKE_CXX_FLAGS_RELEASE", "CMAKE_C_FLAGS_RELEASE",
    "CMAKE_BUILD_TYPE=Debug",
)

// --- 纯函数 helpers(不碰 Project,便于阅读与单测) ---

/** 解析 key=value manifest;返回值 = (键值表, 重复键列表(重复键按"不可信"处理)。 */
fun prfReadKeyValueManifest(file: File): Pair<Map<String, String>, List<String>> {
    val kv = LinkedHashMap<String, String>()
    val duplicated = mutableListOf<String>()
    file.forEachLine { raw ->
        val line = raw.trim()
        if (line.isEmpty() || line.startsWith("#")) return@forEachLine
        val idx = line.indexOf('=')
        if (idx <= 0) return@forEachLine
        val key = line.substring(0, idx).trim()
        if (kv.containsKey(key)) {
            duplicated += key
        } else {
            kv[key] = line.substring(idx + 1).trim()
        }
    }
    return kv to duplicated
}

/** 小写 hex SHA-256(Java 里 byte 是有符号的,必须 & 0xFF,否则会出现 ffffffXX)。 */
fun prfSha256Hex(file: File): String {
    val digest = MessageDigest.getInstance("SHA-256")
    file.inputStream().use { ins ->
        val buf = ByteArray(1 shl 16)
        while (true) {
            val n = ins.read(buf)
            if (n <= 0) break
            digest.update(buf, 0, n)
        }
    }
    return digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xFF) }
}

/** 从 C++ 源码里解析 `#define <macro> <n>` 的取值(拿不到就返回 null,不猜)。 */
fun prfParseMacro(sourceFile: File, macro: String): String? {
    if (!sourceFile.isFile) return null
    val re = Regex("""(?m)^\s*#\s*define\s+""" + macro + """\s+\(?-?\d+\)?""")
    val m = re.find(sourceFile.readText()) ?: return null
    return Regex("""-?\d+""").find(m.value.substringAfter(macro))?.value
}

/** 找出会破坏 Release 优化级别的 cmake 参数(命中即视为 release 块被污染)。 */
fun prfForbiddenCmakeArgs(args: List<String>): List<String> =
    args.filter { arg ->
        PRF_FORBIDDEN_CMAKE_ARGS.any { bad ->
            arg == bad || arg.startsWith("$bad=") || arg.startsWith("-D$bad=")
        }
    }

// --- PRF_* 宏:生效值 + 来源(与传给 CMake 的 externalNativeBuild 参数同源) ---
/** 从 `-P` 属性 / 环境变量里取覆盖值(都为空当没传)。 */
fun prfOverride(prop: String?, env: String?): String? =
    prop?.trim()?.takeIf { it.isNotEmpty() } ?: env?.trim()?.takeIf { it.isNotEmpty() }

val prfTranscodeCpp: File = layout.projectDirectory.file("src/main/cpp/transcode.cpp").asFile

// -PprfProresThreads / -PprfAvframePool(或同名环境变量)优先;
// 否则直接解析 transcode.cpp 里的 #define,保证 manifest 记的就是真正编进 .so 的值。
val prfProresThreadsProp: String? = prfOverride(
    project.findProperty("prfProresThreads") as String?,
    System.getenv("PRF_PROTOTYPE_THREADS"),
)
val prfAvframePoolProp: String? = prfOverride(
    project.findProperty("prfAvframePool") as String?,
    System.getenv("PRF_AVFRAME_POOL"),
)

val prfProresThreads: Pair<String?, String> =
    prfProresThreadsProp?.let { it to "-PprfProresThreads/env PRF_PROTOTYPE_THREADS" }
        ?: prfParseMacro(prfTranscodeCpp, "PRF_PRORES_THREADS")?.let { it to "transcode.cpp #define" }
        ?: (null to "未解析到(交给源码默认值,manifest 记为 source-default)")

val prfAvframePool: Pair<String?, String> =
    prfAvframePoolProp?.let { it to "-PprfAvframePool/env PRF_AVFRAME_POOL" }
        ?: prfParseMacro(prfTranscodeCpp, "PRF_AVFRAME_POOL")?.let { it to "transcode.cpp #define" }
        ?: (null to "未解析到(交给源码默认值,manifest 记为 source-default)")

// release 变体传给 CMake 的完整参数清单(android{} 的 release 块按这个追加)
val prfReleaseCmakeArgs: List<String> = buildList {
    add("-DCMAKE_BUILD_TYPE=Release")
    prfProresThreads.first?.let { add("-DPRF_PRORES_THREADS=$it") }
    prfAvframePool.first?.let { add("-DPRF_AVFRAME_POOL=$it") }
}

// --- 门禁任务 1:清理会污染 Release flags 的旧 .cxx 配置 ---
val prfCleanStaleNativeCxx = tasks.register("prfCleanStaleNativeCxx") {
    group = "verification"
    description = "删除 app/.cxx 下 release 系的旧 CMake 配置(flags 不含 -O3 即视为被污染)"
    outputs.upToDateWhen { false }
    doLast {
        // 注意:AGP 的 externalNativeBuild 缓存放在模块根的 app/.cxx(不在 build/ 下),
        // 早期实现误用 layout.buildDirectory(指向 app/build/.cxx),目录不存在 = 清理
        // 永远空跑,旧 -O2/-O0 缓存会让 Release 门禁 UP-TO-DATE 跳过(2026-10-06 实测撞见)。
        val root = layout.projectDirectory.dir(".cxx").asFile.canonicalFile
        if (!root.isDirectory) {
            logger.lifecycle("[prf] ${root} 不存在,跳过缓存清理")
            return@doLast
        }
        var removed = 0
        root.listFiles()?.sortedBy { it.name }?.forEach { variantDir ->
            if (!variantDir.isDirectory) return@forEach
            // .cxx/<variant>…:只动 release 系(AGP 里 release 变体的目录名是
            // RelWithDebInfo/Release);Debug 等其它变体的缓存留给它们自己用。
            val isReleaseish = variantDir.name.equals("RelWithDebInfo", ignoreCase = true) ||
                variantDir.name.equals("Release", ignoreCase = true)
            if (!isReleaseish) {
                logger.lifecycle("[prf] .cxx/${variantDir.name}:非 release 配置,保留")
                return@forEach
            }
            // 先收集再删:边走边 deleteRecursively 会漏掉后续节点
            val caches = variantDir.walkTopDown().maxDepth(4)
                .filter { it.isFile && it.name == "CMakeCache.txt" }.toList()
            caches.forEach cacheFound@{ cache ->
                    val configDir = cache.parentFile.canonicalFile
                    if (!configDir.path.startsWith(root.path)) return@cacheFound
                    val text = cache.readText()
                    val flags = Regex("(?m)^CMAKE_CXX_FLAGS_RELEASE:STRING=(.*)$")
                        .find(text)?.groupValues?.get(1) ?: ""
                    val buildType = Regex("(?m)^CMAKE_BUILD_TYPE:STRING=(.*)$")
                        .find(text)?.groupValues?.get(1) ?: ""
                    val poisoned = buildType.equals("Debug", ignoreCase = true) ||
                        flags.contains("-O0") || flags.contains("-O1") || flags.contains("-O2") ||
                        flags.contains("-Os") || flags.contains("-Ofast") || !flags.contains("-O3")
                    if (poisoned) {
                        logger.lifecycle(
                            "[prf] .cxx/${variantDir.name}:删除被污染的旧配置 ${configDir.name} " +
                                "(CMAKE_BUILD_TYPE=${buildType}, CMAKE_CXX_FLAGS_RELEASE='${flags.trim()}')"
                        )
                        if (configDir.deleteRecursively()) removed++
                    } else {
                        logger.lifecycle("[prf] .cxx/${variantDir.name}:缓存 flags 已是 -O3,保留")
                    }
                }
        }
        logger.lifecycle("[prf] .cxx 清理完成:删除 $removed 个旧配置目录")
    }
}

// --- 门禁任务 2:per-ABI LibRaw provenance manifest 校验(段A) ---
val prfVerifyLibRawManifests = tasks.register("prfVerifyLibRawManifests") {
    group = "verification"
    description = "校验 prebuilt/<abi>/libraw-manifest-<abi> 与 lib/libraw.a 的实际 SHA-256(manual=true 一律拒绝)"
    outputs.upToDateWhen { false }
    doLast {
        val prebuiltBase = layout.projectDirectory.dir("src/main/cpp/prebuilt")
        val report = mutableListOf<String>()
        val problems = mutableListOf<String>()
        for (abi in PRF_NATIVE_ABIS) {
            val abiDir = prebuiltBase.dir(abi).asFile
            val manifestFile = File(abiDir, "libraw-manifest-$abi")
            val librawA = File(abiDir, "lib/libraw.a")
            val rel = manifestFile.relativeTo(projectDir).path.replace('\\', '/')
            if (!manifestFile.isFile) {
                problems += "缺 ${abi} 的 provenance manifest(${rel})"
                report += "FAIL ${abi}:manifest 不存在(${rel})"
                continue
            }
            val (kv, duplicated) = prfReadKeyValueManifest(manifestFile)
            if (duplicated.isNotEmpty()) problems += "${abi} manifest 重复键:${duplicated.joinToString()}"
            val missing = PRF_LIBRAW_MANIFEST_REQUIRED_FIELDS.filter { kv[it].isNullOrEmpty() }
            if (missing.isNotEmpty()) problems += "${abi} manifest 缺字段:${missing.joinToString()}"

            val manual = kv["manual"]?.lowercase()
            when (manual) {
                null, "" -> problems += "${abi} manifest 没有 manual 字段"
                "true" -> problems +=
                    "${abi} manifest manual=true:BUILDINFO 不再允许手工替换,必须重跑 scripts/build-ffmpeg-android.sh"
                "false" -> {}
                else -> problems += "${abi} manifest manual=${kv["manual"]}(既不是 true 也不是 false,无法判定)"
            }
            if (kv["abi"] != null && kv["abi"] != abi) {
                problems += "${abi} manifest 的 abi=${kv["abi"]} 与目录/ANDROID_ABI 不一致"
            }
            if (kv["raw_r_target"] != null && kv["raw_r_target"] != "raw_r") {
                problems += "${abi} manifest raw_r_target=${kv["raw_r_target"]}(必须是 raw_r)"
            }
            if (kv["libraw_nothreads"] != null && kv["libraw_nothreads"] != "absent") {
                problems += "${abi} manifest libraw_nothreads=${kv["libraw_nothreads"]}(单线程构建产物)"
            }
            if ((kv["raw_r_cmake_flags"] ?: "").contains("LIBRAW_NOTHREADS")) {
                problems += "${abi} manifest raw_r_cmake_flags 含 LIBRAW_NOTHREADS(单线程构建)"
            }
            if (!librawA.isFile) {
                problems += "${abi} 找不到 lib/libraw.a"
                report += "FAIL ${abi}:lib/libraw.a 不存在"
                continue
            }
            val actual = prfSha256Hex(librawA)
            val recorded = kv["libraw_sha256"]
            if (recorded.isNullOrEmpty()) {
                problems += "${abi} manifest 没有 libraw_sha256"
            } else if (!recorded.equals(actual, ignoreCase = true)) {
                problems += "${abi} libraw.a 实际 SHA-256 与 manifest 不符(manifest=${recorded}, 实际=${actual})"
            }
            val symbolsOk = (kv["symbols_sha256"]?.length == 64) && (kv["members_sha256"]?.isNotEmpty() == true)
            if (!symbolsOk) problems += "${abi} manifest 的 symbols_sha256/members_sha256 不合格(需 scripts/verify-prebuilt.sh 复算)"
            if (problems.none { it.startsWith("$abi ") }) {
                report += "PASS ${abi}:manifest 字段齐全,manual=false,libraw_sha256=${actual.take(16)}…"
            }
        }
        report.forEach { logger.lifecycle("[prf] $it") }
        if (problems.isNotEmpty()) {
            throw GradleException(
                buildString {
                    appendLine("LibRaw provenance 门禁失败(release 变体,${problems.size} 项):")
                    problems.forEach { appendLine("  × $it") }
                    appendLine()
                    appendLine("provenance manifest 必须由脚本自动生成(manual=false),手工替换的产物一律拒绝。")
                    appendLine("修复:重跑 scripts/build-ffmpeg-android.sh 生成 per-ABI manifest 与 libraw.a;")
                    appendLine("      接着可用 scripts/verify-prebuilt.sh app/src/main/cpp/prebuilt/<abi> 复算符号/成员 sha。")
                }
            )
        }
        logger.lifecycle("[prf] LibRaw provenance 门禁通过(${PRF_NATIVE_ABIS.joinToString()})")
    }
}

// --- 门禁任务 3:把本次 release 的宏/优化级别写进 app/build/native-flags-manifest.txt ---
val prfWriteNativeFlagsManifest = tasks.register("prfWriteNativeFlagsManifest") {
    group = "verification"
    description = "写 app/build/native-flags-manifest.txt(buildType/ABI/PRF_* 宏/优化级别)"
    val outFile = layout.buildDirectory.file("native-flags-manifest.txt")
    outputs.file(outFile)
    doLast {
        val target = outFile.get().asFile
        target.parentFile?.mkdirs()
        val forbidden = prfForbiddenCmakeArgs(prfReleaseCmakeArgs)
        if (forbidden.isNotEmpty()) {
            throw GradleException(
                "release 的 externalNativeBuild cmake arguments 里含会覆盖 Release 优化级别的参数: " +
                    "${forbidden.joinToString()}。请从 app/build.gradle.kts 的 release 块删除(段D)。"
            )
        }
        val stamp = ZonedDateTime.now(ZoneOffset.UTC)
        target.writeText(
            buildString {
                appendLine("# ProResForge native flags manifest(app/build.gradle.kts 自动生成,勿手改)")
                appendLine("# 由任务 :app:prfWriteNativeFlagsManifest 生成;release 变体构建前刷新")
                appendLine("buildType=Release")
                appendLine("abi=${PRF_NATIVE_ABIS.joinToString(",")}")
                appendLine("cmake_build_type_arg=Release")
                appendLine("cmake_arguments=${prfReleaseCmakeArgs.joinToString(" ")}")
                appendLine("release_cxx_flags=-O3 -DNDEBUG")
                appendLine("prf_prores_threads=${prfProresThreads.first ?: "source-default"}")
                appendLine("prf_prores_threads_source=${prfProresThreads.second}")
                appendLine("prf_avframe_pool=${prfAvframePool.first ?: "source-default"}")
                appendLine("prf_avframe_pool_source=${prfAvframePool.second}")
                appendLine(
                    "generated_utc=" + stamp.format(
                        DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss'Z'")
                    )
                )
            }
        )
        logger.lifecycle("[prf] 已写 ${target.path}")
    }
}

// --- 总入口:release 构建前依次执行上面三个任务 ---
val prfReleaseNativeGate by tasks.registering {
    group = "verification"
    description = "release native 门禁:清旧 .cxx → 校验 LibRaw manifest → 写 flags manifest"
    dependsOn(prfCleanStaleNativeCxx, prfVerifyLibRawManifests, prfWriteNativeFlagsManifest)
}

// 挂到 release 变体的 native 构建路径上(configure/build/merge),debug 完全不受影响。
tasks.matching { task ->
    task.name.startsWith("buildCMakeRelease") ||
        task.name.startsWith("configureCMakeRelease") ||
        task.name == "externalNativeBuildRelease" ||
        task.name == "mergeReleaseNativeLibs" ||
        task.name == "mergeReleaseJniLibs"
}.configureEach {
    dependsOn(prfReleaseNativeGate)
}

android {
    namespace = "com.proresforge.app"
    compileSdk = 35
    ndkVersion = "27.0.12077973"

    sourceSets {
        getByName("main") {
            jniLibs.srcDir(syncFfmpegJniLibs)
        }
    }

    defaultConfig {
        applicationId = "com.proresforge.app"
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = "1.0.0"

        // FFmpeg 预编译产物只有这两个 ABI(scripts/build-ffmpeg-android.sh 默认只编它们),
        // 不加过滤 AGP 会尝试给 x86/x86_64 也配 CMake,CMakeLists 直接 FATAL_ERROR
        ndk {
            abiFilters.addAll(listOf("arm64-v8a", "armeabi-v7a"))
        }
    }

    buildFeatures {
        compose = true
    }

    // native 代理负责 src/main/cpp/CMakeLists.txt(契约 §1/§6);
    // CMake target: ffmpeg-transcode -> libffmpeg-transcode.so
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    testOptions {
        // 纯 JVM 单测(S-3/§7 补测)会触到 android.util.Log(AppLog)与
        // android.net.Uri(JobSpec 字段类型):未实现的方法返回默认值而不是抛
        // "not mocked",测试里不依赖这些方法的返回语义(Log 只写 logcat,
        // Uri 只存字段不调用方法)。
        unitTests.isReturnDefaultValues = true
    }

    // C2(2026-10-11):release 签名凭据只从根目录 keystore.properties(gitignored)
    // 读取;文件不存在时本 config 保持空字段,release 块也不挂 signingConfig ——
    // 出的是未签名 APK(签名配置说明见 docs/RELEASE-SIGNING.md)。
    // 严禁把口令写进本文件或任何入库文件。
    signingConfigs {
        create("release") {
            val p = Properties().apply {
                val f = rootProject.file("keystore.properties")
                if (f.exists()) f.inputStream().use { load(it) }
            }
            val sf = p.getProperty("storeFile")
            if (sf != null) {
                storeFile = rootProject.file(sf)
                storePassword = p.getProperty("storePassword")
                keyAlias = p.getProperty("keyAlias")
                keyPassword = p.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            // C3(minify 决策,2026-10-11):维持 isMinifyEnabled=false。
            // 理由:①本地工具类应用,混淆收益低(无商业逻辑可藏,体积 +~2MB 可接受);
            // ②开 R8 必须先补 JNI keep 规则(FFmpeg native 方法、JNI 反射触碰类、
            //   JobSpec/JobRecord 序列化字段,现有 proguard-rules.pro 仅 9 行不够),
            //   并重跑全套设备冒烟(Round10 既定结论:没有冒烟不开 R8);
            //   1.0.0 首发不做这笔风险投入,以后开 R8 走独立变更+全套 D 段回归。
            // P1-01(2026-10-11):keep 规则已补全(proguard-rules.pro);临时开启 R8
            // 构建+模拟器冒烟六场景通过(探测/转码 rc=0、取消 rc=1 删半成品、历史
            // 序列化/清空/重启持久化,无 ClassNotFound/UnsatisfiedLinkError)。按验证
            // 步骤回退 false,是否开启留给将来的独立变更(前提见下方 C3 注释与
            // docs/RELEASE-SIGNING.md 的 R8 一节)。
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            // C2(签名):凭据从根目录 keystore.properties(gitignored)读,
            // 不入库、不硬编码;文件缺失时保持无 signingConfig(可出未签名包,
            // 供 CI/存档场景),本机出正式包前先按 docs/RELEASE-SIGNING.md 配好。
            val ksProps = Properties().apply {
                val f = rootProject.file("keystore.properties")
                if (f.exists()) f.inputStream().use { load(it) }
            }
            if (ksProps.getProperty("storeFile") != null) {
                signingConfig = signingConfigs.getByName("release")
            }
            // P0-2(Release native 基线):上面的 externalNativeBuild.cmake 主块对
            // debug/release 共用。externalNativeBuild 声明在 VariantDimension 上,
            // 因此在 release buildType 内单独给 CMake configure 追加
            // -DCMAKE_BUILD_TYPE=Release(触发 app/src/main/cpp/CMakeLists.txt 里的
            // -O3 -DNDEBUG 段),assembleDebug 完全不受影响。
            // 证据(2026-10-06 实测):此前 native 只出过 Debug/-O0,同一段解码热循环
            // -O0 36.1ms vs -O3 5.6ms(6.4×)。Debug 保持可调试,不升优化。
            //
            // 段D 加固(2026-10-06):这里**只**允许追加 -DCMAKE_BUILD_TYPE=Release 与
            // PRF_* 宏(prfReleaseCmakeArgs,与 app/build/native-flags-manifest.txt 同源),
            // 不得出现 -O0/-O1/-O2/-ffast-math 或 CMAKE_CXX_FLAGS_RELEASE ——
            // release 块的参数由 prfWriteNativeFlagsManifest 任务自查,命中即构建失败。
            externalNativeBuild {
                cmake {
                    arguments += prfReleaseCmakeArgs
                }
            }
        }
    }

    packaging {
        // S-2 注:这两个排除只影响依赖 jar 里散落的单行许可文件(避免打包重名),
        // 不影响合规 —— 完整许可证文本随 assets/licenses/ 打包,归因界面见
        // ui/LicensesScreen.kt 与 assets/licenses/NOTICE.md。
        resources.excludes += setOf("/META-INF/{AL2.0,LGPL2.1}")
    }
}

dependencies {
    // --- AndroidX 基础 ---
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.documentfile)

    // --- Compose(由 BOM 统一版本)---
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)
    debugImplementation(libs.androidx.ui.tooling)

    // --- XML 主题父级解析:Theme.Material3.DayNight.NoActionBar 来自此库,
    //     移除它 aapt 会报 "resource style/Theme.Material3.DayNight.NoActionBar not found"。
    //     若最终决定改用 android:Theme.Material.NoActionBar 之类父级,可连同
    //     libs.versions.toml 的 material 条目一起删除。
    implementation(libs.material)

    // --- Kotlin 协程(data/engine 直接使用,勿依赖传递) ---
    implementation(libs.kotlinx.coroutines.android)

    // --- Kotlin 序列化(契约 §8.2 JobStore 的 kotlinx.serialization)---
    implementation(libs.kotlinx.serialization.json)

    // --- 单元测试(纯 JVM:app/src/test,跑在本地 JVM 上,不需要设备/NDK)---
    testImplementation(libs.junit)
}

// 说明(2026-10-11 更新):release 签名已接入 —— signingConfigs["release"] 从根目录
// keystore.properties(gitignored)读凭据;本机需按 docs/RELEASE-SIGNING.md 配置。
// 仓库不含任何 keystore / 签名凭据,签名文件一律由本机库外保管并保持 ignored。
