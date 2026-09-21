package com.paddlemeter.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.paddlemeter.app.data.model.RemoteFile
import com.paddlemeter.app.viewmodel.UiState

/**
 * Recording management screen: start/stop, list remote files, download with
 * progress, and delete. Downloaded files are listed at the bottom.
 */
@Composable
fun FilesScreen(
    state: UiState,
    onRefresh: () -> Unit,
    onStart: () -> Unit,
    onStop: () -> Unit,
    onSetRate: (Int) -> Unit,
    onDownload: (RemoteFile) -> Unit,
    onDelete: (RemoteFile) -> Unit,
    onAutoDeleteChange: (Boolean) -> Unit,
    onBack: () -> Unit,
) {
    var showRateDialog by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Recordings", style = MaterialTheme.typography.headlineSmall)
            Row {
                IconButton(onClick = onRefresh) {
                    Icon(Icons.Default.Refresh, contentDescription = "Refresh")
                }
                TextButton(onClick = onBack) { Text("Devices") }
            }
        }

        // Status + controls
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                val s = state.status
                Text(
                    if (s?.recording == true) "● Recording at ${s.rateHz} Hz"
                    else "○ Idle${s?.let { " · ${it.rateHz} Hz" } ?: ""}",
                    style = MaterialTheme.typography.titleMedium,
                )
                s?.let {
                    Text("${it.fileCount} files on device · ${it.freeKb / 1024} MB free · dropped ${it.dropped}")
                }
                Spacer(Modifier.height(12.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (s?.recording == true) {
                        Button(onClick = onStop) {
                            Icon(Icons.Default.Stop, contentDescription = null)
                            Text(" Stop")
                        }
                    } else {
                        Button(onClick = onStart) {
                            Icon(Icons.Default.PlayArrow, contentDescription = null)
                            Text(" Start")
                        }
                    }
                    OutlinedButton(onClick = { showRateDialog = true }) { Text("Set rate") }
                }
                Spacer(Modifier.height(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Switch(checked = state.autoDelete, onCheckedChange = onAutoDeleteChange)
                    Spacer(Modifier.height(0.dp))
                    Text("  Delete on device after download")
                }
            }
        }

        Spacer(Modifier.height(12.dp))

        // Active transfer progress
        state.transfer?.let { t ->
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp)) {
                    Text("Downloading ${t.name}")
                    Spacer(Modifier.height(8.dp))
                    LinearProgressIndicator(
                        progress = { t.fraction },
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Spacer(Modifier.height(4.dp))
                    Text("${t.received} / ${t.total} bytes")
                }
            }
            Spacer(Modifier.height(12.dp))
        }

        state.message?.let {
            Text(it, color = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.height(8.dp))
        }

        Text("On device", style = MaterialTheme.typography.titleMedium)
        if (state.remoteFiles.isEmpty()) {
            Text("No recordings on the device.", style = MaterialTheme.typography.bodyMedium)
        } else {
            LazyColumn(modifier = Modifier.weight(1f)) {
                items(state.remoteFiles, key = { it.name }) { file ->
                    ListItem(
                        headlineContent = { Text(file.name) },
                        supportingContent = { Text(file.sizeLabel) },
                        trailingContent = {
                            Row {
                                IconButton(
                                    onClick = { onDownload(file) },
                                    enabled = state.transfer == null,
                                ) {
                                    Icon(Icons.Default.Download, contentDescription = "Download")
                                }
                                IconButton(
                                    onClick = { onDelete(file) },
                                    enabled = state.transfer == null,
                                ) {
                                    Icon(Icons.Default.Delete, contentDescription = "Delete")
                                }
                            }
                        },
                    )
                }
            }
        }

        if (state.localFiles.isNotEmpty()) {
            Spacer(Modifier.height(8.dp))
            Text("Downloaded to phone", style = MaterialTheme.typography.titleMedium)
            LazyColumn(modifier = Modifier.weight(1f)) {
                items(state.localFiles, key = { it.path }) { file ->
                    ListItem(
                        headlineContent = { Text(file.name) },
                        supportingContent = { Text("${file.sizeBytes} bytes") },
                    )
                }
            }
        }
    }

    if (showRateDialog) {
        RateDialog(
            current = state.status?.rateHz ?: 200,
            onDismiss = { showRateDialog = false },
            onConfirm = {
                onSetRate(it)
                showRateDialog = false
            },
        )
    }
}

@Composable
private fun RateDialog(
    current: Int,
    onDismiss: () -> Unit,
    onConfirm: (Int) -> Unit,
) {
    val options = listOf(50, 100, 200, 500, 1000)
    var selected by remember { mutableStateOf(current) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Sampling rate") },
        text = {
            Column {
                Text("Applies to the next recording session.")
                Spacer(Modifier.height(8.dp))
                options.forEach { hz ->
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        androidx.compose.material3.RadioButton(
                            selected = selected == hz,
                            onClick = { selected = hz },
                        )
                        Text("$hz Hz")
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = { onConfirm(selected) }) { Text("Apply") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}
