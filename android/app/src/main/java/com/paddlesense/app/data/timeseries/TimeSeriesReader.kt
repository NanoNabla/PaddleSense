package com.paddlesense.app.data.timeseries

import java.io.File

/**
 * One parsed sample from a downloaded CSV recording.
 * Units match the firmware: acceleration in m/s^2, angular rate in deg/s.
 */
data class TimeSeriesSample(
    val tUs: Long,
    val ax: Float,
    val ay: Float,
    val az: Float,
    val gx: Float,
    val gy: Float,
    val gz: Float,
)

/** Metadata parsed from the file header/footer. */
data class TimeSeriesMeta(
    val version: String = "?",
    val rateHz: Int = 0,
    val accelRangeG: Int = 0,
    val gyroRangeDps: Int = 0,
    val sampleCount: Long = 0,
    val dropped: Long = 0,
)

data class TimeSeries(
    val meta: TimeSeriesMeta,
    val samples: List<TimeSeriesSample>,
)

/**
 * Parses paddlesense CSV recordings.
 *
 * This is the extension point for the future on-phone processing feature:
 * downstream code can consume [TimeSeries] without knowing the file format.
 */
object TimeSeriesReader {

    /**
     * Read and parse [file]. Lines starting with '#' are treated as metadata
     * (header and footer); the first non-comment line is the column header.
     */
    fun read(file: File): TimeSeries {
        var meta = TimeSeriesMeta()
        val samples = ArrayList<TimeSeriesSample>(4096)
        var sawHeader = false

        file.forEachLine { raw ->
            val line = raw.trim()
            if (line.isEmpty()) return@forEachLine

            if (line.startsWith("#")) {
                meta = parseMetaLine(line, meta)
                return@forEachLine
            }
            if (!sawHeader) {
                // Column header: t_us,ax,ay,az,gx,gy,gz
                sawHeader = true
                return@forEachLine
            }

            val parts = line.split(',')
            if (parts.size < 7) return@forEachLine
            val t = parts[0].toLongOrNull() ?: return@forEachLine
            samples.add(
                TimeSeriesSample(
                    tUs = t,
                    ax = parts[1].toFloatOrNull() ?: 0f,
                    ay = parts[2].toFloatOrNull() ?: 0f,
                    az = parts[3].toFloatOrNull() ?: 0f,
                    gx = parts[4].toFloatOrNull() ?: 0f,
                    gy = parts[5].toFloatOrNull() ?: 0f,
                    gz = parts[6].toFloatOrNull() ?: 0f,
                )
            )
        }

        return TimeSeries(meta = meta, samples = samples)
    }

    private fun parseMetaLine(line: String, current: TimeSeriesMeta): TimeSeriesMeta {
        // "# paddlesense v1 rate=200 arange=4g grange=500dps"
        if (line.contains("paddlesense")) {
            val tokens = line.removePrefix("#").trim().split(' ')
            var version = current.version
            var rate = current.rateHz
            var arange = current.accelRangeG
            var grange = current.gyroRangeDps
            for (tok in tokens) {
                when {
                    tok.startsWith("v") -> version = tok.removePrefix("v")
                    tok.startsWith("rate=") -> rate = tok.removePrefix("rate=").toIntOrNull() ?: rate
                    tok.startsWith("arange=") ->
                        arange = tok.removePrefix("arange=").removeSuffix("g").toIntOrNull() ?: arange
                    tok.startsWith("grange=") ->
                        grange = tok.removePrefix("grange=").removeSuffix("dps").toIntOrNull() ?: grange
                }
            }
            return current.copy(
                version = version,
                rateHz = rate,
                accelRangeG = arange,
                gyroRangeDps = grange,
            )
        }
        // "# samples=41230 dropped=0"
        if (line.contains("samples=")) {
            var count = current.sampleCount
            var dropped = current.dropped
            for (tok in line.removePrefix("#").trim().split(' ')) {
                when {
                    tok.startsWith("samples=") ->
                        count = tok.removePrefix("samples=").toLongOrNull() ?: count
                    tok.startsWith("dropped=") ->
                        dropped = tok.removePrefix("dropped=").toLongOrNull() ?: dropped
                }
            }
            return current.copy(sampleCount = count, dropped = dropped)
        }
        return current
    }
}
