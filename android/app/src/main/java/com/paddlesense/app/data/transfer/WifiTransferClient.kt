package com.paddlesense.app.data.transfer

import com.paddlesense.app.data.model.RemoteFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.IOException
import java.io.OutputStream
import java.net.HttpURLConnection
import java.net.URL

/**
 * Minimal HTTP client for the ESP32's WiFi transfer mode (see docs/protocol.md
 * §2.7). Uses [HttpURLConnection] so no extra dependency is required.
 *
 * The device serves:
 *   GET /files            -> "FILE <name> <size>\n" per file, then "OK\n"
 *   GET /files/<name>     -> 200 + Content-Length + raw bytes
 *
 * Deletion is intentionally NOT done here: the repository deletes over
 * Bluetooth after verifying the transfer, preserving the "delete only after a
 * verified transfer" guarantee.
 */
class WifiTransferClient(private val baseUrl: String) {

    private fun open(path: String): HttpURLConnection {
        val url = URL(baseUrl.trimEnd('/') + path)
        return (url.openConnection() as HttpURLConnection).apply {
            connectTimeout = 5000
            readTimeout = 15000
            requestMethod = "GET"
        }
    }

    /** List the recordings available over HTTP. */
    suspend fun list(): List<RemoteFile> = withContext(Dispatchers.IO) {
        val conn = open("/files")
        try {
            if (conn.responseCode != 200) {
                throw IOException("HTTP ${conn.responseCode} listing files")
            }
            val files = ArrayList<RemoteFile>()
            conn.inputStream.bufferedReader().useLines { lines ->
                for (line in lines) {
                    if (line == "OK") break
                    val parts = line.split(' ')
                    if (parts.size >= 3 && parts[0] == "FILE") {
                        files.add(
                            RemoteFile(
                                name = parts[1],
                                sizeBytes = parts[2].toLongOrNull() ?: 0L,
                            )
                        )
                    }
                }
            }
            files
        } finally {
            conn.disconnect()
        }
    }

    /**
     * Download [name] to [out], verifying the byte count against the
     * Content-Length header. [onProgress] receives (received, total).
     */
    suspend fun download(
        name: String,
        out: OutputStream,
        onProgress: (Long, Long) -> Unit = { _, _ -> },
    ): Long = withContext(Dispatchers.IO) {
        val conn = open("/files/$name")
        try {
            if (conn.responseCode != 200) {
                throw IOException("HTTP ${conn.responseCode} downloading $name")
            }
            val total = conn.contentLengthLong
            var received = 0L
            val buf = ByteArray(8192)
            conn.inputStream.use { input ->
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    out.write(buf, 0, n)
                    received += n
                    onProgress(received, if (total > 0) total else 0L)
                }
            }
            out.flush()
            if (total > 0 && received != total) {
                throw IOException("Short read: got $received of $total bytes")
            }
            received
        } finally {
            conn.disconnect()
        }
    }
}
