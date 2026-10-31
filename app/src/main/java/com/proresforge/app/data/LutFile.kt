package com.proresforge.app.data

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.security.MessageDigest

/**
 * 暂存到 cacheDir 的 LUT 信息。
 * - path:cacheDir/luts/<sha256(uri)>.<ext> 绝对路径(传给 FFmpeg 的 lut= 行 / nativePreview)
 * - title:文件里的 TITLE(没有则 null);size:LUT_3D_SIZE(没有则 null);
 * - bytes:暂存文件字节数
 * - ext:"cube" / "3dl"(不含点)
 */
data class StagedLut(val path: String, val title: String?, val size: Int?, val ext: String, val bytes: Long = 0L)

/**
 * LUT 暂存管理(契约 §2 fd 语义末段 + §8.2)。
 * 只接受 3D LUT(.cube / .3dl;1D 表没有渲染路径,带 LUT_1D_SIZE 判非法);
 * 解析头部 TITLE / LUT_3D_SIZE,校验通过后整体拷到
 * context.cacheDir/luts/<sha256(uri 字符串)>.<ext>,非法返回 null。
 * (注:契约 §8.2 早期表述写过「.dat / sha1」,以这里的实现为准:
 *  <sha256(uri 字符串)>.<ext>。)
 * 暂存成功同时把「最近使用 LUT(uri + 名称)」写回 SettingsStore(契约 §8.2),
 * 强度不做处理(由 UI 的强度滑条负责)。
 */
class LutManager(context: Context) {

    private val appContext = context.applicationContext
    private val settings = SettingsStore(appContext)

    /** cacheDir/luts 目录(不存在则创建)。 */
    private val lutDir: File
        get() = File(appContext.cacheDir, "luts").apply { if (!exists()) mkdirs() }

    /** 暂存;非法(扩展名/1D 表/无法读取/无 LUT 数据)返回 null。 */
    suspend fun stage(uri: Uri): StagedLut? = withContext(Dispatchers.IO) {
        val ext = extensionOf(uri) ?: return@withContext null

        val bytes = try {
            appContext.contentResolver.openInputStream(uri)?.use { it.readCapped(MAX_BYTES) }
        } catch (e: IOException) {
            null
        } catch (e: SecurityException) {
            null
        } ?: return@withContext null

        // 空文件判非法(超大文件 readCapped 会直接返回 null)
        if (bytes.isEmpty()) return@withContext null

        val header = parseHeader(bytes)
        if (!header.valid) return@withContext null

        // 原子落盘:先写临时文件再改名,避免缓存里留下半个 LUT。
        // renameTo 在 Linux/ext4 上就是原子替换,所以不需要先 delete dest(删了反而
        // 凭空多出一段「目标不存在」的窗口,和 JobStore 写 jobs.json 同理)。
        val dest = File(lutDir, digest(uri.toString()) + "." + ext)
        // tmp 名带唯一后缀:固定名 "<dest>.tmp" 时,同一 uri 并发 stage 会写坏同一文件。
        val tmp = File(dest.parentFile, "${dest.name}.${System.nanoTime()}.tmp")
        try {
            tmp.writeBytes(bytes)
            if (!tmp.renameTo(dest)) {
                tmp.delete()
                return@withContext null
            }
        } catch (e: IOException) {
            tmp.delete()
            return@withContext null
        }

        // 契约 §8.2:记录最近使用的 LUT(不含强度)
        settings.lastLutUri = uri.toString()
        settings.lastLutName = header.title ?: displayNameOf(uri)

        StagedLut(
            path = dest.absolutePath,
            title = header.title,
            size = header.size,
            ext = ext,
            bytes = bytes.size.toLong(),
        )
    }

    // ------------------------------------------------------------------ 内部实现

    private data class Header(val valid: Boolean, val title: String?, val size: Int?)

    /** 解析 LUT 头部:TITLE / LUT_3D_SIZE / SIZE(1D 表判非法);并用数据行数量兜底校验。 */
    private fun parseHeader(bytes: ByteArray): Header {
        // 去掉可能的 UTF-8 BOM
        val text = String(bytes, Charsets.UTF_8).trimStart('\uFEFF')
        var title: String? = null
        var size: Int? = null
        var dataLines = 0
        var scanned = 0
        for (raw in text.lineSequence()) {
            if (++scanned > HEADER_SCAN_LINES) break
            val line = raw.trim()
            if (line.isEmpty() || line.startsWith("#")) continue
            // 按空白切成「键 + 值」:不要用固定偏移(line.drop(11)),否则键名/分隔符
            // 稍有出入就取到错内容(旧实现假设键长恒为 11)。
            val key = line.takeWhile { !it.isWhitespace() }.uppercase()
            val value = line.dropWhile { !it.isWhitespace() }.trim()
            when (key) {
                "TITLE" ->
                    title = value.trim('"', '\'', ' ').ifEmpty { null }

                "LUT_1D_SIZE" ->
                    // 1D 表没有渲染路径(native 只有 lut3d 一条路),staging 就拒,
                    // 免得 stage 成功后每个转码/预览必失败。
                    return Header(valid = false, title = title, size = null)

                "LUT_3D_SIZE", "SIZE" ->
                    size = firstInt(value) ?: size

                else ->
                    if (isNumericRow(line) && ++dataLines >= MIN_DATA_LINES && size != null) return Header(
                        valid = true,
                        title = title,
                        size = size.takeIf { it in 2..256 },
                    )
            }
        }
        val saneSize = size?.takeIf { it in 2..256 }
        // 有合理 size → 合法;没有 size 但至少有 MIN_DATA_LINES 行「三个浮点」的数据也放行
        val valid = saneSize != null || dataLines >= MIN_DATA_LINES
        return Header(valid = valid, title = title, size = saneSize)
    }

    /** 一行是否 LUT 数据行:至少 3 个可解析为实数的分量(RGB 三元组)。 */
    private fun isNumericRow(line: String): Boolean {
        val tok = line.split(Regex("\\s+"))
        if (tok.size < 3) return false
        return tok.take(3).all { it.toFloatOrNull() != null }
    }

    private fun firstInt(rest: String): Int? =
        rest.trim().split(Regex("\\s+")).firstOrNull()?.toIntOrNull()

    /** 扩展名小写(不含点);只接受 cube/3dl,其它返回 null。 */
    private fun extensionOf(uri: Uri): String? {
        val name = displayNameOf(uri)?.lowercase()
        val byName = name?.substringAfterLast('.', "")?.takeIf { it.isNotEmpty() && it != name }
        val byMime = appContext.contentResolver.getType(uri)
            ?.substringAfterLast('/', "")?.lowercase()
        return (byName ?: byMime)?.takeIf { it in EXT_ORDER }
    }

    private fun displayNameOf(uri: Uri): String? = try {
        appContext.contentResolver.query(
            uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null
        )?.use { c -> if (c.moveToFirst()) c.getString(0) else null }
            ?: uri.lastPathSegment?.substringAfterLast('/')
    } catch (e: Exception) {
        uri.lastPathSegment?.substringAfterLast('/')
    }

    /** sha256(uri 字符串) 十六进制小写。 */
    private fun digest(s: String): String =
        MessageDigest.getInstance("SHA-256").digest(s.toByteArray(Charsets.UTF_8))
            .joinToString("") { b -> "%02x".format(b) }

    /** 限长读取:超过 limit 字节返回 null(判非法),避免坏文件吃内存。 */
    private fun InputStream.readCapped(limit: Int): ByteArray? {
        val out = java.io.ByteArrayOutputStream()
        val chunk = ByteArray(16 * 1024)
        var total = 0
        while (true) {
            val n = read(chunk)
            if (n < 0) break
            total += n
            if (total > limit) return null
            out.write(chunk, 0, n)
        }
        return out.toByteArray()
    }

    companion object {
        /** 接受的扩展名(缓存路径的扩展名也由此决定)。 */
        private val EXT_ORDER = listOf("cube", "3dl")

        /** 头部最多扫这么多行,够覆盖 TITLE / SIZE / 数据行校验,又不会被垃圾文件拖住。 */
        private const val HEADER_SCAN_LINES = 4096

        /** 没有 SIZE 行时,至少要有这么多「三个浮点」数据行才算合法 LUT。 */
        private const val MIN_DATA_LINES = 8

        /** LUT 文件体积上限(16 MiB):超过即判非法。 */
        private const val MAX_BYTES = 16 * 1024 * 1024
    }
}
