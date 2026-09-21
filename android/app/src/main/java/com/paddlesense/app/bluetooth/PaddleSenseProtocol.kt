package com.paddlesense.app.bluetooth

import com.paddlesense.app.data.model.DeviceStatus
import com.paddlesense.app.data.model.RemoteFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.IOException
import java.io.OutputStream
import java.util.zip.CRC32

/**
 * Client implementation of the paddlesense line protocol (see
 * docs/architecture.md §2.5). One command is in flight at a time.
 */
class PaddleSenseProtocol(private val conn: SerialConnection) {

    /** Thrown when the device answers with an `ERR ...` line. */
    class ProtocolException(message: String) : IOException(message)

    private suspend fun command(cmd: String): String {
        conn.writeLine(cmd)
        val line = conn.readLine() ?: throw IOException("Device disconnected")
        if (line.startsWith("ERR ")) {
            throw ProtocolException(line.removePrefix("ERR ").trim())
        }
        return line
    }

    suspend fun ping(): Boolean = try {
        command("PING") == "PONG"
    } catch (_: IOException) {
        false
    }

    suspend fun status(): DeviceStatus {
        val line = command("STATUS")
        // STATUS recording=0 rate=200 files=3 free_kb=2713600 dropped=0 version=1
        val map = line.removePrefix("STATUS").trim()
            .split(' ')
            .mapNotNull { part ->
                val i = part.indexOf('=')
                if (i <= 0) null else part.substring(0, i) to part.substring(i + 1)
            }
            .toMap()
        return DeviceStatus(
            recording = map["recording"] == "1",
            rateHz = map["rate"]?.toIntOrNull() ?: 0,
            fileCount = map["files"]?.toIntOrNull() ?: 0,
            freeKb = map["free_kb"]?.toLongOrNull() ?: 0L,
            dropped = map["dropped"]?.toLongOrNull() ?: 0L,
            version = map["version"] ?: "?",
        )
    }

    suspend fun list(): List<RemoteFile> {
        val header = command("LIST")
        if (!header.startsWith("FILES ")) {
            throw ProtocolException("Unexpected LIST reply: $header")
        }
        val count = header.removePrefix("FILES ").trim().toIntOrNull() ?: 0
        val files = ArrayList<RemoteFile>(count)
        repeat(count) {
            val line = conn.readLine() ?: throw IOException("Device disconnected")
            // FILE ps_0001.csv 48210
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
        val ok = conn.readLine() ?: throw IOException("Device disconnected")
        if (ok != "OK") {
            throw ProtocolException("LIST not terminated: $ok")
        }
        return files
    }

    suspend fun start(): String = command("START")
    suspend fun stop(): String = command("STOP")

    suspend fun setRate(hz: Int): Int {
        val line = command("RATE $hz")
        return line.removePrefix("RATE ").trim().toIntOrNull() ?: hz
    }

    suspend fun delete(name: String): String = command("DEL $name")

    /**
     * Download [name] to [out]. Verifies the byte count and CRC-32 reported by
     * the device. [onProgress] receives (bytesReceived, totalBytes).
     *
     * Returns the number of bytes written. Throws on any mismatch so the caller
     * can avoid deleting a corrupt transfer.
     */
    suspend fun download(
        name: String,
        out: OutputStream,
        onProgress: (Long, Long) -> Unit = { _, _ -> },
    ): Long = withContext(Dispatchers.IO) {
        conn.writeLine("GET $name")
        val begin = conn.readLine() ?: throw IOException("Device disconnected")
        if (begin.startsWith("ERR ")) {
            throw ProtocolException(begin.removePrefix("ERR ").trim())
        }
        if (!begin.startsWith("BEGIN ")) {
            throw ProtocolException("Unexpected GET reply: $begin")
        }
        val total = begin.removePrefix("BEGIN ").trim().toLongOrNull()
            ?: throw ProtocolException("Bad size in: $begin")

        val crc = CRC32()
        var received = 0L
        val got = conn.readFully(total) { buf, len ->
            out.write(buf, 0, len)
            crc.update(buf, 0, len)
            received += len
            onProgress(received, total)
        }
        out.flush()

        if (got != total) {
            throw IOException("Short read: got $got of $total bytes")
        }

        val end = conn.readLine() ?: throw IOException("Device disconnected")
        if (!end.startsWith("END ")) {
            throw ProtocolException("Missing END marker: $end")
        }
        val remoteCrc = end.removePrefix("END ").trim().toLongOrNull(16)
            ?: throw ProtocolException("Bad CRC in: $end")
        if (remoteCrc == 0L && total > 0) {
            throw IOException("Device reported a failed transfer")
        }
        if (remoteCrc != crc.value) {
            throw IOException(
                "CRC mismatch: local=%08X remote=%08X".format(crc.value, remoteCrc)
            )
        }
        received
    }
}
