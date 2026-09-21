package com.paddlemeter.app.ui

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
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
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.paddlemeter.app.viewmodel.UiState

/**
 * Device selection screen: lists paired Bluetooth devices and lets the user
 * connect to the paddle-meter.
 */
@Composable
fun DeviceScreen(
    state: UiState,
    onConnect: (BluetoothDevice) -> Unit,
    onDisconnect: () -> Unit,
    onContinue: () -> Unit,
) {
    val context = androidx.compose.ui.platform.LocalContext.current
    var paired by remember { mutableStateOf<List<BluetoothDevice>>(emptyList()) }
    var permissionGranted by remember { mutableStateOf(hasBtPermission(context)) }

    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        permissionGranted = result.values.all { it } || hasBtPermission(context)
        if (permissionGranted) paired = loadPairedDevices()
    }

    LaunchedEffect(permissionGranted) {
        if (!permissionGranted) {
            permissionLauncher.launch(requiredBtPermissions())
        } else {
            paired = loadPairedDevices()
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
    ) {
        Text("Paddle Meter", style = MaterialTheme.typography.headlineMedium)
        Spacer(Modifier.height(4.dp))
        Text(
            "Connect to your ESP32 data logger over Bluetooth.",
            style = MaterialTheme.typography.bodyMedium,
        )
        Spacer(Modifier.height(16.dp))

        if (state.connected) {
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp)) {
                    Text("Connected to ${state.deviceName}", style = MaterialTheme.typography.titleMedium)
                    state.status?.let { s ->
                        Spacer(Modifier.height(8.dp))
                        Text("Firmware v${s.version} · ${s.rateHz} Hz · ${s.fileCount} files")
                        Text("Free: ${s.freeKb / 1024} MB · dropped: ${s.dropped}")
                        Text(if (s.recording) "● Recording" else "○ Idle")
                    }
                    Spacer(Modifier.height(12.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(onClick = onContinue) { Text("Manage recordings") }
                        Button(onClick = onDisconnect) { Text("Disconnect") }
                    }
                }
            }
            return@Column
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Paired devices", style = MaterialTheme.typography.titleMedium)
            Button(onClick = { paired = loadPairedDevices() }) {
                Icon(Icons.Default.Refresh, contentDescription = null)
                Spacer(Modifier.height(0.dp))
                Text("Refresh")
            }
        }
        Spacer(Modifier.height(8.dp))

        if (paired.isEmpty()) {
            Text(
                "No paired devices found. Pair 'paddle-meter' in Android Bluetooth settings first.",
                style = MaterialTheme.typography.bodyMedium,
            )
        } else {
            LazyColumn {
                items(paired) { device ->
                    ListItem(
                        headlineContent = { Text(device.name ?: "Unknown") },
                        supportingContent = { Text(device.address) },
                        leadingContent = { Icon(Icons.Default.Bluetooth, contentDescription = null) },
                        modifier = Modifier.clickable(enabled = !state.busy) { onConnect(device) },
                    )
                }
            }
        }

        state.message?.let {
            Spacer(Modifier.height(12.dp))
            Text(it, color = MaterialTheme.colorScheme.primary)
        }
    }
}

private fun requiredBtPermissions(): Array<String> =
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        arrayOf(Manifest.permission.BLUETOOTH_CONNECT)
    } else {
        arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
    }

private fun hasBtPermission(context: android.content.Context): Boolean {
    val perm = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        Manifest.permission.BLUETOOTH_CONNECT
    } else {
        Manifest.permission.ACCESS_FINE_LOCATION
    }
    return context.checkSelfPermission(perm) == android.content.pm.PackageManager.PERMISSION_GRANTED
}

@Suppress("MissingPermission")
private fun loadPairedDevices(): List<BluetoothDevice> {
    val adapter = BluetoothAdapter.getDefaultAdapter() ?: return emptyList()
    return try {
        adapter.bondedDevices?.toList() ?: emptyList()
    } catch (_: SecurityException) {
        emptyList()
    }
}
