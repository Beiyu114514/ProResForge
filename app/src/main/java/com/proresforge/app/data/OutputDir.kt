package com.proresforge.app.data

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.MediaStore
import com.proresforge.app.engine.AppLog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext

/**
 * SAF 输出目录管理(契约 §8.2)。
 *
 * - 首次使用没有授权目录时,调用方用 [initialTreeUri] 作为 OpenDocumentTree 的初始位置;
 * - 用户在系统选择器授权后调 [persist] 取得持久化读权限(并落盘);
 * - [outputUriFor] 用 DocumentsContract.createDocument 真正建文件,重名自动追加 " (2)"。
 *
 * treeUri 与「最近格式/画质」等设置分开存放:这里的目录写 SharedPreferences("proresforge")
 * 的 output_tree_uri 键(SettingsStore 未占用该键,两者不冲突)。
 */
class OutputDir(context: Context) {

    private val appContext = context.applicationContext
    private val resolver get() = appContext.contentResolver

    private val prefs = appContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    private val _treeUri = MutableStateFlow(
        prefs.getString(KEY_TREE, null)?.let { runCatching { Uri.parse(it) }.getOrNull() }
    )

    /** 已授权的输出目录;null = 还没选过(用 [initialTreeUri] 提示用户)。 */
    val treeUri: StateFlow<Uri?> = _treeUri.asStateFlow()

    /**
     * 本进程已建好的输出文件名(去重用)。目录真实内容靠 existingNames 查询,
     * 但同一批入队同一输入时「查询→createDocument」之间没有新变化,光靠查询会撞名,
     * 所以再记一份进程内集合(persist 换目录时清空)。
     */
    private val pendingNames = mutableSetOf<String>()

    /**
     * OpenDocumentTree 的初始目录。
     *
     * 只有「目录真实存在」时初始位置才有意义,否则选择器可能不响应,于是优先用
     * [MediaStore] 的 Movies/ProResForge(不存在则由用户自行导航);都没有时才退回
     * 硬编码的 externalstorage URI(Android 10 及共享卷场景)。
     */
    fun initialTreeUri(): Uri? =
        mediaStoreTreeUri() ?: Uri.parse(
            "content://com.android.externalstorage.documents/document/primary:Movies/ProResForge"
        )

    /**
     * MediaStore.Files / Video 里 Movies/ProResForge 对应的树 URI;表里没有该目录
     * (或查询抛异常)时返回 null。仅用于给选择器一个「已存在的落点」。
     */
    private fun mediaStoreTreeUri(): Uri? = try {
        val rel = "${Environment.DIRECTORY_MOVIES}/ProResForge"
        val projection = arrayOf(MediaStore.MediaColumns.RELATIVE_PATH)
        resolver.query(
            MediaStore.Video.Media.EXTERNAL_CONTENT_URI,
            projection,
            "${MediaStore.MediaColumns.RELATIVE_PATH} LIKE ?",
            arrayOf("$rel%"),
            null
        )?.use { c ->
            if (c.moveToFirst()) c.getString(0)?.trim('/')?.takeIf { it.isNotEmpty() } else null
        }?.let { rel ->
            // EXTERNAL_CONTENT_URI 就是主卷(external_primary):SAF 侧文档 id 即 "primary:<相对路径>"。
            // 目录真实存在(表里有行),选择器才能定位成功;SD 卡卷不落在 EXTERNAL_CONTENT_URI,不会误判。
            DocumentsContract.buildTreeDocumentUri(
                EXTERNAL_STORAGE_AUTHORITY,
                "primary:$rel",
            )
        }
    } catch (e: Exception) {
        AppLog.w(TAG, "initialTreeUri: MediaStore 查询被拒: ${e.message}")
        null
    }

    /** 拿持久化权限并存盘;没有拿到持久化授权(未从选择器返回等)时忽略。 */
    suspend fun persist(uri: Uri) = withContext(Dispatchers.IO) {
        try {
            resolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            )
        } catch (e: SecurityException) {
            return@withContext
        }
        prefs.edit().putString(KEY_TREE, uri.toString()).apply()
        _treeUri.value = uri
        // 换了目录:「本进程已占用」的名字集合作废(换目录后同名文件是另一回事)
        synchronized(pendingNames) { pendingNames.clear() }
    }

    /**
     * 在已授权目录里建一个名为 [name] 的文件,返回它 + 「最终生效的名字」。
     * 重名时自动换 [name] 的基名 + " (2)" / " (3)" …(目录里已存在同名文件时)。
     * 没授权目录/建文档失败返回 null。
     *
     * 去重同时看两处:目录里真实存在的文件(existingNames 查询) + 本进程本次会话
     * 已建好的文件(pendingNames)。后者专门堵「同一批入队同一输入」的 TOCTOU:
     * resolveName 与 createDocument 之间 provider 侧不会有新变化,但连入队两次
     * 同一输入时两次查询都读到同一份 existingNames,拿到的名字会撞车。
     */
    suspend fun outputUriFor(name: String): OutputDoc? = withContext(Dispatchers.IO) {
        val tree = _treeUri.value ?: return@withContext null
        createInTree(tree, name, existing = null)
    }

    /**
     * 批量创建(K-11):目录枚举(existingNames)只做一次,其余行为与逐个调
     * [outputUriFor] 完全一致。批内去重靠 pendingNames 在每次创建成功后累加。
     *
     * R10-11:**第一次创建失败就停**,后面的名字不再创建 —— 继续建只会在 SAF 里
     * 多留一排没人引用的 0 字节文件,而调用方的回删本来就只看它拿到的列表。
     * 返回列表仍与 [names] 等长:失败项及其后未创建的位置都是 null。
     *
     * 调用方义务(回滚):回删**返回列表里每一个非 null 文档**([deleteCreated]),
     * 不是只回删失败点之前的前缀 —— 失败即停之后两者恰好等价,但按列表回删才是
     * 与「已创建」这一事实直接对应的不变量。
     */
    suspend fun outputDocsFor(names: List<String>): List<OutputDoc?> = withContext(Dispatchers.IO) {
        val tree = _treeUri.value ?: return@withContext List(names.size) { null }
        val existing = existingNames(tree)
        val out = ArrayList<OutputDoc?>(names.size)
        for (name in names) {
            val doc = createInTree(tree, name, existing)
            out += doc
            if (doc == null) break // R10-11:第一次失败即停,后续名字不创建
        }
        while (out.size < names.size) out += null // 与 names 等长,未创建的位置填 null
        out
    }

    /**
     * 回删本类创建的文档(入队失败回滚,L-新B),并把名字从 pendingNames 撤掉,
     * 不给后续批次留下「幽灵占用」。
     */
    suspend fun deleteCreated(docs: List<OutputDoc>): Unit = withContext(Dispatchers.IO) {
        for (doc in docs) {
            try {
                // createDocument 返回的就是 document URI,可直接 deleteDocument
                DocumentsContract.deleteDocument(resolver, doc.uri)
            } catch (e: Exception) {
                AppLog.w(TAG, "回删输出文档失败: ${doc.name} err=${e.message}")
            }
            synchronized(pendingNames) { pendingNames -= doc.name }
        }
    }

    /** 单个创建;[existing] 非空时复用调用方已取好的目录快照(K-11,不重复枚举)。 */
    private fun createInTree(tree: Uri, name: String, existing: Set<String>?): OutputDoc? {
        val finalName = resolveName(tree, name, existing)
        val created = try {
            // createDocument 的 parent 必须是 document URI:直接传裸 tree URI
            // (…/tree/primary%3AMovies)在新版 Android 会被 provider 的 enforceTree
            // 以 Invalid URI 拒绝,这里先换成树根的 document URI。
            val rootDoc = DocumentsContract.buildDocumentUriUsingTree(
                tree,
                DocumentsContract.getTreeDocumentId(tree),
            )
            DocumentsContract.createDocument(resolver, rootDoc, mimeFor(finalName), finalName)
        } catch (e: Exception) {
            AppLog.w(TAG, "createDocument 失败: tree=$tree name=$finalName err=${e.message}")
            null
        } ?: return null
        // 记下本进程已占用的名字,挡住下一轮同名去重(见 KDoc 的 TOCTOU 说明)
        synchronized(pendingNames) { pendingNames += finalName }
        // 兜底:个别 provider 会自己改输出名(去重/非法字符),以实际建成的为准,
        // 否则 UI 上显示的文件名与磁盘上的对不上。
        val actual = displayName(created)
        if (actual != null && actual != finalName) {
            AppLog.w(TAG, "provider 改了输出名: 期望=$finalName 实际=$actual")
            synchronized(pendingNames) {
                pendingNames -= finalName
                pendingNames += actual
            }
        }
        return OutputDoc(uri = created, name = actual ?: finalName)
    }

    /** 查一个文档的显示名;查询失败返回 null。 */
    private fun displayName(uri: Uri): String? = try {
        resolver.query(
            uri,
            arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME),
            null, null, null
        )?.use { c -> if (c.moveToFirst()) c.getString(0) else null }
    } catch (e: Exception) {
        null
    }

    /** createDocument 的结果(uri + provider 侧最终采用的名字)。 */
    data class OutputDoc(val uri: Uri, val name: String)

    /**
     * 输出文件名:基名 + _PF_<格式标签> + 扩展名。
     * 例:fileName("MVI_0001", OutFormat.H265) → "MVI_0001_PF_h265.mp4"
     *
     * 截断按「字符」做且不切代理对:直接 take(n) 可能把 emoji(代理对)切成两半,
     * 得到一个非法 UTF-16 的文件名(部分 provider 建文件时直接失败或变成 '?')。
     */
    fun fileName(baseNameNoExt: String, format: OutFormat): String {
        val tag = when (format) {
            OutFormat.H264 -> "h264"
            OutFormat.H265 -> "h265"
            OutFormat.VP9 -> "vp9"
            OutFormat.PRORES -> "prores"
        }
        val base = baseNameNoExt.trim().trimEnd('.')
            .replace(ILLEGAL, "_")
            .truncateSafe(MAX_BASE_CHARS)
            .trimEnd()
            .ifEmpty { "output" }
        return base + "_PF_" + tag + "." + format.ext
    }

    /** 截断到 [n] 个字符,且不会把代理对切散。 */
    private fun String.truncateSafe(n: Int): String {
        if (length <= n) return this
        var end = n
        // 第 n - 1 个字符是高代理 → 说明第 n 个是它的低代理,砍到 n - 1
        if (end > 0 && Character.isHighSurrogate(this[end - 1])) end--
        if (end <= 0) return ""
        return substring(0, end)
    }

    // ------------------------------------------------------------------ 内部实现

    /** 目录下已存在的文件显示名集合(用于去重);查询失败按空集合处理。 */
    private fun existingNames(tree: Uri): Set<String> {
        val docId = try {
            DocumentsContract.getTreeDocumentId(tree)
        } catch (e: Exception) {
            return emptySet()
        }
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId)
        return try {
            resolver.query(
                children,
                arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME),
                null, null, null
            )?.use { c ->
                val out = LinkedHashSet<String>()
                while (c.moveToNext()) out.add(c.getString(0) ?: "")
                out
            } ?: emptySet()
        } catch (e: Exception) {
            // K-11 注:吞掉异常按空集合继续(去重退化,不阻断建文件),但要留痕,
            // 否则「输出名怎么带上了 (2)」这类问题无迹可寻。
            AppLog.w(TAG, "existingNames 查询失败,按空集合去重: ${e.message}")
            emptySet()
        }
    }

    /**
     * 重名追加 " (2)" / " (3)" …;极端情况(99 个全占)退化成带时间戳的名字。
     * [existingSnapshot] 传调用方已取好的目录枚举结果(批量创建 K-11 传一份共用,
     * 避免每个文件都全量枚举一次输出目录);传 null 则现场查一次。
     */
    private fun resolveName(tree: Uri, name: String, existingSnapshot: Set<String>?): String {
        val existing = existingSnapshot ?: existingNames(tree)
        // 已存在的 + 本进程已建的,都算「被占用」
        val taken = synchronized(pendingNames) {
            if (pendingNames.isEmpty()) existing else existing + pendingNames
        }
        if (name !in taken) return name
        val dot = name.lastIndexOf('.')
        val base = if (dot > 0) name.substring(0, dot) else name
        val ext = if (dot > 0) name.substring(dot) else ""
        for (i in 2..99) {
            val candidate = "$base ($i)$ext"
            if (candidate !in taken) return candidate
        }
        return base + " (" + (System.currentTimeMillis() % 100000L) + ")" + ext
    }

    private fun mimeFor(name: String): String =
        when (name.substringAfterLast('.', "").lowercase()) {
            "mp4" -> "video/mp4"
            "mov" -> "video/quicktime"
            "webm" -> "video/webm"
            "mkv" -> "video/x-matroska"
            else -> "video/*"
        }

    companion object {
        private const val PREFS = "proresforge"
        private const val KEY_TREE = "output_tree_uri"
        private const val TAG = "OutputDir"

        /**
         * 外部存储 SAF provider 的 authority。DocumentsContract 没把它暴露成公开常量
         * (android-35 的 android.jar 里没有),只能写字面量;它自 API 24 起稳定未变。
         */
        private const val EXTERNAL_STORAGE_AUTHORITY = "com.android.externalstorage.documents"

        /** 文件名里不允许出现的字符(FAT/SAF 通用)。 */
        private val ILLEGAL = Regex("[\\\\/:*?\"<>|]")

        /**
         * 输出文件基名上限。留足 "_PF_h265.mp4" 后缀与 EXT4/FAT 255 字节上限
         * (120 个 CJK 字符 = 360 字节,建文件会 ENAMETOOLONG)。
         */
        private const val MAX_BASE_CHARS = 60
    }
}
