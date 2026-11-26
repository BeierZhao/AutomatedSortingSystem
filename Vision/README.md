# Vision (Android)

Android app that implements the Vision interface **3.1 read part label** of the
Interface Specification (`../README.md`). It reads the labels with the phone's
back camera, ML Kit OCR and ML Kit barcode scanning, and serves the result
over HTTP to the controller (ESP32). Interface 3.2 (get part position) is not
implemented.

## HTTP API

Follows the mapping in `../project_context.md` section 5.

```
POST http://<phone-ip>:8080/vision/read-part-label
Content-Type: application/json

{}
```

Response (always HTTP 200 when the request itself was valid):

```json
{"reading_result": "success", "origin": "Hamilton", "destination": "Toronto", "part_id": "345678"}
```

| Field | Values |
| --- | --- |
| `reading_result` | `"success"`, `"info lost"`, `"none"`, `"fault"` |
| `origin`, `destination` | City name as read, `"none"` (label found, value blank), `"fault"` (label not found) |
| `part_id` | int32 as a string, `"none"` (label found, no valid barcode), `"fault"` (label not found) |

Transport errors: `404` unknown path, `405` not POST, `400` body is not valid
JSON (an empty body is accepted). Unknown JSON fields are ignored.

Each request triggers one capture. The request blocks until recognition ends
(typically under 1 s) and is bounded by the read timeout (default 8 s). If the
camera, OCR or barcode scanner fails or times out, the reply is
`"fault"` in all four fields. Concurrent requests are served one after the
other. The controller should use an HTTP timeout longer than the read timeout.

Test from a laptop on the same network:

```powershell
curl.exe -X POST -H "Content-Type: application/json" -d "{}" http://<phone-ip>:8080/vision/read-part-label
```

## Label reading rules

The label is a small table (see the reference image in the project brief):

```
Origin:       Hamilton
Destination:  Toronto
Part ID:                   <- blank text cell
|||| ||| |||| ||           <- barcode = part ID
     345678
```

- **Origin / Destination**: OCR finds the key text (`Origin`, `Destination`,
  tolerant to small OCR errors like `0rigin`, `Destlnation`). The value is the
  text after the colon on the same OCR line, or else the nearest text to the
  right on the same row. A key without a value gives `"none"`; a missing key
  gives `"fault"`.
- **City snapping** (setting, on by default): a value within a small edit
  distance of a known city is replaced by that city's spelling. For example
  the label text `Missisauga` becomes `Mississauga`, so it matches the
  controller's position config. Edit the known-city list in the app to match
  the config, or turn snapping off to return the raw OCR text. Unknown cities
  are returned as read.
- **Part ID**: comes from the barcode only (1D formats: Code 128/39/93,
  Codabar, ITF, EAN, UPC). The largest barcode in view is used. If its
  content is not an int32, or there is a `Part ID` key but no barcode, the
  result is `"none"`. No barcode and no key gives `"fault"`.
- **reading_result**: `"success"` if all three fields have values, `"none"` if
  all three are `"fault"` (no part in view), otherwise `"info lost"`.
- If several labels are in view, the largest (nearest) one is used.

The parsing logic is in `LabelParser.kt` and has no Android dependencies;
`LabelParserTest.kt` covers the rules above.

## Build and install

Requirements: Android Studio (Koala or newer) with Android SDK 34, a phone
running Android 8.0 (API 26) or later.

1. Open the `Vision` folder in Android Studio and let Gradle sync.
2. Connect the phone with USB debugging on and press **Run**, or build with
   `.\gradlew.bat assembleDebug` and install
   `app/build/outputs/apk/debug/app-debug.apk`.
3. Run the unit tests with `.\gradlew.bat test`.

## Operating the phone

1. Open **Sorting Vision** and grant the camera (and notification) permission.
   The server starts automatically and the screen shows its URL.
2. Press **Battery optimization…** and set the app to *Unrestricted / Don't
   optimize*, so Android does not stop the server.
3. Mount the phone above the read position and press **Test read**. The last
   captured frame, the JSON result and the raw OCR lines are shown, so you can
   adjust the aim, distance and lighting (turn on **Torch** if it is dark).
4. Give the phone a fixed IP (DHCP reservation) and set that address in the
   ESP32 firmware.

The camera and server run in a foreground service (persistent notification)
and hold a wake lock and a Wi-Fi lock, so they keep working with the screen
off. The app screen itself keeps the display on while open.

Settings (saved on the phone): HTTP port (default 8080), read timeout, known
cities, city snapping, torch. Saving restarts the service.

Every request and reply is logged with a timestamp on screen and in logcat
(tag `Vision`).

## Source layout

| File | Role |
| --- | --- |
| `LabelParser.kt` | OCR lines + barcodes → spec outputs (pure Kotlin, unit tested) |
| `LabelReader.kt` | CameraX capture, ML Kit OCR and barcode scan, timeouts |
| `VisionHttpServer.kt` | NanoHTTPD server, `POST /vision/read-part-label` |
| `VisionService.kt` | Foreground service: camera, server, wake/Wi-Fi locks |
| `MainActivity.kt` | Status, settings, test read, last capture, log |
| `VisionSettings.kt` / `VisionState.kt` | Saved settings / shared UI state and log |
