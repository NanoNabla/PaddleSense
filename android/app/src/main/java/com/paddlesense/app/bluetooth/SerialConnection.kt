package com.paddlesense.app.bluetooth

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID

/**
 * Thin wrapper around a Bluetooth Classic RFCOMM (SPP) socket.
 *
 * All I/O is performed on [Dispatchers.IO]. The ESP32 only ever replies to
 * commands, so callers can use the simple request/response helpers below
 * without a background reader thread.
 */
class SerialConnection {

    companion object {
        /** Well-known SPP UUID. */
        val SPP_UUID: UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")
    }

    private var socket: BluetoothSocket? = null
    private var input: InputStream? = null
    private var output: OutputStream? = null

    val isConnected: Boolean
        get() = socket?.isConnected == true

    /**
     * Connect to [device]. Throws [IOException] on failure.
     * Must be called from a coroutine (suspends on IO).
     */
    @SuppressLint("MissingPermission")
    suspend fun connect(device: BluetoothDevice) = withContext(Dispatchers.IO) {
        disconnect()
        // Cancel discovery first: it slows down / can block connection setup.
        try {
            BluetoothAdapter.getDefaultAdapter()?.cancelDiscovery()
        } catch (_: SecurityException) {
            // ignore
        }

        val sock = device.createRfcommSocketToServiceRecord(SPP_UUID)
        try {
            sock.connect()
        } catch (e: IOException) {
            try { sock.close() } catch (_: IOException) {}
            throw IOException("Could not connect to ${device.name ?: device.address}", e)
        }

        socket = sock
        input = BufferedInputStream(sock.inputStream, 8192)
        output = BufferedOutputStream(sock.outputStream, 8192)
    }

    suspend fun disconnect() = withContext(Dispatchers.IO) {
        try { input?.close() } catch (_: IOException) {}
        try { output?.close() } catch (_: IOException) {}
        try { socket?.close() } catch (_: IOException) {}
        input = null
        output = null
        socket = null
    }

    /** Write a line terminated with '\n' and flush. */
    suspend fun writeLine(line: String) = withContext(Dispatchers.IO) {
        val out = output ?: throw IOException("Not connected")
        out.write(line.toByteArray(Charsets.US_ASCII))
        out.write('\n'.code)
        out.flush()
    }

    /**
     * Read a single '\n'-terminated line (without the terminator).
     * Returns null on EOF / disconnect.
     */
    suspend fun readLine(): String? = withContext(Dispatchers.IO) {
        val inp = input ?: throw IOException("Not connected")
        val sb = StringBuilder(64)
        while (true) {
            val b = inp.read()
            if (b == -1) return@withContext if (sb.isEmpty()) null else sb.toString()
            if (b == '\n'.code) return@withContext sb.toString()
            if (b == '\r'.code) continue
            sb.append(b.toChar())
        }
        @Suppress("UNREACHABLE_CODE") null
    }

    /**
     * Read exactly [count] bytes into [sink]. Returns the number of bytes read
     * (may be less than [count] only on EOF).
     */
    suspend fun readFully(count: Long, sink: (ByteArray, Int) -> Unit): Long =
        withContext(Dispatchers.IO) {
            val inp = input ?: throw IOException("Not connected")
            val buf = ByteArray(8192)
            var remaining = count
            while (remaining > 0) {
                val want = minOf(buf.size.toLong(), remaining).toInt()
                val got = inp.read(buf, 0, want)
                if (got == -1) break
                sink(buf, got)
                remaining -= got
            }
            count - remaining
        }
}
