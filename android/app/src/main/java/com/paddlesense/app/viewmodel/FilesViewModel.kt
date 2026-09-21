package com.paddlesense.app.viewmodel

import android.app.Application
import android.bluetooth.BluetoothDevice
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.paddlesense.app.data.RecordingRepository
import com.paddlesense.app.data.model.DeviceStatus
import com.paddlesense.app.data.model.LocalFile
import com.paddlesense.app.data.model.RemoteFile
import com.paddlesense.app.data.model.TransferProgress
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/** Immutable UI state. */
data class UiState(
    val connected: Boolean = false,
    val deviceName: String? = null,
    val status: DeviceStatus? = null,
    val remoteFiles: List<RemoteFile> = emptyList(),
    val localFiles: List<LocalFile> = emptyList(),
    val transfer: TransferProgress? = null,
    val autoDelete: Boolean = true,
    val busy: Boolean = false,
    val message: String? = null,
    /** True while a live-tail session is streaming to the phone. */
    val liveTransfer: TransferProgress? = null,
    /** User preference: stream recordings live instead of downloading after STOP. */
    val liveTail: Boolean = false,
    /** HTTP base URL while WiFi transfer mode is active, else null. */
    val wifiBaseUrl: String? = null,
    /** True while a WiFi mode switch is in progress. */
    val wifiBusy: Boolean = false,
)

class FilesViewModel(app: Application) : AndroidViewModel(app) {

    private val repo = RecordingRepository(app)

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    /** Completed by [stopRecording] to end a live-tail session. */
    private var liveStopSignal: CompletableDeferred<Unit>? = null

    fun connect(device: BluetoothDevice) {
        viewModelScope.launch {
            _state.update { it.copy(busy = true, message = "Connecting…") }
            try {
                repo.connect(device)
                _state.update {
                    it.copy(
                        connected = true,
                        deviceName = device.name ?: device.address,
                        busy = false,
                        message = "Connected",
                    )
                }
                refresh()
            } catch (e: Exception) {
                _state.update {
                    it.copy(connected = false, busy = false, message = e.message ?: "Connect failed")
                }
            }
        }
    }

    fun disconnect() {
        viewModelScope.launch {
            repo.disconnect()
            _state.update {
                it.copy(connected = false, deviceName = null, status = null, remoteFiles = emptyList())
            }
        }
    }

    fun refresh() {
        viewModelScope.launch {
            if (!repo.isConnected) return@launch
            _state.update { it.copy(busy = true) }
            try {
                val status = repo.status()
                val files = repo.list()
                val local = repo.localFiles()
                _state.update {
                    it.copy(
                        status = status,
                        remoteFiles = files,
                        localFiles = local,
                        busy = false,
                        message = null,
                    )
                }
            } catch (e: Exception) {
                _state.update { it.copy(busy = false, message = e.message ?: "Refresh failed") }
            }
        }
    }

    fun setLiveTail(enabled: Boolean) {
        _state.update { it.copy(liveTail = enabled) }
    }

    fun startRecording() {
        if (_state.value.liveTail) {
            startLiveRecording()
            return
        }
        viewModelScope.launch {
            runCatching { repo.start() }
                .onSuccess { refresh() }
                .onFailure { e -> _state.update { it.copy(message = e.message) } }
        }
    }

    private fun startLiveRecording() {
        viewModelScope.launch {
            val stopSignal = CompletableDeferred<Unit>()
            liveStopSignal = stopSignal
            _state.update {
                it.copy(
                    liveTransfer = TransferProgress("live", 0, 0),
                    message = "Live streaming…",
                )
            }
            try {
                val file = repo.startLiveAndStream(
                    stopSignal = stopSignal,
                    autoDelete = _state.value.autoDelete,
                ) { received, total ->
                    _state.update {
                        it.copy(liveTransfer = TransferProgress("live", received, total))
                    }
                }
                _state.update {
                    it.copy(
                        liveTransfer = null,
                        message = "Live recording saved: ${file.name}",
                    )
                }
                refresh()
            } catch (e: Exception) {
                _state.update {
                    it.copy(liveTransfer = null, message = "Live stream failed: ${e.message}")
                }
            } finally {
                liveStopSignal = null
            }
        }
    }

    fun stopRecording() {
        // In live mode, completing the signal makes the tail send STOP and wait
        // for the final END + STOPPED; the streaming coroutine handles the rest.
        val signal = liveStopSignal
        if (signal != null) {
            signal.complete(Unit)
            return
        }
        viewModelScope.launch {
            runCatching { repo.stop() }
                .onSuccess { refresh() }
                .onFailure { e -> _state.update { it.copy(message = e.message) } }
        }
    }

    // -----------------------------------------------------------------------
    // WiFi transfer mode
    // -----------------------------------------------------------------------

    fun enableWifi() {
        viewModelScope.launch {
            _state.update { it.copy(wifiBusy = true, message = "Starting WiFi…") }
            runCatching { repo.enableWifi() }
                .onSuccess { url ->
                    _state.update {
                        it.copy(
                            wifiBusy = false,
                            wifiBaseUrl = url,
                            message = if (url != null)
                                "WiFi ready — join the paddlesense network"
                            else "WiFi started but no IP reported",
                        )
                    }
                    refresh()
                }
                .onFailure { e ->
                    _state.update { it.copy(wifiBusy = false, message = "WiFi failed: ${e.message}") }
                }
        }
    }

    fun disableWifi() {
        viewModelScope.launch {
            _state.update { it.copy(wifiBusy = true) }
            runCatching { repo.disableWifi() }
                .onSuccess {
                    _state.update { it.copy(wifiBusy = false, wifiBaseUrl = null, message = "WiFi off") }
                    refresh()
                }
                .onFailure { e ->
                    _state.update { it.copy(wifiBusy = false, message = "WiFi off failed: ${e.message}") }
                }
        }
    }

    fun downloadOverWifi(file: RemoteFile) {
        val baseUrl = _state.value.wifiBaseUrl
        if (baseUrl == null) {
            _state.update { it.copy(message = "Enable WiFi transfer first") }
            return
        }
        viewModelScope.launch {
            val autoDelete = _state.value.autoDelete
            _state.update {
                it.copy(transfer = TransferProgress(file.name, 0, file.sizeBytes), message = null)
            }
            try {
                repo.downloadOverWifi(baseUrl, file, autoDelete) { received, total ->
                    _state.update { it.copy(transfer = TransferProgress(file.name, received, total)) }
                }
                _state.update {
                    it.copy(
                        transfer = null,
                        message = if (autoDelete) "Downloaded via WiFi and deleted on device"
                        else "Downloaded via WiFi",
                    )
                }
                refresh()
            } catch (e: Exception) {
                _state.update {
                    it.copy(transfer = null, message = "WiFi download failed: ${e.message}")
                }
            }
        }
    }

    fun setRate(hz: Int) {
        viewModelScope.launch {
            runCatching { repo.setRate(hz) }
                .onSuccess { refresh() }
                .onFailure { e -> _state.update { it.copy(message = e.message) } }
        }
    }

    fun setAutoDelete(enabled: Boolean) {
        _state.update { it.copy(autoDelete = enabled) }
    }

    fun download(file: RemoteFile) {
        viewModelScope.launch {
            val autoDelete = _state.value.autoDelete
            _state.update {
                it.copy(
                    transfer = TransferProgress(file.name, 0, file.sizeBytes),
                    message = null,
                )
            }
            try {
                repo.downloadAndMaybeDelete(file, autoDelete) { received, total ->
                    _state.update { it.copy(transfer = TransferProgress(file.name, received, total)) }
                }
                _state.update {
                    it.copy(
                        transfer = null,
                        message = if (autoDelete) "Downloaded and deleted on device" else "Downloaded",
                    )
                }
                refresh()
            } catch (e: Exception) {
                _state.update {
                    it.copy(transfer = null, message = "Download failed: ${e.message}")
                }
            }
        }
    }

    fun deleteRemote(file: RemoteFile) {
        viewModelScope.launch {
            runCatching { repo.delete(file.name) }
                .onSuccess { refresh() }
                .onFailure { e -> _state.update { it.copy(message = e.message) } }
        }
    }

    fun clearMessage() {
        _state.update { it.copy(message = null) }
    }
}
