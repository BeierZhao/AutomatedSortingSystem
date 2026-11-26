package ca.mcmaster.sorting.vision

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Matrix
import android.util.Size
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageCapture
import androidx.camera.core.ImageCaptureException
import androidx.camera.core.ImageProxy
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleOwner
import com.google.android.gms.tasks.Tasks
import com.google.mlkit.vision.barcode.BarcodeScannerOptions
import com.google.mlkit.vision.barcode.BarcodeScanning
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.text.TextRecognition
import com.google.mlkit.vision.text.latin.TextRecognizerOptions
import java.util.concurrent.CompletableFuture
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock

/**
 * Owns the back camera (bound to [lifecycleOwner], normally the foreground
 * service) and runs capture -> ML Kit OCR + barcode scan -> [LabelParser].
 */
class LabelReader(private val context: Context, private val lifecycleOwner: LifecycleOwner) {

    private val cameraExecutor = Executors.newSingleThreadExecutor()
    private val textRecognizer = TextRecognition.getClient(TextRecognizerOptions.DEFAULT_OPTIONS)
    private val barcodeScanner = BarcodeScanning.getClient(
        BarcodeScannerOptions.Builder()
            .setBarcodeFormats(
                Barcode.FORMAT_CODE_128, Barcode.FORMAT_CODE_39, Barcode.FORMAT_CODE_93,
                Barcode.FORMAT_CODABAR, Barcode.FORMAT_ITF, Barcode.FORMAT_EAN_13,
                Barcode.FORMAT_EAN_8, Barcode.FORMAT_UPC_A, Barcode.FORMAT_UPC_E,
            )
            .build()
    )

    /** One read at a time; a second request waits for the first. */
    private val readLock = ReentrantLock()

    @Volatile private var imageCapture: ImageCapture? = null
    private var provider: ProcessCameraProvider? = null

    /** Binds the camera. Call on the main thread. */
    fun start(torch: Boolean) {
        val future = ProcessCameraProvider.getInstance(context)
        future.addListener({
            try {
                val p = future.get()
                provider = p
                val capture = ImageCapture.Builder()
                    .setCaptureMode(ImageCapture.CAPTURE_MODE_MINIMIZE_LATENCY)
                    .setResolutionSelector(
                        ResolutionSelector.Builder()
                            .setResolutionStrategy(
                                ResolutionStrategy(
                                    Size(1920, 1440),
                                    ResolutionStrategy.FALLBACK_RULE_CLOSEST_LOWER_THEN_HIGHER,
                                )
                            )
                            .build()
                    )
                    .build()
                p.unbindAll()
                val cam = p.bindToLifecycle(lifecycleOwner, CameraSelector.DEFAULT_BACK_CAMERA, capture)
                cam.cameraInfo.cameraState.observe(lifecycleOwner) { state ->
                    state.error?.let { VisionState.log("camera error: code ${it.code}") }
                }
                if (torch && cam.cameraInfo.hasFlashUnit()) cam.cameraControl.enableTorch(true)
                imageCapture = capture
                VisionState.log("camera bound")
            } catch (e: Exception) {
                VisionState.log("camera bind failed: ${e.message}")
            }
        }, ContextCompat.getMainExecutor(context))
    }

    /** Releases the camera and recognizers. Call on the main thread. */
    fun stop() {
        imageCapture = null
        provider?.unbindAll()
        textRecognizer.close()
        barcodeScanner.close()
        cameraExecutor.shutdown()
    }

    /**
     * Captures one frame and reads the label. Blocking; never call on the main
     * thread. Any camera or recognition failure gives the spec's "fault" reading.
     */
    fun read(parser: LabelParser, timeoutMs: Long): LabelReading {
        val deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMs)
        fun remainingMs() = TimeUnit.NANOSECONDS.toMillis(deadline - System.nanoTime()).coerceAtLeast(1)

        if (!readLock.tryLock(timeoutMs, TimeUnit.MILLISECONDS)) {
            VisionState.log("read timed out waiting for a previous read")
            return LabelReading.FAULT_READING
        }
        try {
            val capture = imageCapture ?: throw IllegalStateException("camera not ready")
            val bitmap = captureUpright(capture, remainingMs())
            val input = InputImage.fromBitmap(bitmap, 0)
            val textTask = textRecognizer.process(input)
            val barcodeTask = barcodeScanner.process(input)
            val text = Tasks.await(textTask, remainingMs(), TimeUnit.MILLISECONDS)
            val codes = Tasks.await(barcodeTask, remainingMs(), TimeUnit.MILLISECONDS)

            val lines = text.textBlocks.flatMap { it.lines }.mapNotNull { line ->
                line.boundingBox?.let { TextBox(line.text, it.left, it.top, it.right, it.bottom) }
            }
            val barcodes = codes.mapNotNull { code ->
                code.boundingBox?.let { BarcodeBox(code.rawValue, it.left, it.top, it.right, it.bottom) }
            }
            val reading = parser.parse(lines, barcodes)

            val details = StringBuilder(reading.toJson().toString(2))
            details.append("\n\nOCR lines:\n")
            lines.forEach { details.append("  ").append(it.text).append("\n") }
            details.append("Barcodes:\n")
            barcodes.forEach { details.append("  ").append(it.rawValue).append("\n") }
            VisionState.setLastRead(downscale(bitmap, 960), details.toString())
            return reading
        } catch (e: Exception) {
            VisionState.log("read failed: ${e.javaClass.simpleName}: ${e.message}")
            VisionState.setLastRead(null, "fault: ${e.message}")
            return LabelReading.FAULT_READING
        } finally {
            readLock.unlock()
        }
    }

    private fun captureUpright(capture: ImageCapture, timeoutMs: Long): Bitmap {
        val result = CompletableFuture<Bitmap>()
        capture.takePicture(cameraExecutor, object : ImageCapture.OnImageCapturedCallback() {
            override fun onCaptureSuccess(image: ImageProxy) {
                try {
                    result.complete(rotate(image.toBitmap(), image.imageInfo.rotationDegrees))
                } catch (e: Exception) {
                    result.completeExceptionally(e)
                } finally {
                    image.close()
                }
            }

            override fun onError(exception: ImageCaptureException) {
                result.completeExceptionally(exception)
            }
        })
        return result.get(timeoutMs, TimeUnit.MILLISECONDS)
    }

    private fun rotate(bitmap: Bitmap, degrees: Int): Bitmap {
        if (degrees == 0) return bitmap
        val m = Matrix().apply { postRotate(degrees.toFloat()) }
        return Bitmap.createBitmap(bitmap, 0, 0, bitmap.width, bitmap.height, m, true)
    }

    private fun downscale(bitmap: Bitmap, maxSide: Int): Bitmap {
        val scale = maxSide.toFloat() / maxOf(bitmap.width, bitmap.height)
        if (scale >= 1f) return bitmap
        return Bitmap.createScaledBitmap(bitmap, (bitmap.width * scale).toInt(), (bitmap.height * scale).toInt(), true)
    }
}
