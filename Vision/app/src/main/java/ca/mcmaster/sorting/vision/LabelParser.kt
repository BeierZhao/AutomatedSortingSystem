package ca.mcmaster.sorting.vision

import org.json.JSONObject

/**
 * Pure-Kotlin label interpretation (no Android types), so it can be unit
 * tested on the JVM. Turns the raw OCR lines and barcodes of one camera frame
 * into the outputs of interface 3.1 "read part label".
 *
 * Label layout (see Vision/README.md):
 *
 *     Origin:       Hamilton
 *     Destination:  Toronto
 *     Part ID:                    <- blank text cell
 *     ||| |||| || |||             <- barcode carrying the part ID
 *          345678
 *
 * The key ("Origin:") and the value ("Hamilton") sit in separate table cells,
 * so OCR may return them as one line or as two lines on the same row. Both
 * cases are handled.
 */

/** An OCR text line with its bounding box in image pixels. */
data class TextBox(
    val text: String,
    val left: Int,
    val top: Int,
    val right: Int,
    val bottom: Int,
) {
    val height: Int get() = (bottom - top).coerceAtLeast(1)
    val centerX: Int get() = (left + right) / 2
}

/** A decoded barcode with its bounding box in image pixels. */
data class BarcodeBox(
    val rawValue: String?,
    val left: Int,
    val top: Int,
    val right: Int,
    val bottom: Int,
) {
    val area: Long get() = (right - left).toLong() * (bottom - top).toLong()
}

/** The four outputs of interface 3.1, with the exact enum strings of the spec. */
data class LabelReading(
    val readingResult: String,
    val origin: String,
    val destination: String,
    val partId: String,
) {
    fun toJson(): JSONObject = JSONObject()
        .put("reading_result", readingResult)
        .put("origin", origin)
        .put("destination", destination)
        .put("part_id", partId)

    companion object {
        const val SUCCESS = "success"
        const val INFO_LOST = "info lost"
        const val NONE = "none"
        const val FAULT = "fault"

        /** The vision system itself failed: no reading could be made. */
        val FAULT_READING = LabelReading(FAULT, FAULT, FAULT, FAULT)
    }
}

class LabelParser(
    knownCities: List<String> = DEFAULT_CITIES,
    /** Replace a near-miss OCR result with the closest known city name. */
    private val snapToKnownCities: Boolean = true,
) {
    private val cities = knownCities.map { it.trim() }.filter { it.isNotEmpty() }

    fun parse(lines: List<TextBox>, barcodes: List<BarcodeBox>): LabelReading {
        val keyed = lines.map { it to splitKey(it.text) }
        val keyLines = keyed.filter { it.second != null }.map { it.first }.toSet()

        val origin = readCityField(Key.ORIGIN, keyed, lines, keyLines)
        val destination = readCityField(Key.DESTINATION, keyed, lines, keyLines)
        val partId = readPartId(keyed.any { it.second?.first == Key.PART_ID }, barcodes)

        val fields = listOf(origin, destination, partId)
        val found = fields.count { it != LabelReading.FAULT }
        val result = when {
            found == 0 -> LabelReading.NONE
            found == fields.size && fields.none { it == LabelReading.NONE } -> LabelReading.SUCCESS
            else -> LabelReading.INFO_LOST
        }
        return LabelReading(result, origin, destination, partId)
    }

    // ---- Origin / Destination ---------------------------------------------

    private fun readCityField(
        key: Key,
        keyed: List<Pair<TextBox, Pair<Key, String>?>>,
        lines: List<TextBox>,
        keyLines: Set<TextBox>,
    ): String {
        // If several labels are in view, the biggest one is nearest the camera.
        val (keyBox, split) = keyed
            .filter { it.second?.first == key }
            .maxByOrNull { it.first.height }
            ?: return LabelReading.FAULT
        val inline = cleanValue(split!!.second)
        val raw = inline.ifEmpty { cleanValue(valueRightOf(keyBox, lines, keyLines)?.text.orEmpty()) }
        if (raw.isEmpty()) return LabelReading.NONE
        return if (snapToKnownCities) snapCity(raw) else raw
    }

    /** The nearest non-key line on the same row, to the right of [key]. */
    private fun valueRightOf(key: TextBox, lines: List<TextBox>, keyLines: Set<TextBox>): TextBox? {
        val maxGap = key.height * MAX_VALUE_GAP_IN_LINE_HEIGHTS
        return lines
            .filter { it !in keyLines && it.centerX > key.right - key.height }
            .filter { verticalOverlap(key, it) >= 0.5 * minOf(key.height, it.height) }
            .filter { it.left - key.right <= maxGap }
            .minByOrNull { it.left - key.right }
    }

    private fun verticalOverlap(a: TextBox, b: TextBox): Int =
        (minOf(a.bottom, b.bottom) - maxOf(a.top, b.top)).coerceAtLeast(0)

    /** Keep letters, spaces, hyphens, apostrophes and periods; blank if no letters. */
    private fun cleanValue(text: String): String {
        val kept = text.filter { it.isLetter() || it == ' ' || it == '-' || it == '\'' || it == '.' }
            .replace(Regex("\\s+"), " ")
            .trim(' ', '-', '.', '\'')
        return if (kept.any { it.isLetter() }) kept else ""
    }

    private fun snapCity(raw: String): String {
        val norm = raw.lowercase().filter { it.isLetter() }
        val best = cities.minByOrNull { levenshtein(norm, it.lowercase().filter { c -> c.isLetter() }) }
            ?: return raw
        val distance = levenshtein(norm, best.lowercase().filter { it.isLetter() })
        return if (distance <= maxOf(1, best.length / 4)) best else raw
    }

    // ---- Part ID ------------------------------------------------------------

    private fun readPartId(keyFound: Boolean, barcodes: List<BarcodeBox>): String {
        val code = barcodes.maxByOrNull { it.area }
        if (code != null) {
            parseInt32(code.rawValue)?.let { return it }
            return LabelReading.NONE // label present, but its content is not a part ID
        }
        return if (keyFound) LabelReading.NONE else LabelReading.FAULT
    }

    private fun parseInt32(raw: String?): String? {
        val s = raw?.trim() ?: return null
        if (!Regex("-?\\d{1,10}").matches(s)) return null
        val v = s.toLongOrNull() ?: return null
        return if (v in Int.MIN_VALUE..Int.MAX_VALUE) v.toString() else null
    }

    // ---- Keys -----------------------------------------------------------------

    private enum class Key(val normalized: String, val tolerance: Int) {
        ORIGIN("origin", 1),
        DESTINATION("destination", 2),
        PART_ID("partid", 1),
    }

    /**
     * If [text] starts with a key ("Origin:", "Destination", "Part ID:"),
     * returns the key and the rest of the line (the inline value, may be blank).
     */
    private fun splitKey(text: String): Pair<Key, String>? {
        val t = text.trim()
        val colon = t.indexOfFirst { it == ':' || it == ';' }
        if (colon >= 0) {
            val key = matchKey(t.substring(0, colon)) ?: return null
            return key to t.substring(colon + 1)
        }
        // No colon read: try the first one or two words as the key.
        val words = t.split(Regex("\\s+"))
        for (n in 1..minOf(2, words.size)) {
            val key = matchKey(words.take(n).joinToString(" ")) ?: continue
            return key to words.drop(n).joinToString(" ")
        }
        return null
    }

    private fun matchKey(head: String): Key? {
        val norm = head.lowercase()
            .map { c -> when (c) { '0' -> 'o'; '1', 'l', '|', '!' -> 'i'; else -> c } }
            .filter { it.isLetter() }
            .joinToString("")
        if (norm.isEmpty()) return null
        return Key.entries
            .map { it to levenshtein(norm, it.normalized) }
            .filter { (key, d) -> d <= key.tolerance }
            .minByOrNull { it.second }
            ?.first
    }

    companion object {
        val DEFAULT_CITIES = listOf("Hamilton", "Toronto", "Mississauga", "Oakville", "Burlington")

        /** How far right of the key a value cell may start, in key line heights. */
        private const val MAX_VALUE_GAP_IN_LINE_HEIGHTS = 10

        fun levenshtein(a: String, b: String): Int {
            var prev = IntArray(b.length + 1) { it }
            var cur = IntArray(b.length + 1)
            for (i in 1..a.length) {
                cur[0] = i
                for (j in 1..b.length) {
                    val cost = if (a[i - 1] == b[j - 1]) 0 else 1
                    cur[j] = minOf(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost)
                }
                val t = prev; prev = cur; cur = t
            }
            return prev[b.length]
        }
    }
}
