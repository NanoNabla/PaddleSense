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
)

class FilesViewModel(app: Application) : AndroidViewModel(app) {

    private val repo = RecordingRepository(app)

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

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

    fun startRecording() {
        viewModelScope.launch {
            runCatching { repo.start() }
                .onSuccess { refresh() }
                .onFailure { e -> _state.update { it.copy(message = e.message) } }
        }
    }

    fun stopRecording() {
        viewModelScope.launch {
            runCatching { repo.stop() }
                .onSuccess { refresh() }
                .onFailure { e -> _state.update { it.copy(message = e.message) } }
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
