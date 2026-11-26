package ca.mcmaster.sorting.vision

import fi.iki.elonen.NanoHTTPD
import org.json.JSONException
import org.json.JSONObject

/**
 * HTTP + JSON mapping of the Vision interfaces (project_context.md section 5).
 * Only 3.1 "read part label" is implemented; 3.2 (part position) is not.
 *
 *   POST /vision/read-part-label   body: {}  (input is void; unknown fields are ignored)
 *   200 {"reading_result": "...", "origin": "...", "destination": "...", "part_id": "..."}
 */
class VisionHttpServer(port: Int, private val readLabel: () -> LabelReading) : NanoHTTPD(port) {

    override fun serve(session: IHTTPSession): Response {
        val path = session.uri.trimEnd('/')
        val client = session.remoteIpAddress
        if (path != READ_PART_LABEL) {
            VisionState.log("$client ${session.method} ${session.uri} -> 404")
            return error(Response.Status.NOT_FOUND, "unknown path")
        }
        if (session.method != Method.POST) {
            return error(Response.Status.METHOD_NOT_ALLOWED, "use POST")
        }

        // Input is void, but the body must still be read and be valid JSON if present.
        val files = HashMap<String, String>()
        try {
            session.parseBody(files)
            val body = files["postData"]
            if (!body.isNullOrBlank()) JSONObject(body)
        } catch (e: JSONException) {
            VisionState.log("$client POST $path -> 400 malformed JSON")
            return error(Response.Status.BAD_REQUEST, "malformed JSON")
        } catch (e: Exception) {
            return error(Response.Status.BAD_REQUEST, e.message ?: "bad request")
        }

        VisionState.log("$client POST $path")
        val started = System.currentTimeMillis()
        val reading = try {
            readLabel()
        } catch (e: Exception) {
            VisionState.log("read crashed: ${e.message}")
            LabelReading.FAULT_READING
        }
        val json = reading.toJson().toString()
        VisionState.log("-> $json (${System.currentTimeMillis() - started} ms, part ${reading.partId})")
        return newFixedLengthResponse(Response.Status.OK, JSON, json)
    }

    private fun error(status: Response.Status, message: String): Response =
        newFixedLengthResponse(status, JSON, JSONObject().put("error", message).toString())

    companion object {
        const val READ_PART_LABEL = "/vision/read-part-label"
        private const val JSON = "application/json; charset=utf-8"
    }
}
