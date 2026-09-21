package com.paddlesense.app.data.model

/** A recording file present on the ESP32. */
data class RemoteFile(
    val name: String,
    val sizeBytes: Long,
) {
    val sizeLabel: String
        get() = when {
            sizeBytes >= 1_048_576 -> "%.2f MB".format(sizeBytes / 1_048_576.0)
            sizeBytes >= 1024 -> "%.1f KB".format(sizeBytes / 1024.0)
            else -> "$sizeBytes B"
        }
}

/** Device status as reported by the STATUS command. */
data class DeviceStatus(
    val recording: Boolean,
    val rateHz: Int,
    val fileCount: Int,
    val freeKb: Long,
    val dropped: Long,
    val version: String,
    /** AP IP address while WiFi transfer mode is active, else null (v2). */
    val wifiIp: String? = null,
    /** AP SSID while WiFi transfer mode is active, else null (v2). */
    val wifiSsid: String? = null,
) {
    val wifiActive: Boolean get() = !wifiIp.isNullOrEmpty()
}

/** A file that has been downloaded to the phone. */
data class LocalFile(
    val name: String,
    val path: String,
    val sizeBytes: Long,
)

/** Per-file transfer progress for the UI. */
data class TransferProgress(
    val name: String,
    val received: Long,
    val total: Long,
) {
    val fraction: Float
        get() = if (total <= 0) 0f else (received.toFloat() / total).coerceIn(0f, 1f)
}
