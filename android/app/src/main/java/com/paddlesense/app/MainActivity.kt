package com.paddlesense.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.viewModels
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import com.paddlesense.app.ui.DeviceScreen
import com.paddlesense.app.ui.FilesScreen
import com.paddlesense.app.ui.theme.PaddleSenseTheme
import com.paddlesense.app.viewmodel.FilesViewModel

class MainActivity : ComponentActivity() {

    private val viewModel: FilesViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            PaddleSenseTheme {
                val state by viewModel.state.collectAsState()
                var showFiles by remember { mutableStateOf(false) }

                if (showFiles && state.connected) {
                    FilesScreen(
                        state = state,
                        onRefresh = viewModel::refresh,
                        onStart = viewModel::startRecording,
                        onStop = viewModel::stopRecording,
                        onSetRate = viewModel::setRate,
                        onDownload = viewModel::download,
                        onDelete = viewModel::deleteRemote,
                        onAutoDeleteChange = viewModel::setAutoDelete,
                        onBack = { showFiles = false },
                    )
                } else {
                    DeviceScreen(
                        state = state,
                        onConnect = viewModel::connect,
                        onDisconnect = viewModel::disconnect,
                        onContinue = { showFiles = true },
                    )
                }
            }
        }
    }
}
