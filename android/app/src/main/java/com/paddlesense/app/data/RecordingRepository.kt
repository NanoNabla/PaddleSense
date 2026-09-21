package com.paddlesense.app.data

import android.bluetooth.BluetoothDevice
import android.content.Context
import android.os.Environment
import com.paddlesense.app.bluetooth.PaddleSenseProtocol
import com.paddlesense.app.bluetooth.SerialConnection
import com.paddlesense.app.data.model.DeviceStatus
import com.paddlesense.app.data.model.LocalFile
import com.paddlesense.app.data.model.RemoteFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileOutputStream

/**
 * Coordinates the Bluetooth connection, the protocol client and local storage.
 *
 * Downloads are written to the app-specific external files directory
 * (no storage permission required, still user-visible over USB / Files app).
 * A file is deleted on the device only after a verified transfer.
 */
class RecordingRepository(private val context: Context) {

    private val connection = SerialConnection()
    private val protocol = PaddleSenseProtocol(connection)

    val isConnected: Boolean get() = connection.isConnected

    /** Directory where downloaded recordings are stored. */
    val downloadDir: File
        get() = File(
            context.getExternalFilesDir(Environment.DIRECTORY_DOCUMENTS),
            "paddlesense"
        ).apply { if (!exists()) mkdirs() }

    suspend fun connect(device: BluetoothDevice) = connection.connect(device)

    suspend fun disconnect() = connection.disconnect()

    suspend fun status(): DeviceStatus = protocol.status()

    suspend fun list(): List<RemoteFile> = protocol.list()

    suspend fun start(): String = protocol.start()

    suspend fun stop(): String = protocol.stop()

    suspend fun setRate(hz: Int): Int = protocol.setRate(hz)

    suspend fun delete(name: String): String = protocol.delete(name)

    /**
     * Download [file] and, on success, delete it on the device.
     *
     * @param autoDelete delete the remote file after a verified transfer
     * @param onProgress (received, total) callback
     * @return the local [File]
     */
    suspend fun downloadAndMaybeDelete(
        file: RemoteFile,
        autoDelete: Boolean,
        onProgress: (Long, Long) -> Unit,
    ): File = withContext(Dispatchers.IO) {
        val target = File(downloadDir, file.name)
        val tmp = File(downloadDir, "${file.name}.part")

        try {
            FileOutputStream(tmp).use { out ->
                protocol.download(file.name, out, onProgress)
            }
            // Atomic-ish replace: only expose the file once fully verified.
            if (target.exists()) target.delete()
            if (!tmp.renameTo(target)) {
                tmp.copyTo(target, overwrite = true)
                tmp.delete()
            }
        } catch (e: Exception) {
            tmp.delete()
            throw e
        }

        if (autoDelete) {
            protocol.delete(file.name)
        }
        target
    }

    /** List recordings already downloaded to this phone. */
    suspend fun localFiles(): List<LocalFile> = withContext(Dispatchers.IO) {
        downloadDir.listFiles()
            ?.filter { it.isFile && it.name.endsWith(".csv") }
            ?.map { LocalFile(it.name, it.absolutePath, it.length()) }
            ?.sortedBy { it.name }
            ?: emptyList()
    }
}
