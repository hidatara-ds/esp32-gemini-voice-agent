# Gabriel AI Ecosystem: Cloud Backend & Edge ESP32 Firmware

**Recommended GitHub topics:** `cloud-run`, `gemini`, `websocket`, `esp32`, `voice-ai`, `flask-socketio`, `iot`, `speech-to-text`, `text-to-speech`, `embedded-ai`

![Python](https://img.shields.io/badge/Python-3.10%2B-3776AB?logo=python&logoColor=white)
![C++](https://img.shields.io/badge/C%2B%2B-ESP32%20Firmware-00599C?logo=cplusplus&logoColor=white)
![GCP](https://img.shields.io/badge/GCP-Cloud%20Run-4285F4?logo=googlecloud&logoColor=white)
![ESP32](https://img.shields.io/badge/ESP32-S3-E7352C?logo=espressif&logoColor=white)
![Flask](https://img.shields.io/badge/Flask-Backend-000000?logo=flask&logoColor=white)
![WebSocket](https://img.shields.io/badge/Protocol-WebSocket-010101?logo=socketdotio&logoColor=white)

---

## English

### Executive Summary
Gabriel AI Ecosystem is an end-to-end voice assistant stack with two cohesive systems:
- `gabriel` (cloud backend): Python + Flask-SocketIO service, deployed on Google Cloud Run.
- `firmware-gabriel-buddy` (edge firmware): C++ firmware for ESP32-S3 with local VAD, I2S audio capture, and OLED UI.

System flow:
1. ESP32-S3 continuously monitors microphone input with VAD.
2. Voice segments are sent to cloud endpoints over WebSocket and/or REST.
3. Cloud backend performs STT, sends user intent to Gemini 2.5 Flash, and generates TTS.
4. Response text/audio is streamed back to device.
5. ESP32 renders text on OLED and plays audio through I2S amplifier.

### Security Notice (Critical)
- Never hardcode real URLs, credentials, API keys, or Wi-Fi passwords in source code.
- Use placeholders such as:
  - `https://your-cloud-run-service.a.run.app`
  - `YOUR_GEMINI_API_KEY`
  - `<WIFI_PASSWORD>`
- Create your own `.env` file locally and **do not commit it**.
- Add `.env` and credential files to `.gitignore` in every repo.

### Repository Structure (Typical)
```text
.
|-- cloud-api/
|   `-- gabriel/                 # Cloud backend (Flask-SocketIO + Cloud Run)
`-- firmware-gabriel-buddy/
    `-- gabriel/                 # ESP32-S3 firmware project
```

### Hardware Requirements
| Component | Purpose |
| --- | --- |
| ESP32-S3 Dev Board (N16R8 or equivalent) | Main edge controller |
| INMP441 I2S microphone | Voice capture |
| MAX98357A I2S amplifier + speaker | Audio playback |
| SSD1306 OLED (I2C, 128x64) | On-device text/status UI |
| Stable 5V/USB power | Noise-safe operation |

### Wiring & Physical Setup
#### INMP441 microphone (requires **6 jumper wires**)
| INMP441 Pin | ESP32-S3 Pin |
| --- | --- |
| `SCK` | `GPIO5` |
| `WS` | `GPIO4` |
| `SD` | `GPIO6` |
| `L/R` | `GND` |
| `VDD` | `3.3V` |
| `GND` | `GND` |

#### MAX98357A amplifier
| MAX98357A Pin | ESP32-S3 Pin |
| --- | --- |
| `BCLK` | `GPIO40` |
| `LRC` | `GPIO41` |
| `DIN` | `GPIO42` |
| `VIN` | `5V` (recommended) |
| `GND` | `GND` |

#### SSD1306 OLED display
| OLED Pin | ESP32-S3 Pin |
| --- | --- |
| `SDA` | `GPIO8` |
| `SCL` | `GPIO9` |
| `VCC` | `3.3V` |
| `GND` | `GND` |

**Mechanical constraint:** the OLED module cannot be mounted exactly at the chassis edge due to physical frame clearance. Keep a small inset/offset from the edge to avoid stress on PCB and jumper cables.

---

### Beginner-Friendly Setup
### 1) Backend (`gabriel`) Setup
#### Prerequisites
- Python 3.10+
- Docker Desktop (or Docker Engine)
- Google Cloud SDK (`gcloud`)
- A GCP project with APIs enabled:
  - Vertex AI API
  - Speech-to-Text API
  - Text-to-Speech API
  - Cloud Run Admin API
  - Artifact Registry API

#### Step-by-step
```bash
# 1) Go to backend folder (example)
cd "cloud-api/gabriel"

# 2) Create virtual environment
python -m venv .venv

# 3) Activate venv (PowerShell)
.venv\Scripts\Activate.ps1

# 4) Install dependencies
pip install -r requirements.txt

# 5) Create local env file (DO NOT COMMIT)
# File: .env
```

Example `.env` template:
```env
FLASK_ENV=production
PORT=8080
GOOGLE_CLOUD_PROJECT_ID=your-gcp-project-id
GOOGLE_APPLICATION_CREDENTIALS=./secrets/service-account.json
GEMINI_API_KEY=YOUR_GEMINI_API_KEY
API_BASE_URL=https://your-cloud-run-service.a.run.app
```

Run locally:
```bash
python app.py
```

### 2) ESP32-S3 Firmware Setup
#### Prerequisites
- Arduino IDE 2.x (or PlatformIO)
- ESP32 board package installed
- USB data cable
- Required libraries:
  - ArduinoJson
  - U8g2

#### Step-by-step
1. Open firmware project (example path): `firmware-gabriel-buddy/gabriel`.
2. Update configuration placeholders in firmware config file:
   - `WIFI_SSID="<WIFI_SSID>"`
   - `WIFI_PASSWORD="<WIFI_PASSWORD>"`
   - `API_BASE_URL="https://your-cloud-run-service.a.run.app"`
3. In Arduino IDE:
   - Board: `ESP32S3 Dev Module`
   - PSRAM: enabled (`OPI` or `QSPI` per board)
4. Connect ESP32-S3 via USB.
5. Click Upload, then open Serial Monitor to verify Wi-Fi + backend connection.

---

### API Reference (Sanitized)
Gabriel uses a dual-interface model: REST for transactional calls and WebSocket for streaming/interactive sessions.

#### REST API (example)
Base URL:
```text
https://your-cloud-run-service.a.run.app
```

Example endpoint:
```text
POST /api/v1/assistant/query
```

Example request JSON:
```json
{
  "device_id": "esp32s3-01",
  "session_id": "session-abc123",
  "language": "id-ID",
  "input_text": "What is the weather today?",
  "audio_format": "pcm16",
  "sample_rate": 16000
}
```

Example response JSON:
```json
{
  "status": "ok",
  "reply_text": "Today's weather is sunny with a chance of rain.",
  "reply_audio_url": "https://your-cloud-run-service.a.run.app/media/reply-001.wav",
  "latency_ms": 1240,
  "trace_id": "trace-xyz789"
}
```

#### WebSocket Events (example)
WebSocket URL:
```text
wss://your-cloud-run-service.a.run.app/socket.io/?EIO=4&transport=websocket
```

Core events:
- `connect`: device session successfully connected.
- `audio_stream`: device sends real-time/base64 audio chunks.
- `transcript_partial`: backend emits incremental STT text.
- `transcript_final`: backend emits final STT text.
- `response_text`: backend emits Gemini-generated text.
- `response_audio`: backend emits/announces synthesized TTS audio.
- `error`: backend reports recoverable/non-recoverable errors.
- `disconnect`: session closed.

Example `audio_stream` payload:
```json
{
  "device_id": "esp32s3-01",
  "sequence": 128,
  "audio_base64": "<BASE64_PCM_CHUNK>",
  "sample_rate": 16000,
  "channels": 1
}
```

---

### Deployment Guide
### A) Deploy backend to Cloud Run (Dockerfile)
```bash
cd "cloud-api/gabriel"

# Authenticate and select project
gcloud auth login
gcloud config set project your-gcp-project-id

# Build and push image with Cloud Build
gcloud builds submit --tag gcr.io/your-gcp-project-id/gabriel-backend

# Deploy to Cloud Run
gcloud run deploy gabriel-backend ^
  --image gcr.io/your-gcp-project-id/gabriel-backend ^
  --platform managed ^
  --region asia-southeast2 ^
  --allow-unauthenticated ^
  --port 8080 ^
  --set-env-vars GEMINI_API_KEY=YOUR_GEMINI_API_KEY
```

After deployment, update firmware `API_BASE_URL` with:
`https://your-cloud-run-service.a.run.app`

### B) Flash firmware to ESP32-S3
1. Verify wiring (especially 6-wire INMP441 mapping).
2. Compile and upload firmware from Arduino IDE/PlatformIO.
3. Reboot board and monitor serial logs.
4. Confirm OLED status changes: idle -> listening -> processing -> response.

---

### Operations & Troubleshooting
- If VAD is too sensitive or misses speech, tune VAD threshold in firmware config.
- If audio is noisy, validate grounding and use stable 5V power for amp.
- If latency is high, check Wi-Fi RSSI, Cloud Run region, and STT/TTS response times.
- If WebSocket reconnect loops occur, verify `API_BASE_URL`, TLS (`wss`), and backend CORS/socket config.

### License
Copyright © 2026. All Rights Reserved.

---

## Bahasa Indonesia

### Ringkasan Eksekutif
Gabriel AI Ecosystem adalah stack asisten suara end-to-end yang terdiri dari dua sistem terintegrasi:
- `gabriel` (backend cloud): layanan Python + Flask-SocketIO yang di-deploy ke Google Cloud Run.
- `firmware-gabriel-buddy` (firmware edge): firmware C++ untuk ESP32-S3 dengan VAD lokal, perekaman audio I2S, dan antarmuka OLED.

Alur sistem:
1. ESP32-S3 memantau input mikrofon secara kontinu menggunakan VAD.
2. Segmen suara dikirim ke endpoint cloud melalui WebSocket dan/atau REST.
3. Backend melakukan STT, meneruskan intent ke Gemini 2.5 Flash, lalu menghasilkan TTS.
4. Respons teks/audio dikirim kembali ke perangkat.
5. ESP32 menampilkan teks di OLED dan memutar audio melalui amplifier I2S.

### Peringatan Keamanan (Wajib)
- Jangan pernah menaruh URL asli, kredensial, API key, atau password Wi-Fi nyata di source code.
- Gunakan placeholder yang jelas:
  - `https://your-cloud-run-service.a.run.app`
  - `YOUR_GEMINI_API_KEY`
  - `<WIFI_PASSWORD>`
- Buat file `.env` sendiri di lokal dan **jangan di-commit**.
- Pastikan `.env` serta file kredensial masuk ke `.gitignore`.

### Struktur Repositori (Contoh)
```text
.
|-- cloud-api/
|   `-- gabriel/                 # Backend cloud (Flask-SocketIO + Cloud Run)
`-- firmware-gabriel-buddy/
    `-- gabriel/                 # Proyek firmware ESP32-S3
```

### Kebutuhan Hardware
| Komponen | Fungsi |
| --- | --- |
| ESP32-S3 Dev Board (N16R8 atau setara) | Kontroler edge utama |
| Mikrofon I2S INMP441 | Penangkap suara |
| MAX98357A + speaker | Output audio |
| OLED SSD1306 (I2C, 128x64) | UI teks/status di perangkat |
| Catu daya 5V/USB stabil | Operasi rendah noise |

### Wiring & Setup Fisik
#### Mikrofon INMP441 (wajib **6 kabel jumper**)
| Pin INMP441 | Pin ESP32-S3 |
| --- | --- |
| `SCK` | `GPIO5` |
| `WS` | `GPIO4` |
| `SD` | `GPIO6` |
| `L/R` | `GND` |
| `VDD` | `3.3V` |
| `GND` | `GND` |

#### Amplifier MAX98357A
| Pin MAX98357A | Pin ESP32-S3 |
| --- | --- |
| `BCLK` | `GPIO40` |
| `LRC` | `GPIO41` |
| `DIN` | `GPIO42` |
| `VIN` | `5V` (disarankan) |
| `GND` | `GND` |

#### Layar OLED SSD1306
| Pin OLED | Pin ESP32-S3 |
| --- | --- |
| `SDA` | `GPIO8` |
| `SCL` | `GPIO9` |
| `VCC` | `3.3V` |
| `GND` | `GND` |

**Batasan mekanik:** modul OLED tidak bisa ditempatkan tepat di pinggir chassis karena keterbatasan ruang frame. Sisakan offset kecil dari tepi agar PCB dan kabel tidak tertekan.

---

### Instalasi Ramah Pemula
### 1) Setup Backend (`gabriel`)
#### Prasyarat
- Python 3.10+
- Docker Desktop (atau Docker Engine)
- Google Cloud SDK (`gcloud`)
- Project GCP dengan API aktif:
  - Vertex AI API
  - Speech-to-Text API
  - Text-to-Speech API
  - Cloud Run Admin API
  - Artifact Registry API

#### Langkah
```bash
# 1) Masuk folder backend (contoh)
cd "cloud-api/gabriel"

# 2) Buat virtual environment
python -m venv .venv

# 3) Aktifkan venv (PowerShell)
.venv\Scripts\Activate.ps1

# 4) Install dependency
pip install -r requirements.txt

# 5) Buat file env lokal (JANGAN DI-COMMIT)
# File: .env
```

Contoh template `.env`:
```env
FLASK_ENV=production
PORT=8080
GOOGLE_CLOUD_PROJECT_ID=your-gcp-project-id
GOOGLE_APPLICATION_CREDENTIALS=./secrets/service-account.json
GEMINI_API_KEY=YOUR_GEMINI_API_KEY
API_BASE_URL=https://your-cloud-run-service.a.run.app
```

Jalankan lokal:
```bash
python app.py
```

### 2) Setup Firmware ESP32-S3
#### Prasyarat
- Arduino IDE 2.x (atau PlatformIO)
- ESP32 board package terpasang
- Kabel USB data
- Library:
  - ArduinoJson
  - U8g2

#### Langkah
1. Buka proyek firmware (contoh): `firmware-gabriel-buddy/gabriel`.
2. Ubah placeholder konfigurasi di file config firmware:
   - `WIFI_SSID="<WIFI_SSID>"`
   - `WIFI_PASSWORD="<WIFI_PASSWORD>"`
   - `API_BASE_URL="https://your-cloud-run-service.a.run.app"`
3. Di Arduino IDE:
   - Board: `ESP32S3 Dev Module`
   - PSRAM: aktif (`OPI` atau `QSPI` sesuai board)
4. Sambungkan ESP32-S3 ke USB.
5. Klik Upload, lalu cek Serial Monitor untuk validasi koneksi Wi-Fi + backend.

---

### Referensi API (Disanitasi)
Arsitektur komunikasi menggunakan dua antarmuka: REST untuk request transaksional dan WebSocket untuk sesi streaming/interaktif.

#### REST API (contoh)
Base URL:
```text
https://your-cloud-run-service.a.run.app
```

Contoh endpoint:
```text
POST /api/v1/assistant/query
```

Contoh request JSON:
```json
{
  "device_id": "esp32s3-01",
  "session_id": "session-abc123",
  "language": "id-ID",
  "input_text": "Cuaca hari ini bagaimana?",
  "audio_format": "pcm16",
  "sample_rate": 16000
}
```

Contoh response JSON:
```json
{
  "status": "ok",
  "reply_text": "Hari ini cerah dengan kemungkinan hujan ringan.",
  "reply_audio_url": "https://your-cloud-run-service.a.run.app/media/reply-001.wav",
  "latency_ms": 1240,
  "trace_id": "trace-xyz789"
}
```

#### Event WebSocket (contoh)
URL WebSocket:
```text
wss://your-cloud-run-service.a.run.app/socket.io/?EIO=4&transport=websocket
```

Event inti:
- `connect`: sesi perangkat berhasil tersambung.
- `audio_stream`: perangkat mengirim potongan audio real-time/base64.
- `transcript_partial`: backend mengirim hasil STT parsial.
- `transcript_final`: backend mengirim hasil STT final.
- `response_text`: backend mengirim teks jawaban dari Gemini.
- `response_audio`: backend mengirim/menginformasikan audio TTS.
- `error`: backend mengirim error yang perlu ditangani.
- `disconnect`: sesi ditutup.

Contoh payload `audio_stream`:
```json
{
  "device_id": "esp32s3-01",
  "sequence": 128,
  "audio_base64": "<BASE64_PCM_CHUNK>",
  "sample_rate": 16000,
  "channels": 1
}
```

---

### Panduan Deployment
### A) Deploy backend ke Cloud Run (Dockerfile)
```bash
cd "cloud-api/gabriel"

# Login dan set project
gcloud auth login
gcloud config set project your-gcp-project-id

# Build + push image via Cloud Build
gcloud builds submit --tag gcr.io/your-gcp-project-id/gabriel-backend

# Deploy ke Cloud Run
gcloud run deploy gabriel-backend ^
  --image gcr.io/your-gcp-project-id/gabriel-backend ^
  --platform managed ^
  --region asia-southeast2 ^
  --allow-unauthenticated ^
  --port 8080 ^
  --set-env-vars GEMINI_API_KEY=YOUR_GEMINI_API_KEY
```

Setelah deploy, update `API_BASE_URL` firmware menjadi:
`https://your-cloud-run-service.a.run.app`

### B) Flash firmware ke ESP32-S3
1. Verifikasi wiring (terutama mapping 6 kabel INMP441).
2. Compile dan upload firmware via Arduino IDE/PlatformIO.
3. Reboot board dan pantau serial log.
4. Pastikan status OLED berjalan: idle -> listening -> processing -> response.

---

### Operasional & Troubleshooting
- Jika VAD terlalu sensitif atau kurang peka, sesuaikan threshold VAD di config firmware.
- Jika audio bising, cek grounding dan pastikan catu 5V amplifier stabil.
- Jika latensi tinggi, cek kekuatan Wi-Fi, region Cloud Run, dan waktu proses STT/TTS.
- Jika WebSocket reconnect terus, cek `API_BASE_URL`, TLS (`wss`), dan konfigurasi CORS/socket backend.

### Lisensi
Copyright © 2026. All Rights Reserved.
