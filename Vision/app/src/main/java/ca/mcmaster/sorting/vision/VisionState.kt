package ca.mcmaster.sorting.vision

import android.graphics.Bitmap
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Process-wide state shared by the service (writer) and the activity (reader). */
object VisionState {
    private const val TAG = "Vision"
    private const val MAX_LOG = 100

    private val _serverUrl = MutableStateFlow<String?>(null)
    /** Base URL of the running HTTP server, or null when stopped. */
    val serverUrl: StateFlow<String?> = _serverUrl

    private val _lastCapture = MutableStateFlow<Bitmap?>(null)
    val lastCapture: StateFlow<Bitmap?> = _lastCapture

    private val _lastText = MutableStateFlow("")
    /** Last reading as JSON plus the raw OCR lines, for aiming and debugging. */
    val lastText: StateFlow<String> = _lastText

    private val _log = MutableStateFlow<List<String>>(emptyList())
    val log: StateFlow<List<String>> = _log

    private val timeFormat = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)

    fun setServerUrl(url: String?) {
        _serverUrl.value = url
    }

    fun setLastRead(preview: Bitmap?, text: String) {
        if (preview != null) _lastCapture.value = preview
        _lastText.value = text
    }

    fun log(message: String) {
        Log.i(TAG, message)
        val time = synchronized(timeFormat) { timeFormat.format(Date()) }
        _log.update { (listOf("$time  $message") + it).take(MAX_LOG) }
    }
}
