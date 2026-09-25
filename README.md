# PaddleSense

An ESP32 data logger that samples an MPU-6050 at high rate, stores time series
on an SD card, and serves them to an Android app that lists, downloads, verifies
and then deletes the recordings. Transfer happens over **Bluetooth Classic
(SPP)** — with an optional **live-tail** stream during recording — or over an
on-demand **WiFi softAP + HTTP** mode for fast bulk downloads.

```
ESP32 + MPU-6050 + SD card  ──Bluetooth Classic SPP──►  Android app
   (sampling + storage)      ──WiFi softAP + HTTP────►  (list/download/delete)
```

See [`docs/architecture.md`](docs/architecture.md) for the full design and
[`docs/protocol.md`](docs/protocol.md) for the wire protocol and file format
(the contract shared by the firmware and the app).

---

## Repository layout

```
paddlesense/
├── firmware/     PlatformIO project (ESP32, Arduino framework)
├── android/      Android Studio project (Kotlin, Jetpack Compose)
├── branding/     Source artwork + launcher-icon generator
├── tools/        Host-side helper scripts (CSV conversion)
└── docs/         Architecture document
```

The Android launcher icons are generated from
[`branding/PaddleSense.png`](branding/PaddleSense.png) by
[`branding/generate_android_icons.py`](branding/generate_android_icons.py)
(requires Pillow). Re-run it after changing the artwork:

```bash
python3 branding/generate_android_icons.py
```

---

## 1. Hardware

| Component | ESP32 pin |
|---|---|
| MPU-6050 SDA | GPIO 21 |
| MPU-6050 SCL | GPIO 22 |
| MPU-6050 VCC | 3V3 |
| MPU-6050 GND | GND |
| MPU-6050 AD0 | GND (address 0x68) |
| SD CS | GPIO 5 |
| SD SCK | GPIO 18 |
| SD MOSI | GPIO 23 |
| SD MISO | GPIO 19 |
| SD VCC | 5V (module has onboard regulator) |
| Status LED | GPIO 2 (onboard) |

- Use a **3.3 V-compatible** MPU-6050 module (most breakout boards are).
- Format the SD card as **FAT32**.
- The MPU-6050 has no magnetometer, so the firmware logs raw acceleration and
  angular rate; orientation/processing is done later on the phone.

---

## 2. Firmware

### Build & flash

```bash
cd firmware
pio run                 # build
pio run -t upload       # flash
pio device monitor      # 115200 baud serial log
```

Requires [PlatformIO](https://platformio.org/) (VS Code extension or CLI).
No external libraries are needed — `BluetoothSerial`, `SD`, `Wire` and
`Preferences` ship with the ESP32 Arduino core.

### Behavior

- Samples the MPU-6050 at **200 Hz** by default (configurable 50–1000 Hz).
- Writes CSV files to `/data/ps_0001.csv`, `ps_0002.csv`, … (index kept in NVS).
- Status LED: **solid** = recording, **slow blink** = idle, **fast blink** = error.
- Recording is started/stopped from the phone (`START` / `STOP`).

### File format

```
# paddlesense v2 rate=200 arange=4g grange=500dps
t_us,ax,ay,az,gx,gy,gz
1234567,0.1234,-9.8012,0.4321,1.20,-0.50,3.10
...
# samples=41230 dropped=0
```

Acceleration in m/s², angular rate in deg/s, `t_us` = microseconds since boot.

### Protocol (Bluetooth SPP, line-based ASCII)

| Command | Reply |
|---|---|
| `PING` | `PONG` |
| `STATUS` | `STATUS recording=0 rate=200 files=3 free_kb=2713600 dropped=0 version=2 wifi=0 ip=- ssid=-` |
| `LIST` | `FILES n`, then `FILE <name> <size>` ×n, then `OK` |
| `GET <name>` | `BEGIN <size>`, raw bytes, `END <crc32>` |
| `TAIL <name>` | `BEGIN ?`, then `DATA <n>` + n bytes ×n, then `END <crc32>` (live stream) |
| `ENDTAIL` | `END ABORT` |
| `MODE wifi` | `MODE wifi ssid=paddlesense ip=192.168.4.1` |
| `MODE legacy` | `MODE legacy` |
| `DEL <name>` | `DELETED` |
| `START` | `STARTED ps_0005.csv` |
| `STOP` | `STOPPED` |
| `RATE <hz>` | `RATE <applied>` |

Errors are reported as `ERR <reason>`. The CRC-32 uses the zlib polynomial and
matches `java.util.zip.CRC32` on the phone.

### WiFi transfer mode

`MODE wifi` brings up a softAP (`paddlesense` / `paddlesense`, overridable via
`/config.txt` keys `wifi_ssid` / `wifi_pass`) and a small HTTP server. The
Bluetooth device name is likewise configurable via the `/config.txt` key
`bt_name` (default `paddlesense`):

| Endpoint | Response |
|---|---|
| `GET /files` | `FILE <name> <size>` lines, then `OK` |
| `GET /files/<name>` | `200 OK` + `Content-Length` + raw bytes |
| `GET /status` | JSON status document |

Bluetooth Classic and WiFi share the same 2.4 GHz radio, so the app treats the
modes as mutually exclusive: use WiFi for bulk transfer, Bluetooth for control
and live tail. Deletion still happens over Bluetooth after a verified transfer.

---

## 3. Android app

### Build & run

1. Open the `android/` folder in Android Studio (Giraffe or newer).
2. Let Gradle sync (it will download the Compose BOM and AndroidX deps).
3. Run on a physical device (Bluetooth Classic is not available on emulators).

`minSdk 26`, `targetSdk 34`, Kotlin + Jetpack Compose (Material 3).

Prefer VS Code? See [`docs/vscode-android.md`](docs/vscode-android.md) for a
ready-made setup (recommended extensions, build/install/debug tasks, and the
toolchain install steps).

### Usage

1. Pair the ESP32 (`paddlesense`) in Android Bluetooth settings.
2. Open the app, grant the Bluetooth permission, tap the device to connect.
3. Tap **Manage recordings**:
   - **Start / Stop** a recording session.
   - **Set rate** (50–1000 Hz) for the next session.
   - **Live stream while recording** — when enabled, the recording is streamed
     to the phone as it is written, so there is nothing left to download at
     STOP. The CRC-32 is verified at the end.
   - Tap the download icon on a file — progress is shown, and on success the
     file is verified (size + CRC-32) and then **deleted on the device**
     (toggle "Delete on device after download" to keep it).
   - **Enable WiFi transfer** — brings up the ESP32 access point; join it in
     Android WiFi settings, then downloads use HTTP (~100× faster). The
     download icon automatically uses WiFi while it is enabled.
   - Downloaded files are listed under "Downloaded to phone" and stored in the
     app-specific external files directory
     (`Android/data/com.paddlesense.app/files/Documents/paddlesense`).

### Extending with data processing

Downloaded CSVs are parsed by
[`TimeSeriesReader`](android/app/src/main/java/com/paddlesense/app/data/timeseries/TimeSeriesReader.kt),
which returns a `TimeSeries` (metadata + `List<TimeSeriesSample>`). Add your
processing pipeline on top of that type — the UI and transfer layers do not need
to change.

---

## 4. Helper tools

[`tools/paddlesense_csv.py`](tools/paddlesense_csv.py) is a dependency-free Python 3
utility that converts recordings in both directions:

```bash
# paddlesense recording -> readable CSV (index, seconds, vector magnitudes)
python3 tools/paddlesense_csv.py to-csv ps_0001.csv -o ps_0001.readable.csv

# readable CSV (or any CSV with named columns) -> paddlesense recording
python3 tools/paddlesense_csv.py to-paddlesense ps_0001.readable.csv -o ps_0001.csv

# auto-detect the direction
python3 tools/paddlesense_csv.py auto somefile.csv -o out.csv
```

The round-trip is lossless (metadata is preserved as `#` comments). See
[`tools/README.md`](tools/README.md) for the full option reference and run the
tests with `python3 -m unittest discover -s tools/tests`.

---

## 5. Notes & limitations

- Bluetooth Classic SPP throughput is roughly 10–50 KB/s, so multi-MB files
  take a few minutes over Bluetooth. Use **WiFi transfer mode** for large files
  (or **live stream** to avoid the post-session wait entirely).
- Bluetooth Classic and WiFi share one 2.4 GHz radio; do not run a WiFi
  download and a Bluetooth live tail at the same time.
- The WiFi stack + HTTP server grow the firmware image, so the build uses the
  `huge_app.csv` partition (3 MB app, no OTA).
- FAT32 limits a single file to 4 GB (≈ 8+ hours at 200 Hz).
- If the SD card stalls, the ring buffer drops the oldest samples; the count is
  reported in `STATUS` and written into the file footer.
- Deletion on the device only ever happens after a phone-verified transfer, so a
  dropped link never loses data.
