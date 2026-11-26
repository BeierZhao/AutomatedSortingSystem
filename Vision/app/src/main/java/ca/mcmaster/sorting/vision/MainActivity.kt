package ca.mcmaster.sorting.vision

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import ca.mcmaster.sorting.vision.databinding.ActivityMainBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import java.net.HttpURLConnection
import java.net.URL

/**
 * Status and setup screen: starts/stops the service, edits settings, shows the
 * last captured frame and reading (to aim the camera), and the message log.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var settings: VisionSettings

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { granted ->
            if (granted[Manifest.permission.CAMERA] == true) VisionService.start(this)
            else Toast.makeText(this, R.string.camera_permission_needed, Toast.LENGTH_LONG).show()
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        settings = VisionSettings(this)
        loadSettings()

        binding.startStop.setOnClickListener {
            if (VisionState.serverUrl.value == null) startService() else VisionService.stop(this)
        }
        binding.save.setOnClickListener { saveSettings() }
        binding.testRead.setOnClickListener { testRead() }
        binding.battery.setOnClickListener {
            startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
        }

        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch {
                    VisionState.serverUrl.collect { url ->
                        binding.status.text = if (url == null) getString(R.string.status_stopped)
                        else getString(R.string.status_running, url + VisionHttpServer.READ_PART_LABEL)
                        binding.startStop.setText(if (url == null) R.string.start else R.string.stop)
                        binding.testRead.isEnabled = url != null
                    }
                }
                launch { VisionState.lastCapture.collect { binding.capture.setImageBitmap(it) } }
                launch { VisionState.lastText.collect { binding.result.text = it } }
                launch { VisionState.log.collect { binding.log.text = it.joinToString("\n") } }
            }
        }

        // Start straight away so a phone that is rebooted or reopened is back in service.
        if (savedInstanceState == null && hasCameraPermission()) VisionService.start(this)
    }

    private fun hasCameraPermission() =
        ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    private fun startService() {
        val needed = mutableListOf(Manifest.permission.CAMERA)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) needed += Manifest.permission.POST_NOTIFICATIONS
        if (hasCameraPermission()) VisionService.start(this) else permissionLauncher.launch(needed.toTypedArray())
    }

    private fun loadSettings() {
        binding.port.setText(settings.port.toString())
        binding.timeout.setText(settings.readTimeoutMs.toString())
        binding.cities.setText(settings.knownCities.joinToString(", "))
        binding.snap.isChecked = settings.snapToKnownCities
        binding.torch.isChecked = settings.torch
    }

    private fun saveSettings() {
        val port = binding.port.text.toString().toIntOrNull()
        val timeout = binding.timeout.text.toString().toLongOrNull()
        if (port == null || port !in 1024..65535 || timeout == null || timeout < 1000) {
            Toast.makeText(this, R.string.invalid_settings, Toast.LENGTH_LONG).show()
            return
        }
        settings.port = port
        settings.readTimeoutMs = timeout
        settings.knownCities = binding.cities.text.toString().split(',').map { it.trim() }.filter { it.isNotEmpty() }
        settings.snapToKnownCities = binding.snap.isChecked
        settings.torch = binding.torch.isChecked
        loadSettings()
        // Port and torch are applied when the service starts, so restart it.
        if (VisionState.serverUrl.value != null) {
            VisionService.stop(this)
            lifecycleScope.launch {
                // Wait until the old instance has released the port and the camera.
                withTimeoutOrNull(3000) { VisionState.serverUrl.first { it == null } }
                delay(300)
                VisionService.start(this@MainActivity)
            }
        }
        Toast.makeText(this, R.string.saved, Toast.LENGTH_SHORT).show()
    }

    /** Calls our own endpoint over loopback, exactly as the controller would. */
    private fun testRead() {
        binding.testRead.isEnabled = false
        lifecycleScope.launch {
            val reply = withContext(Dispatchers.IO) {
                try {
                    val url = URL("http://127.0.0.1:${settings.port}${VisionHttpServer.READ_PART_LABEL}")
                    val conn = url.openConnection() as HttpURLConnection
                    conn.requestMethod = "POST"
                    conn.connectTimeout = 2000
                    conn.readTimeout = (settings.readTimeoutMs + 5000).toInt()
                    conn.doOutput = true
                    conn.setRequestProperty("Content-Type", "application/json")
                    conn.outputStream.use { it.write("{}".toByteArray()) }
                    val body = conn.inputStream.bufferedReader().use { it.readText() }
                    "HTTP ${conn.responseCode}: $body"
                } catch (e: Exception) {
                    "test read failed: ${e.message}"
                }
            }
            Toast.makeText(this@MainActivity, reply, Toast.LENGTH_LONG).show()
            binding.testRead.isEnabled = VisionState.serverUrl.value != null
        }
    }
}
