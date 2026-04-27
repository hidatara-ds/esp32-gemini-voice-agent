# ☁️ GABRIEL — AI Voice Assistant for ESP32-S3

**Gabriel** adalah asisten suara pintar berbasis AI yang menggunakan ESP32-S3 untuk mendengarkan perintah suara, memprosesnya melalui Google Gemini, dan memberikan jawaban baik dalam bentuk teks di layar OLED maupun suara melalui speaker.

> Proyek ini cocok untuk kamu yang ingin membangun **AI assistant fisik**: bisa mendengar, berpikir, menampilkan teks, dan berbicara secara natural.

---

## 🚀 Fitur Utama
- **Always Listening**: Menggunakan VAD (Voice Activity Detection) untuk mendeteksi suara secara otomatis.
- **AI Intelligence**: Didukung oleh **Gemini 2.5 Flash** untuk jawaban yang cepat dan cerdas.
- **Natural Voice**: Output suara jernih menggunakan Google Text-to-Speech (Wavenet).
- **Visual Display**: Interface rapi pada layar OLED SSD1306.
- **Modular Build**: Komponen mudah dicari dan mudah di-upgrade.

---

## 🛠️ Kebutuhan Hardware

| Komponen | Deskripsi |
| :--- | :--- |
| **Microcontroller** | ESP32-S3 (N16R8) |
| **Microphone** | INMP441 (I2S Digital Mic) |
| **Speaker Amp** | MAX98357A (I2S Class-D Amp) |
| **Display** | OLED SSD1306 128x64 (I2C) |

### 💰 Estimasi Biaya Komponen

> Estimasi ini membantu kamu menghitung budget awal sebelum mulai rakit.

| Komponen | Harga (IDR) | Estimasi (USD) |
| :--- | ---: | ---: |
| MAX98357A | Rp 45.000 | $2.81 |
| Kit Modul OLED 128 x 65 | Rp 55.500 | $3.47 |
| ESP32-S3 N16R8 | Rp 115.000 | $7.19 |
| Speaker 8 ohm 3 watt | Rp 15.000 | $0.94 |
| INMP441 | Rp 60.000 | $3.75 |
| **Total** | **Rp 290.500** | **$18.16** |

**Asumsi kurs:** 1 USD = Rp 16.000 (estimasi, bisa berubah mengikuti kurs harian).

### Skema Wiring (Pinout)

#### 🎙️ Mikrofon (INMP441)
- **SCK** -> GPIO 5
- **WS** -> GPIO 4
- **SD** -> GPIO 6
- **L/R** -> GND
- **VDD** -> 3.3V
- **GND** -> GND

#### 🔊 Speaker (MAX98357A)
- **BCLK** -> GPIO 40
- **LRC** -> GPIO 41
- **DIN** -> GPIO 42
- **VIN** -> 5V (Disarankan)
- **GND** -> GND

#### 📺 Display (OLED SSD1306)
- **SDA** -> GPIO 8
- **SCL** -> GPIO 9
- **VCC** -> 3.3V
- **GND** -> GND

---

## ⚙️ Persiapan Software

### 1. Backend (Python)
Backend berfungsi sebagai jembatan antara ESP32 dan Google Cloud Services.

**Prasyarat:**
- Python 3.9+
- Google Cloud Project dengan API berikut aktif:
  - Vertex AI API
  - Cloud Speech-to-Text API
  - Cloud Text-to-Speech API
- Service Account Key (`key.json`)

**Instalasi:**
```bash
# Clone repository
git clone <url-repo>
cd firmware-gabriel-buddy

# Install dependencies
pip install -r requirements.txt

# Set environment variables (Linux/Mac)
export GOOGLE_APPLICATION_CREDENTIALS="path/to/your/key.json"
export GOOGLE_CLOUD_PROJECT_ID="your-project-id"

# Jalankan server
python app.py
```

### 2. Firmware (Arduino)
**Library yang dibutuhkan:**
- `ArduinoJson` (by Benoit Blanchon)
- `U8g2` (by oliver)

**Konfigurasi:**
Buka file `gabriel/config.h` dan sesuaikan pengaturan berikut:
```cpp
#define WIFI_SSID "Nama_WiFi_Kamu"
#define WIFI_PASSWORD "Password_WiFi_Kamu"
#define API_BASE_URL "https://url-backend-kamu.com"
```

**Upload:**
1. Pilih Board: **ESP32S3 Dev Module**.
2. Pastikan **PSRAM** diaktifkan (OPI atau QSPI sesuai board kamu).
3. Klik **Upload**.

---

## 📖 Cara Penggunaan
1. Hubungkan ESP32 ke sumber daya.
2. Tunggu hingga layar menampilkan pesan **"Standby"**.
3. Mulailah berbicara. Gabriel akan mendeteksi suara secara otomatis.
4. Layar akan menunjukkan **"Processing..."** saat mengirim data ke server.
5. Gabriel akan menampilkan jawaban di layar dan mengucapkannya melalui speaker.

---

## 📝 Catatan Penting
- **VAD Threshold**: Jika Gabriel terlalu sensitif atau sulit mendeteksi suara, sesuaikan `VAD_THRESHOLD` di `config.h`.
- **Power**: Gunakan power supply yang stabil. Noise pada power dapat mengganggu kualitas input mikrofon I2S.
- **Latency**: Kecepatan respon sangat bergantung pada koneksi internet dan region server Google Cloud yang digunakan.

---

## 📄 Lisensi
Copyright © 2025. All Rights Reserved. PROPRIETARY AND CONFIDENTIAL.
