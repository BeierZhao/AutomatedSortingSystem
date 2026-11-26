package ca.mcmaster.sorting.vision

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleService
import java.net.Inet4Address
import java.net.NetworkInterface

/**
 * Foreground service that keeps the camera and the HTTP server alive while the
 * screen is off or the app is in the background.
 */
class VisionService : LifecycleService() {

    private lateinit var settings: VisionSettings
    private var reader: LabelReader? = null
    private var server: VisionHttpServer? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    override fun onCreate() {
        super.onCreate()
        settings = VisionSettings(this)
        ServiceCompat.startForeground(
            this, NOTIFICATION_ID, buildNotification(),
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA else 0,
        )
        acquireLocks()

        val r = LabelReader(this, this).also { it.start(settings.torch) }
        reader = r
        val port = settings.port
        try {
            server = VisionHttpServer(port) { r.read(settings.parser(), settings.readTimeoutMs) }
                .also { it.start(SOCKET_READ_TIMEOUT_MS, false) }
            val url = "http://${localIpAddress() ?: "<no-wifi>"}:$port"
            VisionState.setServerUrl(url)
            VisionState.log("HTTP server listening on $url${VisionHttpServer.READ_PART_LABEL}")
        } catch (e: Exception) {
            VisionState.log("HTTP server failed to start on port $port: ${e.message}")
            stopSelf()
        }
    }

    override fun onDestroy() {
        server?.stop()
        reader?.stop()
        wakeLock?.takeIf { it.isHeld }?.release()
        wifiLock?.takeIf { it.isHeld }?.release()
        VisionState.setServerUrl(null)
        VisionState.log("service stopped")
        super.onDestroy()
    }

    private fun acquireLocks() {
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Vision::server").apply { acquire() }
        val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        @Suppress("DEPRECATION")
        val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) WifiManager.WIFI_MODE_FULL_LOW_LATENCY
        else WifiManager.WIFI_MODE_FULL_HIGH_PERF
        wifiLock = wm.createWifiLock(mode, "Vision::server").apply { acquire() }
    }

    private fun buildNotification(): Notification {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, getString(R.string.channel_name), NotificationManager.IMPORTANCE_LOW)
        )
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_label)
            .setContentTitle(getString(R.string.notification_title))
            .setContentText(getString(R.string.notification_text, settings.port))
            .setContentIntent(open)
            .setOngoing(true)
            .build()
    }

    companion object {
        private const val CHANNEL_ID = "vision_server"
        private const val NOTIFICATION_ID = 1
        private const val SOCKET_READ_TIMEOUT_MS = 30_000

        fun start(context: Context) =
            ContextCompat.startForegroundService(context, Intent(context, VisionService::class.java))

        fun stop(context: Context) = context.stopService(Intent(context, VisionService::class.java))

        /** First site-local IPv4 address, i.e. the phone's Wi-Fi address. */
        fun localIpAddress(): String? = try {
            NetworkInterface.getNetworkInterfaces().toList()
                .filter { it.isUp && !it.isLoopback }
                .flatMap { it.inetAddresses.toList() }
                .filterIsInstance<Inet4Address>()
                .firstOrNull { it.isSiteLocalAddress }
                ?.hostAddress
        } catch (e: Exception) {
            null
        }
    }
}
