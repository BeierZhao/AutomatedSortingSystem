package ca.mcmaster.sorting.vision

import android.content.Context

/** User-editable settings, stored in SharedPreferences. */
class VisionSettings(context: Context) {
    private val prefs = context.getSharedPreferences("vision", Context.MODE_PRIVATE)

    var port: Int
        get() = prefs.getInt(KEY_PORT, DEFAULT_PORT)
        set(v) = prefs.edit().putInt(KEY_PORT, v).apply()

    /** City names that near-miss OCR results are snapped to. */
    var knownCities: List<String>
        get() = prefs.getString(KEY_CITIES, null)?.split(',')?.map { it.trim() }?.filter { it.isNotEmpty() }
            ?: LabelParser.DEFAULT_CITIES
        set(v) = prefs.edit().putString(KEY_CITIES, v.joinToString(", ")).apply()

    var snapToKnownCities: Boolean
        get() = prefs.getBoolean(KEY_SNAP, true)
        set(v) = prefs.edit().putBoolean(KEY_SNAP, v).apply()

    var torch: Boolean
        get() = prefs.getBoolean(KEY_TORCH, false)
        set(v) = prefs.edit().putBoolean(KEY_TORCH, v).apply()

    /** Internal timeout for one capture + recognition, in ms. */
    var readTimeoutMs: Long
        get() = prefs.getLong(KEY_TIMEOUT, DEFAULT_TIMEOUT_MS)
        set(v) = prefs.edit().putLong(KEY_TIMEOUT, v).apply()

    fun parser() = LabelParser(knownCities, snapToKnownCities)

    companion object {
        const val DEFAULT_PORT = 8080
        const val DEFAULT_TIMEOUT_MS = 8000L
        private const val KEY_PORT = "port"
        private const val KEY_CITIES = "cities"
        private const val KEY_SNAP = "snap"
        private const val KEY_TORCH = "torch"
        private const val KEY_TIMEOUT = "timeout_ms"
    }
}
