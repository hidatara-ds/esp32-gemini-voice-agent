// ============================================================
// ☁️ GABRIEL — Cloud Messenger for ESP32-S3
// Fetch AI-generated messages and display on OLED
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>   // Native WS — supports binary frames
#include <U8g2lib.h>
#include <Wire.h>
#include <esp_system.h>
#include <driver/i2s_std.h>
#include <mbedtls/base64.h>
#include "config.h"

// ── App States ──────────────────────────────────────────────
enum AppState {
    STATE_STANDBY,
    STATE_RECORDING,
    STATE_PROCESSING,
    STATE_DISPLAY
};

// ── I2S Mic Config ──────────────────────────────────────────
#define I2S_PORT I2S_NUM_0
#ifndef RECORD_TIME
#define RECORD_TIME 4 // max 4 seconds
#endif
#ifndef MIN_UPLOAD_PEAK
#define MIN_UPLOAD_PEAK 300
#endif
#ifndef MIC_DIAG_ON_BOOT
#define MIC_DIAG_ON_BOOT true
#endif
#ifndef MIC_DIAG_DURATION_MS
#define MIC_DIAG_DURATION_MS 3000
#endif
#ifndef VAD_START_HITS
#define VAD_START_HITS 4
#endif
#ifndef VAD_STOP_THRESHOLD
#define VAD_STOP_THRESHOLD 60
#endif
#ifndef VAD_SILENCE_MS
#define VAD_SILENCE_MS 700
#endif
#ifndef MIN_RECORD_MS
#define MIN_RECORD_MS 700
#endif
#ifndef I2S_SAMPLE_SHIFT
#define I2S_SAMPLE_SHIFT 14
#endif
#ifndef INMP441_USE_LEFT
#define INMP441_USE_LEFT true
#endif
#ifndef SPEAKER_VOLUME_PERCENT
#define SPEAKER_VOLUME_PERCENT 95
#endif
#ifndef SPEAKER_GAIN_PERCENT
#define SPEAKER_GAIN_PERCENT 130
#endif
#ifndef AUDIO_STREAM_IDLE_TIMEOUT_MS
#define AUDIO_STREAM_IDLE_TIMEOUT_MS 1200
#endif
const size_t RECORD_SIZE = I2S_SAMPLE_RATE * 2 * RECORD_TIME;
uint8_t* audioBuffer = nullptr;
size_t audioDataSize = 0;
i2s_chan_handle_t rxChan = nullptr;
i2s_chan_handle_t txChan = nullptr;
String pendingAudioUrl = "";

// ── Ticker + Speaking Animation ─────────────────────────────
String tickerText    = "";    // answer text scrolling below face
int    tickerOffset  = 0;     // pixels scrolled (increases over time)
unsigned long lastTickerMs = 0;
int speakingFrame = 0;        // 0-3 for mouth open/close animation

// ── Streaming audio queue ─────────────────────────────────────
#define AUDIO_QUEUE_SIZE 8
String audioUrlQueue[AUDIO_QUEUE_SIZE];
int    audioQueueHead = 0;
int    audioQueueTail = 0;
bool   wsStreamDone   = false;   // server sent "done" packet

// ── WebSocket (raw binary, Socket.IO-like protocol) ──────────
WebSocketsClient wsClient;
bool wsConnected   = false;
bool wsHandshook   = false;   // received 'connected' event from server
String wsSessionId = "";
uint8_t wsSidBuf[24] = {0};    // Engine.IO sid buffer

inline int16_t convertI2SSampleToPCM16(int32_t sample32) {
    int32_t s = sample32 >> I2S_SAMPLE_SHIFT;
    if (s > 32767)  s =  32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

void writeWavHeader(uint8_t* header, uint32_t pcmDataSize, uint32_t sampleRate) {
    const uint16_t numChannels = 1;
    const uint16_t bitsPerSample = 16;
    const uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    const uint16_t blockAlign = numChannels * (bitsPerSample / 8);
    const uint32_t chunkSize = 36 + pcmDataSize;

    memcpy(header + 0, "RIFF", 4);
    header[4] = chunkSize & 0xFF;
    header[5] = (chunkSize >> 8) & 0xFF;
    header[6] = (chunkSize >> 16) & 0xFF;
    header[7] = (chunkSize >> 24) & 0xFF;
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0; // Subchunk1Size (PCM)
    header[20] = 1; header[21] = 0; // AudioFormat (PCM)
    header[22] = numChannels & 0xFF;
    header[23] = (numChannels >> 8) & 0xFF;
    header[24] = sampleRate & 0xFF;
    header[25] = (sampleRate >> 8) & 0xFF;
    header[26] = (sampleRate >> 16) & 0xFF;
    header[27] = (sampleRate >> 24) & 0xFF;
    header[28] = byteRate & 0xFF;
    header[29] = (byteRate >> 8) & 0xFF;
    header[30] = (byteRate >> 16) & 0xFF;
    header[31] = (byteRate >> 24) & 0xFF;
    header[32] = blockAlign & 0xFF;
    header[33] = (blockAlign >> 8) & 0xFF;
    header[34] = bitsPerSample & 0xFF;
    header[35] = (bitsPerSample >> 8) & 0xFF;
    memcpy(header + 36, "data", 4);
    header[40] = pcmDataSize & 0xFF;
    header[41] = (pcmDataSize >> 8) & 0xFF;
    header[42] = (pcmDataSize >> 16) & 0xFF;
    header[43] = (pcmDataSize >> 24) & 0xFF;
}

bool base64Encode(const uint8_t* data, size_t dataLen, String& out) {
    size_t encodedLen = 0;
    int rc = mbedtls_base64_encode(nullptr, 0, &encodedLen, data, dataLen);
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || encodedLen == 0) {
        return false;
    }

    unsigned char* encoded = (unsigned char*)malloc(encodedLen + 1);
    if (!encoded) return false;

    rc = mbedtls_base64_encode(encoded, encodedLen, &encodedLen, data, dataLen);
    if (rc != 0) {
        free(encoded);
        return false;
    }

    encoded[encodedLen] = '\0';
    out = String((char*)encoded);
    free(encoded);
    return true;
}

String extractJsonStringField(const String& json, const char* key) {
    String token = "\"";
    token += key;
    token += "\":\"";
    int start = json.indexOf(token);
    if (start < 0) return "";
    start += token.length();

    String out;
    out.reserve(128);
    bool escape = false;
    for (int i = start; i < json.length(); i++) {
        char c = json[i];
        if (escape) {
            switch (c) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                default: out += c; break;
            }
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') {
            break;
        }
        out += c;
    }
    return out;
}

void flushI2SInput() {
    if (rxChan == nullptr) return;

    uint8_t flushBuf[512];
    size_t bytesRead = 0;
    while (i2s_channel_read(rxChan, flushBuf, sizeof(flushBuf), &bytesRead, 0) == ESP_OK && bytesRead > 0) {
        // Drain stale samples from RX FIFO/DMA.
    }
}

void runMicDiagnostic(uint32_t durationMs) {
    if (rxChan == nullptr) {
        Serial.println("[MIC] Diagnostic skipped: rxChan null");
        return;
    }

    Serial.printf("[MIC] Diagnostic start: %u ms\n", (unsigned int)durationMs);
    uint32_t startMs = millis();
    uint64_t sumAbs = 0;
    uint32_t totalSamples = 0;
    int peak = 0;
    uint32_t nonZeroCount = 0;
    uint32_t nearClipCount = 0;

    while ((millis() - startMs) < durationMs) {
        int32_t chunk[256];
        size_t bytesRead = 0;
        i2s_channel_read(rxChan, chunk, sizeof(chunk), &bytesRead, 50 / portTICK_PERIOD_MS);
        size_t sampleCount = bytesRead / sizeof(int32_t);
        for (size_t i = 0; i < sampleCount; i++) {
            int level = abs(convertI2SSampleToPCM16(chunk[i]));
            sumAbs += level;
            totalSamples++;
            if (level > peak) peak = level;
            if (level > 8) nonZeroCount++;
            if (level > 30000) nearClipCount++;
        }
    }

    int avgAbs = (totalSamples > 0) ? (int)(sumAbs / totalSamples) : 0;
    float nonZeroPct = (totalSamples > 0) ? (100.0f * nonZeroCount / totalSamples) : 0.0f;
    float clipPct = (totalSamples > 0) ? (100.0f * nearClipCount / totalSamples) : 0.0f;

    Serial.printf("[MIC] Samples=%u avgAbs=%d peak=%d nonZero=%.1f%% nearClip=%.2f%%\n",
                  (unsigned int)totalSamples, avgAbs, peak, nonZeroPct, clipPct);

    if (totalSamples < 100) {
        Serial.println("[MIC] FAIL: almost no samples read. Check I2S pins/clock.");
    } else if (peak < 100 || avgAbs < 20) {
        Serial.println("[MIC] FAIL: signal too low/flat. Check mic wiring, VDD/GND, SD pin.");
    } else if (clipPct > 20.0f) {
        Serial.println("[MIC] WARN: signal clipping heavily. Lower gain / check scaling.");
    } else {
        Serial.println("[MIC] OK: microphone signal detected.");
    }
}

// ── OLED Display (SSD1306 128x64, I2C) ─────────────────────
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

// ── State ───────────────────────────────────────────────────
char currentMessage[MAX_MSG_LENGTH] = "";
char currentCategory[16] = "";
char currentEmoji[8] = "";
int fetchCount = 0;
int errorCount = 0;
bool wifiConnected = false;
unsigned long recordStartMs = 0;
unsigned long lastRecordProgressLogMs = 0;
int recordPeakLevel = 0;
int vadHitCount = 0;
unsigned long silenceStartMs = 0;


AppState currentState = STATE_STANDBY;
unsigned long stateStartTime = 0;

// ── Display Layout Constants ────────────────────────────────
#define HEADER_HEIGHT   14
#define FOOTER_HEIGHT   10
#define BODY_Y_START    (HEADER_HEIGHT + 2)
#define BODY_HEIGHT     (64 - HEADER_HEIGHT - FOOTER_HEIGHT - 2)
#define CHAR_WIDTH      6
#define LINE_HEIGHT     10
#define MAX_CHARS_LINE  21  // 128 / 6 = 21 chars per line

enum FaceMood {
    FACE_IDLE,
    FACE_LISTENING,
    FACE_PROCESSING,
    FACE_SPEAKING,
};

void drawFaceAndTicker(FaceMood mood, int speakFrame);
void updateFaceAnimation(FaceMood mood, bool forceDraw);

unsigned long nextBlinkMs = 0;
unsigned long blinkEndMs = 0;
unsigned long lastFaceDrawMs = 0;
bool eyesClosed = false;

// ═══════════════════════════════════════════════════════════
// BOOT ANIMATION
// ═══════════════════════════════════════════════════════════

void showBootAnimation() {
    // Frame 1: Logo fade-in
    for (int brightness = 0; brightness <= 255; brightness += 15) {
        display.setContrast(brightness);
        display.clearBuffer();
        display.setFont(u8g2_font_helvB12_tr);
        
        int textWidth = display.getStrWidth("GABRIEL");
        int x = (128 - textWidth) / 2;
        display.drawStr(x, 28, "GABRIEL");
        
        display.setFont(u8g2_font_5x7_tr);
        const char* subtitle = "Cloud Messenger";
        int subWidth = display.getStrWidth(subtitle);
        display.drawStr((128 - subWidth) / 2, 44, subtitle);
        
        display.sendBuffer();
        delay(20);
    }
    delay(800);

    // Frame 2: Cloud icon animation
    display.clearBuffer();
    display.setFont(u8g2_font_helvB12_tr);
    int tw = display.getStrWidth("GABRIEL");
    display.drawStr((128 - tw) / 2, 28, "GABRIEL");
    
    display.setFont(u8g2_font_5x7_tr);
    
    // Animated dots
    for (int i = 0; i < 3; i++) {
        display.clearBuffer();
        display.setFont(u8g2_font_helvB12_tr);
        display.drawStr((128 - tw) / 2, 28, "GABRIEL");
        display.setFont(u8g2_font_5x7_tr);
        
        char bootText[16];
        snprintf(bootText, sizeof(bootText), "Booting%.*s", i + 1, "...");
        int bw = display.getStrWidth(bootText);
        display.drawStr((128 - bw) / 2, 50, bootText);
        
        display.sendBuffer();
        delay(400);
    }
}

// ═══════════════════════════════════════════════════════════
// WiFi
// ═══════════════════════════════════════════════════════════

void showWiFiStatus(const char* status, int progress) {
    display.clearBuffer();
    
    // Header
    display.setFont(u8g2_font_5x7_tr);
    display.drawStr(4, 8, "WiFi Connecting...");
    
    // SSID
    display.setFont(u8g2_font_6x10_tr);
    char ssidLine[32];
    snprintf(ssidLine, sizeof(ssidLine), "SSID: %.16s", WIFI_SSID);
    display.drawStr(4, 28, ssidLine);
    
    // Status
    display.drawStr(4, 42, status);
    
    // Progress bar
    display.drawFrame(4, 50, 120, 8);
    if (progress > 0) {
        display.drawBox(4, 50, (120 * progress) / 100, 8);
    }
    
    display.sendBuffer();
}

bool connectWiFi() {
    if (DEBUG_SERIAL) Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    unsigned long startTime = millis();
    int dots = 0;
    
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startTime > WIFI_TIMEOUT_MS) {
            if (DEBUG_SERIAL) Serial.println("[WiFi] ✗ Timeout!");
            showWiFiStatus("TIMEOUT!", 0);
            delay(1000);
            return false;
        }
        
        dots = ((millis() - startTime) * 100) / WIFI_TIMEOUT_MS;
        char statusText[32];
        snprintf(statusText, sizeof(statusText), "Connecting%.*s", (int)((millis() / 500) % 4), "...");
        showWiFiStatus(statusText, dots);
        delay(200);
    }
    
    wifiConnected = true;
    
    if (DEBUG_SERIAL) {
        Serial.printf("[WiFi] ✓ Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
    }
    
    // Show success
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(4, 20, "WiFi Connected!");
    
    char ipLine[32];
    snprintf(ipLine, sizeof(ipLine), "IP: %s", WiFi.localIP().toString().c_str());
    display.drawStr(4, 36, ipLine);
    
    char rssiLine[24];
    snprintf(rssiLine, sizeof(rssiLine), "RSSI: %d dBm", WiFi.RSSI());
    display.drawStr(4, 50, rssiLine);
    
    display.sendBuffer();
    delay(1500);
    
    return true;
}

bool ensureWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    
    wifiConnected = false;
    if (DEBUG_SERIAL) Serial.println("[WiFi] Disconnected, reconnecting...");
    return connectWiFi();
}

// ═══════════════════════════════════════════════════════════
// (FETCH MESSAGE DIHAPUS - DIGANTIKAN VAD STT)
// ═══════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════
// DISPLAY FUNCTIONS
// ═══════════════════════════════════════════════════════════

// ── Combined Face + Ticker Display ──────────────────────────────────
// Layout: face in rows 0-42, separator at 43, ticker in rows 44-63
void drawFaceAndTicker(FaceMood mood, int speakFrame) {
    display.clearBuffer();

    // ── Face frame (rows 1-41) ──────────────────────────────
    display.drawRFrame(4, 1, 120, 40, 5);

    // Eyes
    if (eyesClosed) {
        display.drawHLine(33, 18, 18);
        display.drawHLine(77, 18, 18);
    } else {
        int eyeH = (mood == FACE_LISTENING) ? 14 : 10;
        int eyeY = (mood == FACE_LISTENING) ?  9 : 12;
        display.drawRBox(33, eyeY, 18, eyeH, 3);
        display.drawRBox(77, eyeY, 18, eyeH, 3);
    }

    // Mouth
    switch (mood) {
        case FACE_LISTENING:
            display.drawDisc(64, 34, 3, U8G2_DRAW_ALL);
            break;
        case FACE_PROCESSING: {
            // Simple moving dot — left to right
            int px = 44 + (speakFrame % 5) * 10;
            display.drawDisc(px, 33, 3, U8G2_DRAW_ALL);
            // Static base line
            display.drawHLine(44, 33, 40);
            break;
        }
        case FACE_SPEAKING: {
            // Outline mouth (not filled) to avoid bleed artifacts
            switch (speakFrame % 4) {
                case 0: // closed — thin line
                    display.drawHLine(50, 35, 28);
                    break;
                case 1: // half open — small outline rect
                    display.drawRFrame(50, 31, 28, 6, 2);
                    break;
                case 2: // open — larger outline rect
                    display.drawRFrame(50, 28, 28, 10, 3);
                    break;
                case 3: // half open
                    display.drawRFrame(50, 31, 28, 6, 2);
                    break;
            }
            break;
        }
        case FACE_IDLE:
        default:
            display.drawLine(50, 34, 64, 38);
            display.drawLine(64, 38, 78, 34);
            break;
    }


    // ── Separator line ────────────────────────────────────
    display.drawHLine(0, 43, 128);

    // ── Ticker strip (rows 44-63) ────────────────────────
    display.setFont(u8g2_font_6x10_tr);
    if (tickerText.length() > 0) {
        // Scrolling answer text
        int x = 128 - tickerOffset;
        display.drawStr(x, 60, tickerText.c_str());
        // Draw second copy for seamless loop
        int tw = display.getStrWidth(tickerText.c_str());
        display.drawStr(x + tw + 16, 60, tickerText.c_str());
    } else {
        // Status label when idle
        display.setFont(u8g2_font_5x7_tr);
        const char* lbl = "STANDBY";
        if      (mood == FACE_LISTENING)   lbl = "LISTENING...";
        else if (mood == FACE_PROCESSING)  lbl = "THINKING...";
        else if (mood == FACE_SPEAKING)    lbl = "SPEAKING";
        int lw = display.getStrWidth(lbl);
        display.drawStr((128 - lw) / 2, 58, lbl);
    }

    display.sendBuffer();
}

void updateFaceAnimation(FaceMood mood, bool forceDraw = false) {
    unsigned long now = millis();
    if (nextBlinkMs == 0) nextBlinkMs = now + 1600 + random(0, 1400);

    if (!eyesClosed && now >= nextBlinkMs) {
        eyesClosed = true;
        blinkEndMs = now + 120;
    } else if (eyesClosed && now >= blinkEndMs) {
        eyesClosed = false;
        nextBlinkMs = now + 1400 + random(0, 2200);
    }

    // Advance processing/speaking animation frame every 200ms
    static unsigned long lastFrameMs = 0;
    if (now - lastFrameMs >= 200) {
        speakingFrame = (speakingFrame + 1) % 20;  // large modulus avoids frequent reset
        lastFrameMs = now;
    }

    // Advance ticker scroll every 35ms
    bool tickerMoved = false;
    if (tickerText.length() > 0 && (now - lastTickerMs) >= 35) {
        tickerOffset += 2;
        int tw = display.getStrWidth(tickerText.c_str());
        if (tickerOffset >= tw + 20) tickerOffset = 0;
        lastTickerMs = now;
        tickerMoved  = true;
    }

    if (forceDraw || tickerMoved || (now - lastFaceDrawMs) >= 80) {
        drawFaceAndTicker(mood, speakingFrame);
        lastFaceDrawMs = now;
    }

}



void showError(const char* title, const char* detail) {
    // Show error using face+ticker layout
    tickerText = String(title) + " - " + String(detail);
    tickerOffset = 0;
    drawFaceAndTicker(FACE_IDLE, 0);
}


size_t monoToStereoWithGain(const uint8_t* monoPcmBytes, size_t monoByteLen, uint8_t* stereoOut, size_t stereoOutCapacity) {
    if (monoPcmBytes == nullptr || stereoOut == nullptr || monoByteLen < 2 || stereoOutCapacity < 4) return 0;
    size_t monoSampleCount = monoByteLen / sizeof(int16_t);
    size_t maxStereoSamples = stereoOutCapacity / (sizeof(int16_t) * 2);
    if (monoSampleCount > maxStereoSamples) monoSampleCount = maxStereoSamples;

    const int16_t* mono = (const int16_t*)monoPcmBytes;
    int16_t* stereo = (int16_t*)stereoOut;
    for (size_t i = 0; i < monoSampleCount; i++) {
        int32_t scaled = ((int32_t)mono[i] * (int32_t)SPEAKER_VOLUME_PERCENT) / 100;
        scaled = (scaled * (int32_t)SPEAKER_GAIN_PERCENT) / 100;
        if (scaled > 32767) scaled = 32767;
        if (scaled < -32768) scaled = -32768;
        int16_t sample = (int16_t)scaled;
        stereo[i * 2] = sample;      // Left
        stereo[i * 2 + 1] = sample;  // Right
    }
    return monoSampleCount * sizeof(int16_t) * 2;
}

// Parse WAV header properly — find the "data" chunk offset.
// Returns the byte offset where PCM data starts, or -1 on failure.
// This handles WAV files with extra chunks (fact, LIST, etc.) before "data".
int parseWavHeaderFromStream(WiFiClient* stream) {
    // Read the fixed RIFF header (12 bytes): "RIFF" + size + "WAVE"
    uint8_t riff[12];
    int got = 0;
    unsigned long t0 = millis();
    while (got < 12) {
        if (millis() - t0 > 2000) return -1;
        int r = stream->read(riff + got, 12 - got);
        if (r > 0) got += r;
        else delay(1);
    }
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        if (DEBUG_SERIAL) Serial.println("[AUDIO] Not a valid WAV file");
        return -1;
    }

    // Walk chunks until we find "data"
    int totalSkipped = 12;
    for (int attempt = 0; attempt < 16; attempt++) {
        // Read 8-byte chunk header: 4-byte ID + 4-byte size (little-endian)
        uint8_t chunkHdr[8];
        got = 0;
        t0 = millis();
        while (got < 8) {
            if (millis() - t0 > 2000) return -1;
            int r = stream->read(chunkHdr + got, 8 - got);
            if (r > 0) got += r;
            else delay(1);
        }
        totalSkipped += 8;

        uint32_t chunkSize = (uint32_t)chunkHdr[4]
                           | ((uint32_t)chunkHdr[5] << 8)
                           | ((uint32_t)chunkHdr[6] << 16)
                           | ((uint32_t)chunkHdr[7] << 24);

        if (memcmp(chunkHdr, "data", 4) == 0) {
            // Found PCM data chunk — stream is now positioned at first PCM byte
            if (DEBUG_SERIAL) Serial.printf("[AUDIO] WAV data chunk found at offset %d, size=%u\n",
                                            totalSkipped, (unsigned int)chunkSize);
            return totalSkipped;
        }

        // Skip this chunk's payload (e.g. "fmt ", "fact", "LIST", etc.)
        if (DEBUG_SERIAL) {
            char id[5] = {0};
            memcpy(id, chunkHdr, 4);
            Serial.printf("[AUDIO] Skipping WAV chunk '%s' size=%u\n", id, (unsigned int)chunkSize);
        }
        uint32_t remaining = chunkSize;
        uint8_t skipBuf[64];
        t0 = millis();
        while (remaining > 0) {
            if (millis() - t0 > 3000) return -1;
            size_t want = (remaining < sizeof(skipBuf)) ? remaining : sizeof(skipBuf);
            int r = stream->read(skipBuf, want);
            if (r > 0) {
                remaining -= r;
                totalSkipped += r;
            } else {
                delay(1);
            }
        }
        // WAV chunks are word-aligned (pad byte if odd size)
        if (chunkSize & 1) {
            uint8_t pad;
            stream->read(&pad, 1);
            totalSkipped++;
        }
    }

    if (DEBUG_SERIAL) Serial.println("[AUDIO] WAV data chunk not found");
    return -1;
}

void playAudioFromUrl(String url) {
    if (txChan == nullptr) { Serial.println("[AUDIO] txChan is null — speaker not init"); return; }
    if (url.length() == 0) { Serial.println("[AUDIO] URL is empty"); return; }
    Serial.printf("[AUDIO] Playing: %s\n", url.c_str());

    HTTPClient http;
    http.begin(url);
    http.setTimeout(15000);
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[AUDIO] HTTP GET failed: %d\n", httpCode);
        http.end();
        return;
    }

    int contentLength = http.getSize();
    Serial.printf("[AUDIO] Content-Length: %d\n", contentLength);

    WiFiClient* stream = http.getStreamPtr();

    const size_t MAX_PSRAM_BYTES = 600 * 1024;
    const size_t MAX_HEAP_BYTES  = 180 * 1024;

    size_t allocSize = (contentLength > 0) ? (size_t)contentLength
                     : (psramFound() ? MAX_PSRAM_BYTES : MAX_HEAP_BYTES);

    uint8_t* rawBuf = nullptr;
    if (psramFound()) rawBuf = (uint8_t*)ps_malloc(allocSize);
    if (!rawBuf) {
        if (allocSize > MAX_HEAP_BYTES) allocSize = MAX_HEAP_BYTES;
        rawBuf = (uint8_t*)malloc(allocSize);
    }
    if (!rawBuf) {
        Serial.printf("[AUDIO] Buffer alloc FAILED: need %u bytes, free heap=%u\n",
                      (unsigned)allocSize, ESP.getFreeHeap());
        http.end(); return;
    }
    Serial.printf("[AUDIO] Buffer alloc OK: %u bytes\n", (unsigned)allocSize);

    // ── Download ──────────────────────────────────────────────
    size_t total = 0;
    uint8_t chunk[2048];
    const unsigned long DOWNLOAD_TIMEOUT_MS = 20000;
    unsigned long downloadStart = millis();

    while (total < allocSize) {
        if (millis() - downloadStart > DOWNLOAD_TIMEOUT_MS) {
            Serial.printf("[AUDIO] Download timeout after %u bytes\n", (unsigned)total);
            break;
        }
        int waited = 0;
        while (stream->available() == 0 && waited < 3000) {
            if (!http.connected()) goto download_done;
            delay(10); waited += 10;
        }
        if (stream->available() == 0) break;
        size_t want = stream->available();
        if (want > sizeof(chunk)) want = sizeof(chunk);
        if (want > allocSize - total) want = allocSize - total;
        size_t got = stream->readBytes(chunk, want);
        if (got > 0) { memcpy(rawBuf + total, chunk, got); total += got; }
        if (contentLength > 0 && total >= (size_t)contentLength) break;
    }
    download_done:
    http.end();
    Serial.printf("[AUDIO] Downloaded: %u bytes\n", (unsigned)total);

    if (contentLength > 0 && total < (size_t)contentLength) {
        Serial.printf("[AUDIO] Truncated! got=%u expected=%d — abort\n", (unsigned)total, contentLength);
        free(rawBuf); return;
    }
    if (total < 44) {
        Serial.printf("[AUDIO] Too small (%u bytes) — abort\n", (unsigned)total);
        free(rawBuf); return;
    }

    // ── Parse WAV header ──────────────────────────────────────
    if (memcmp(rawBuf, "RIFF", 4) != 0 || memcmp(rawBuf + 8, "WAVE", 4) != 0) {
        // Dump first 16 bytes for diagnosis
        Serial.printf("[AUDIO] Not a WAV! First bytes: %02X %02X %02X %02X %02X %02X %02X %02X\n",
            rawBuf[0],rawBuf[1],rawBuf[2],rawBuf[3],rawBuf[4],rawBuf[5],rawBuf[6],rawBuf[7]);
        free(rawBuf); return;
    }

    // Read sample rate from fmt chunk (bytes 24-27 in standard WAV)
    uint32_t wavSampleRate = 16000;
    uint16_t wavChannels   = 1;
    uint16_t wavBitsPerSample = 16;
    int dataOffset = -1;
    int pos = 12;
    while (pos + 8 <= (int)total) {
        uint32_t sz = rawBuf[pos+4] | (rawBuf[pos+5]<<8) | (rawBuf[pos+6]<<16) | (rawBuf[pos+7]<<24);
        if (memcmp(rawBuf + pos, "fmt ", 4) == 0) {
            wavChannels    = rawBuf[pos+10] | (rawBuf[pos+11]<<8);
            wavSampleRate  = rawBuf[pos+12] | (rawBuf[pos+13]<<8)
                           | ((uint32_t)rawBuf[pos+14]<<16) | ((uint32_t)rawBuf[pos+15]<<24);
            wavBitsPerSample = rawBuf[pos+22] | (rawBuf[pos+23]<<8);
            Serial.printf("[AUDIO] WAV fmt: %uHz %uch %ubit\n",
                          (unsigned)wavSampleRate, (unsigned)wavChannels, (unsigned)wavBitsPerSample);
        }
        if (memcmp(rawBuf + pos, "data", 4) == 0) {
            dataOffset = pos + 8;
            break;
        }
        pos += 8 + (int)sz + (sz & 1 ? 1 : 0);
    }

    if (dataOffset < 0 || dataOffset >= (int)total) {
        Serial.println("[AUDIO] WAV data chunk not found — abort");
        free(rawBuf); return;
    }

    uint8_t* pcm    = rawBuf + dataOffset;
    size_t   pcmLen = total - dataOffset;
    if (pcmLen & 1) pcmLen--;
    Serial.printf("[AUDIO] PCM: offset=%d len=%u (%.2fs @ %uHz)\n",
        dataOffset, (unsigned)pcmLen,
        pcmLen / (float)(wavSampleRate * wavChannels * (wavBitsPerSample / 8)),
        (unsigned)wavSampleRate);

    // ── Reconfigure I2S clock if WAV sample rate differs ──────
    if (wavSampleRate != I2S_SAMPLE_RATE) {
        Serial.printf("[AUDIO] Reconfiguring I2S from %d to %u Hz\n", I2S_SAMPLE_RATE, (unsigned)wavSampleRate);
        i2s_channel_disable(txChan);
        i2s_std_clk_config_t newClk = I2S_STD_CLK_DEFAULT_CONFIG(wavSampleRate);
        i2s_channel_reconfig_std_clock(txChan, &newClk);
        i2s_channel_enable(txChan);
    }

    // ── Play PCM via I2S ──────────────────────────────────────
    const size_t SBUF = 4096;
    uint8_t* sbuf = (uint8_t*)malloc(SBUF);
    if (!sbuf) {
        Serial.printf("[AUDIO] sbuf malloc FAILED (free heap=%u) — abort\n", ESP.getFreeHeap());
        free(rawBuf); return;
    }

    // Silence warm-up
    memset(sbuf, 0, SBUF);
    size_t dummy = 0;
    i2s_channel_write(txChan, sbuf, SBUF, &dummy, pdMS_TO_TICKS(200));
    Serial.printf("[AUDIO] Starting playback: %u bytes PCM...\n", (unsigned)pcmLen);

    size_t offset2 = 0;
    unsigned long lastAnimMs = 0;
    while (offset2 < pcmLen) {
        size_t monoChunk = pcmLen - offset2;
        if (monoChunk > SBUF / 2) monoChunk = SBUF / 2;
        if (monoChunk & 1) monoChunk--;
        size_t stereoBytes = monoToStereoWithGain(pcm + offset2, monoChunk, sbuf, SBUF);
        if (stereoBytes > 0) {
            size_t written = 0;
            esp_err_t werr = i2s_channel_write(txChan, sbuf, stereoBytes, &written, pdMS_TO_TICKS(500));
            if (werr != ESP_OK) {
                Serial.printf("[AUDIO] i2s_channel_write err: %d (written=%u)\n", werr, (unsigned)written);
            }
        }
        // Animate speaking mouth + scroll ticker every 150ms
        // Also pump WS to receive next sentence chunks while playing current one
        unsigned long nowAnim = millis();
        if (nowAnim - lastAnimMs >= 150) {
            speakingFrame = (speakingFrame + 1) % 4;
            tickerOffset += 4;
            int tw2 = display.getStrWidth(tickerText.c_str());
            if (tickerOffset >= tw2 + 20) tickerOffset = 0;
            drawFaceAndTicker(FACE_SPEAKING, speakingFrame);
            lastAnimMs = nowAnim;
            // Receive queued WS messages while playing (next sentence arrives here)
            if (USE_SOCKET_IO && wsConnected) wsClient.loop();
        }
        offset2 += monoChunk;
    }


    // Silence tail flush
    const size_t SILENCE_FLUSH_BYTES = 32768;
    uint8_t* silenceBuf = (uint8_t*)calloc(SILENCE_FLUSH_BYTES, 1);
    if (silenceBuf) {
        size_t dummy2 = 0;
        i2s_channel_write(txChan, silenceBuf, SILENCE_FLUSH_BYTES, &dummy2, pdMS_TO_TICKS(500));
        free(silenceBuf);
    }
    delay(50);

    // Restore original I2S clock if we changed it
    if (wavSampleRate != I2S_SAMPLE_RATE) {
        i2s_channel_disable(txChan);
        i2s_std_clk_config_t origClk = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE);
        i2s_channel_reconfig_std_clock(txChan, &origClk);
        i2s_channel_enable(txChan);
    } else {
        i2s_channel_disable(txChan);
        delay(10);
        i2s_channel_enable(txChan);
    }

    Serial.printf("[AUDIO] Done | vol=%d%% gain=%d%%\n", SPEAKER_VOLUME_PERCENT, SPEAKER_GAIN_PERCENT);
    free(sbuf);
    free(rawBuf);
}

// ═══════════════════════════════════════════════════════════
// AUDIO UPLOAD
// ═══════════════════════════════════════════════════════════

String getApiHost() {
    String base = String(API_BASE_URL);
    if (base.startsWith("https://")) base = base.substring(8);
    if (base.startsWith("http://")) base = base.substring(7);
    int slash = base.indexOf('/');
    if (slash > 0) base = base.substring(0, slash);
    return base;
}

bool isApiTls() {
    String base = String(API_BASE_URL);
    return base.startsWith("https://");
}

// ── WebSocket Event Handler ──────────────────────────────────
// Handles Socket.IO over raw WebSocket:
//   • Engine.IO handshake (packet '0' = OPEN, '40' = CONNECT OK)
//   • Socket.IO message events ('42["message",{...}]')
//   • Binary audio frames sent by sendBIN
// ── Audio queue helpers ───────────────────────────────────────
bool audioQueuePush(const String& url) {
    int next = (audioQueueTail + 1) % AUDIO_QUEUE_SIZE;
    if (next == audioQueueHead) return false;  // full
    audioUrlQueue[audioQueueTail] = url;
    audioQueueTail = next;
    return true;
}
bool audioQueuePop(String& out) {
    if (audioQueueHead == audioQueueTail) return false;  // empty
    out = audioUrlQueue[audioQueueHead];
    audioQueueHead = (audioQueueHead + 1) % AUDIO_QUEUE_SIZE;
    return true;
}
bool audioQueueIsEmpty() { return audioQueueHead == audioQueueTail; }
void audioQueueReset() { audioQueueHead = audioQueueTail = 0; }

void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            wsConnected  = false;
            wsHandshook  = false;
            wsSessionId  = "";
            if (DEBUG_SERIAL) Serial.println("[WS] Disconnected");
            break;

        case WStype_CONNECTED:
            wsConnected = true;
            if (DEBUG_SERIAL) Serial.printf("[WS] TCP connected to %s\n", (char*)payload);
            // Engine.IO will immediately send "0{...}" OPEN packet — we wait for it.
            break;

        case WStype_TEXT: {
            if (length == 0) break;
            char* text = (char*)payload;
            if (DEBUG_SERIAL) Serial.printf("[WS] TEXT: %.120s\n", text);

            // Engine.IO OPEN packet: starts with '0'
            if (text[0] == '0') {
                // Reply with Socket.IO connect: '40'
                wsClient.sendTXT("40");
                break;
            }

            // Socket.IO EVENT packet: starts with '42'
            if (length >= 2 && text[0] == '4' && text[1] == '2') {
                // text looks like: 42["message","{...}"]
                char* jsonPart = text + 2;  // skip '42'
                JsonDocument root;
                if (deserializeJson(root, jsonPart) != DeserializationError::Ok) break;
                if (!root.is<JsonArray>()) break;
                JsonArray arr = root.as<JsonArray>();
                if (arr.size() < 2) break;
                const char* eventName = arr[0] | "";
                if (strcmp(eventName, "message") != 0) break;

                // Inner payload can be a JSON string or object
                JsonDocument msgDoc;
                JsonVariant msgVariant = arr[1];
                if (msgVariant.is<const char*>()) {
                    if (deserializeJson(msgDoc, msgVariant.as<const char*>()) != DeserializationError::Ok) break;
                } else {
                    msgDoc.set(msgVariant);
                }

                const char* msgType = msgDoc["type"] | "";

                if (strcmp(msgType, "connected") == 0) {
                    wsSessionId = String(msgDoc["session_id"] | "");
                    wsHandshook = true;
                    if (DEBUG_SERIAL) Serial.printf("[WS] Session: %s\n", wsSessionId.c_str());
                }
                else if (strcmp(msgType, "processing") == 0) {
                    if (DEBUG_SERIAL) Serial.println("[WS] Server processing audio...");
                }
                else if (strcmp(msgType, "stt_result") == 0) {
                    const char* txt = msgDoc["text"] | "";
                    if (DEBUG_SERIAL) Serial.printf("[WS] STT: %s\n", txt);
                }
                // ── Streaming: server sends one audio_ready per sentence ──
                else if (strcmp(msgType, "audio_ready") == 0) {
                    const char* url = msgDoc["audio_url"] | "";
                    if (strlen(url) > 0) {
                        String fullUrl = String(url);
                        if (!fullUrl.startsWith("http")) fullUrl = String(API_BASE_URL) + fullUrl;
                        audioQueuePush(fullUrl);
                    }
                    // Accumulate text into ticker
                    const char* ct = msgDoc["text"] | "";
                    if (strlen(ct) > 0) {
                        if (tickerText.length() == 0) tickerText = String(ct);
                        else tickerText += String(" ") + String(ct);
                        tickerOffset = 0;
                    }
                    if (DEBUG_SERIAL) Serial.printf("[WS] audio_ready: %s\n", url);
                }
                // ── Streaming: server done sending all sentences ──
                else if (strcmp(msgType, "done") == 0) {
                    const char* ans = msgDoc["full_answer"] | "";
                    if (strlen(ans) > 0) {
                        strncpy(currentMessage, ans, MAX_MSG_LENGTH - 1);
                        currentMessage[MAX_MSG_LENGTH - 1] = '\0';
                    }
                    strncpy(currentCategory, "chat", sizeof(currentCategory) - 1);
                    wsStreamDone = true;
                    if (DEBUG_SERIAL) Serial.println("[WS] Stream done");
                }
                // ── Legacy: single result packet (fallback) ──
                else if (strcmp(msgType, "result") == 0) {
                    strncpy(currentMessage, msgDoc["answer"] | "No message", MAX_MSG_LENGTH - 1);
                    currentMessage[MAX_MSG_LENGTH - 1] = '\0';
                    strncpy(currentCategory, "chat", sizeof(currentCategory) - 1);
                    const char* audioUrl = msgDoc["audio_url"] | "";
                    if (strlen(audioUrl) > 0) {
                        String fu = String(audioUrl);
                        if (!fu.startsWith("http")) fu = String(API_BASE_URL) + fu;
                        audioQueuePush(fu);
                    }
                    wsStreamDone = true;
                    if (DEBUG_SERIAL) Serial.printf("[WS] Answer(legacy): %s\n", currentMessage);
                }
                else if (strcmp(msgType, "error") == 0) {
                    strncpy(currentMessage, msgDoc["message"] | "WS error", MAX_MSG_LENGTH - 1);
                    currentMessage[MAX_MSG_LENGTH - 1] = '\0';
                    strncpy(currentCategory, "error", sizeof(currentCategory) - 1);
                    if (DEBUG_SERIAL) Serial.printf("[WS] Error: %s\n", currentMessage);
                }
                break;
            }

            // Engine.IO PING? Reply PONG
            if (text[0] == '2') { wsClient.sendTXT("3"); break; }
            break;
        }

        case WStype_ERROR:
            if (DEBUG_SERIAL) Serial.printf("[WS] Error: %.*s\n", (int)length, (char*)payload);
            break;

        default: break;
    }
}

// ── Socket.IO message emit helper ────────────────────────────
// Wraps a JSON payload as Socket.IO event: 42["message", payload]
void wsSendMessage(const char* jsonPayload) {
    // Build: 42["message",<payload>]
    String frame = "42[\"message\",";
    frame += jsonPayload;
    frame += "]";
    wsClient.sendTXT(frame);
    if (DEBUG_SERIAL) Serial.printf("[WS] SEND: %.80s\n", frame.c_str());
}

bool ensureWsConnected() {
    if (!USE_SOCKET_IO) return false;
    if (!ensureWiFi()) return false;
    if (wsConnected && wsHandshook) return true;

    String host = getApiHost();
    if (host.length() == 0) return false;

    if (DEBUG_SERIAL) Serial.printf("[WS] Connecting to %s...\n", host.c_str());

    // Socket.IO endpoint — EIO=4 is Engine.IO v4 (Flask-SocketIO default)
    String path = "/socket.io/?EIO=4&transport=websocket";
    if (isApiTls()) {
        wsClient.beginSSL(host.c_str(), 443, path.c_str());
    } else {
        wsClient.begin(host.c_str(), 80, path.c_str());
    }
    wsClient.onEvent(wsEvent);
    wsClient.setReconnectInterval(4000);
    wsClient.enableHeartbeat(25000, 10000, 3);  // ping every 25s, pong timeout 10s

    unsigned long start = millis();
    while (millis() - start < 15000) {
        wsClient.loop();
        if (wsConnected && wsHandshook) {
            if (DEBUG_SERIAL) Serial.println("[WS] Handshake complete!");
            return true;
        }
        delay(10);
    }
    if (DEBUG_SERIAL) Serial.println("[WS] Connection timeout");
    return false;
}

bool postAudioViaWebSocket() {
    if (!ensureWsConnected()) return false;
    if (audioBuffer == nullptr || audioDataSize < 2) return false;

    // Reset streaming state
    audioQueueReset();
    wsStreamDone     = false;
    currentCategory[0] = '\0';
    tickerText       = "";
    tickerOffset     = 0;

    if (DEBUG_SERIAL) Serial.printf("[WS] Sending %u bytes audio...\n", (unsigned)audioDataSize);

    // Send PCM chunks as base64 JSON
    const size_t CHUNK_PCM = 4096;
    size_t offset = 0;
    while (offset < audioDataSize) {
        size_t n = audioDataSize - offset;
        if (n > CHUNK_PCM) n = CHUNK_PCM;
        String b64Chunk;
        if (!base64Encode(audioBuffer + offset, n, b64Chunk)) {
            if (DEBUG_SERIAL) Serial.println("[WS] base64 encode failed");
            return false;
        }
        String frame = "42[\"message\",{\"type\":\"audio_chunk\",\"data\":\"";
        frame += b64Chunk;
        frame += "\"}]";
        wsClient.sendTXT(frame);
        wsClient.loop();
        delay(5);
        offset += n;
    }
    wsSendMessage("{\"type\":\"audio_end\"}");
    if (DEBUG_SERIAL) Serial.println("[WS] Audio sent. Waiting for streaming response...");

    // ── Streaming play loop ───────────────────────────────────────────────
    // As audio_ready events arrive, play each sentence immediately.
    // wsClient.loop() inside playAudioFromUrl() receives next sentences while playing.
    unsigned long start = millis();
    while (millis() - start < (unsigned long)SOCKET_IO_RESPONSE_TIMEOUT_MS) {
        wsClient.loop();
        updateFaceAnimation(FACE_PROCESSING);

        if (strcmp(currentCategory, "error") == 0) return false;

        // Play any ready sentence
        String nextUrl;
        while (audioQueuePop(nextUrl)) {
            speakingFrame = 0;
            drawFaceAndTicker(FACE_SPEAKING, 0);
            playAudioFromUrl(nextUrl);  // pumps wsClient.loop() internally every 150ms
            // Brief pump after each sentence
            for (int i = 0; i < 8; i++) { wsClient.loop(); delay(8); }
        }

        // Exit when server done AND queue drained
        if (wsStreamDone && audioQueueIsEmpty()) {
            drawFaceAndTicker(FACE_IDLE, 0);
            return true;
        }
        delay(5);
    }

    strncpy(currentMessage, "WebSocket timeout", MAX_MSG_LENGTH - 1);
    strncpy(currentCategory, "error", sizeof(currentCategory) - 1);
    return false;
}


bool postAudio() {
    currentCategory[0] = '\0';
    currentMessage[0] = '\0';

    if (USE_SOCKET_IO && postAudioViaWebSocket()) {
        return true;
    }

    if (!ensureWiFi()) {
        strncpy(currentMessage, "WiFi terputus", MAX_MSG_LENGTH);
        return false;
    }

    if (recordPeakLevel < MIN_UPLOAD_PEAK) {
        if (DEBUG_SERIAL) {
            Serial.printf("[AUDIO] Skip upload: peak too low (%d < %d)\n", recordPeakLevel, MIN_UPLOAD_PEAK);
        }
        strncpy(currentMessage, "Suara terlalu pelan", MAX_MSG_LENGTH - 1);
        currentMessage[MAX_MSG_LENGTH - 1] = '\0';
        strncpy(currentCategory, "error", sizeof(currentCategory) - 1);
        currentCategory[sizeof(currentCategory) - 1] = '\0';
        return false;
    }

    HTTPClient http;
    String url = String(API_BASE_URL) + API_ENDPOINT;

    if (DEBUG_SERIAL) Serial.printf("[API] Preparing %d bytes PCM for: %s\n", audioDataSize, url.c_str());

    const size_t wavSize = audioDataSize + 44;
    uint8_t* wavBuffer = nullptr;
    if (psramFound()) {
        wavBuffer = (uint8_t*)ps_malloc(wavSize);
    }
    if (!wavBuffer) {
        wavBuffer = (uint8_t*)malloc(wavSize);
    }
    if (!wavBuffer) {
        if (DEBUG_SERIAL) Serial.printf("[API] WAV alloc failed: need %u bytes, free heap=%u\n",
                                        (unsigned)wavSize, ESP.getFreeHeap());
        strncpy(currentMessage, "WAV alloc failed", MAX_MSG_LENGTH);
        strncpy(currentCategory, "error", sizeof(currentCategory));
        return false;
    }

    writeWavHeader(wavBuffer, (uint32_t)audioDataSize, I2S_SAMPLE_RATE);
    memcpy(wavBuffer + 44, audioBuffer, audioDataSize);

    String audioBase64;
    if (!base64Encode(wavBuffer, wavSize, audioBase64)) {
        free(wavBuffer);
        strncpy(currentMessage, "Base64 encode failed", MAX_MSG_LENGTH);
        strncpy(currentCategory, "error", sizeof(currentCategory));
        return false;
    }
    free(wavBuffer);

    String requestBody;
    requestBody.reserve(audioBase64.length() + 96);
    requestBody = "{\"audio\":\"";
    requestBody += audioBase64;
    requestBody += "\",\"include_audio\":true,\"audio_delivery\":\"url\"}";

    if (DEBUG_SERIAL) {
        float recordSeconds = (float)audioDataSize / (float)(I2S_SAMPLE_RATE * 2);
        Serial.printf("[AUDIO] PCM bytes: %u | WAV bytes: %u | b64 chars: %u | rec: %.2fs\n",
                      (unsigned int)audioDataSize,
                      (unsigned int)wavSize,
                      (unsigned int)audioBase64.length(),
                      recordSeconds);
    }

    http.begin(url);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Client", "esp32");
    if (DEBUG_SERIAL) {
        Serial.printf("[API] Upload bytes(json): %d | Free heap: %u\n", requestBody.length(), ESP.getFreeHeap());
    }

    int httpCode = http.POST((uint8_t*)requestBody.c_str(), requestBody.length());
    if (httpCode < 0) {
        if (DEBUG_SERIAL) {
            Serial.printf("[API] Transport error: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
            Serial.println("[API] Retrying request once...");
        }
        http.end();
        delay(250);
        http.begin(url);
        http.setTimeout(HTTP_TIMEOUT_MS);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("X-Client", "esp32");
        httpCode = http.POST((uint8_t*)requestBody.c_str(), requestBody.length());
    }

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument filter;
        filter["answer"] = true;
        filter["message"] = true;
        filter["question"] = true;
        filter["stage"] = true;
        filter["error"] = true;
        filter["audio_url"] = true;
        filter["audio_id"] = true;
        filter["audio_mime"] = true;
        filter["audio_encoding"] = true;

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
        if (error) {
            String fallbackAnswer = extractJsonStringField(payload, "answer");
            if (fallbackAnswer.length() > 0) {
                strncpy(currentMessage, fallbackAnswer.c_str(), MAX_MSG_LENGTH - 1);
                currentMessage[MAX_MSG_LENGTH - 1] = '\0';
                strncpy(currentCategory, "chat", sizeof(currentCategory) - 1);
                currentCategory[sizeof(currentCategory) - 1] = '\0';
                strncpy(currentEmoji, "AI", sizeof(currentEmoji) - 1);
                currentEmoji[sizeof(currentEmoji) - 1] = '\0';
                if (DEBUG_SERIAL) {
                    Serial.printf("[API] JSON parse fallback OK | payload_len=%u\n", (unsigned int)payload.length());
                    Serial.printf("[MSG] %s | %s: %s\n", currentEmoji, currentCategory, currentMessage);
                }
                http.end();
                return true;
            }

            if (DEBUG_SERIAL) {
                String preview = payload.substring(0, 220);
                Serial.printf("[API] JSON parse error: %s | payload_len=%u | preview=%s\n",
                              error.c_str(), (unsigned int)payload.length(), preview.c_str());
            }
            strncpy(currentMessage, "JSON parse error", MAX_MSG_LENGTH);
            strncpy(currentCategory, "error", sizeof(currentCategory));
            http.end();
            return false;
        }

        strncpy(currentMessage, doc["answer"] | doc["message"] | "No message", MAX_MSG_LENGTH - 1);
        currentMessage[MAX_MSG_LENGTH - 1] = '\0';
        strncpy(currentCategory, "chat", sizeof(currentCategory) - 1);
        strncpy(currentEmoji, "AI", sizeof(currentEmoji) - 1);

        const char* audioUrl = doc["audio_url"] | "";
        if (strlen(audioUrl) > 0) {
            pendingAudioUrl = String(API_BASE_URL) + audioUrl;
        } else {
            pendingAudioUrl = "";
        }

        if (DEBUG_SERIAL) {
            const char* question = doc["question"] | "";
            Serial.printf("[API] OK 200 | question_len=%u | answer_len=%u\n",
                          (unsigned int)strlen(question),
                          (unsigned int)strlen(currentMessage));
            if (strlen(question) > 0) {
                Serial.printf("[API] STT: %s\n", question);
            }
            Serial.printf("[MSG] %s | %s: %s\n", currentEmoji, currentCategory, currentMessage);
        }

        http.end();
        return true;
    } else {
        String errPayload = http.getString();
        if (DEBUG_SERIAL) {
            if (httpCode < 0) {
                Serial.printf("[API] Transport error: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
            } else {
                Serial.printf("[API] HTTP Error: %d | %s\n", httpCode, errPayload.c_str());
            }
        }
        char errMsg[MAX_MSG_LENGTH];
        if (httpCode < 0) {
            snprintf(errMsg, MAX_MSG_LENGTH, "NET Error: %d", httpCode);
        } else {
            snprintf(errMsg, MAX_MSG_LENGTH, "HTTP Error: %d", httpCode);
        }
        strncpy(currentMessage, errMsg, MAX_MSG_LENGTH);
        strncpy(currentCategory, "error", sizeof(currentCategory));
        http.end();
        return false;
    }
}

// ═══════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(500);
    
    Serial.println();
    Serial.println("============================================");
    Serial.println("  GABRIEL — Cloud Messenger for ESP32-S3");
    Serial.println("  Version 1.0.0");
    Serial.println("============================================");
    
    // Init OLED
    display.begin();
    display.setContrast(200);
    display.clearBuffer();
    display.sendBuffer();
    
    // Boot animation
    showBootAnimation();
    
    // Init I2S (new driver API to avoid legacy/new conflict)
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t i2sErr = i2s_new_channel(&chanCfg, NULL, &rxChan);
    if (i2sErr != ESP_OK) {
        Serial.printf("[ERROR] i2s_new_channel failed: %d\n", i2sErr);
    }

    i2s_std_config_t stdCfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SCK,
            .ws = I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    stdCfg.slot_cfg.slot_mask = INMP441_USE_LEFT ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_RIGHT;

    if (rxChan != nullptr) {
        i2sErr = i2s_channel_init_std_mode(rxChan, &stdCfg);
        if (i2sErr != ESP_OK) {
            Serial.printf("[ERROR] i2s_channel_init_std_mode failed: %d\n", i2sErr);
        }

        i2sErr = i2s_channel_enable(rxChan);
        if (i2sErr != ESP_OK) {
            Serial.printf("[ERROR] i2s_channel_enable failed: %d\n", i2sErr);
        }

        flushI2SInput();
    }

    // Init I2S Speaker (MAX98357A) on I2S_NUM_1
    i2s_chan_config_t txChanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
txChanCfg.dma_desc_num  = 8;   // tambah ini (default 6)
txChanCfg.dma_frame_num = 512; // tambah ini (default 240) → buffer lebih besar
i2sErr = i2s_new_channel(&txChanCfg, &txChan, NULL);
    if (i2sErr != ESP_OK) {
        Serial.printf("[SPEAKER] i2s_new_channel failed: %d\n", i2sErr);
    } else {
        i2s_std_config_t txStdCfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = I2S_SPEAKER_BCLK,
                .ws = I2S_SPEAKER_LRC,
                .dout = I2S_SPEAKER_DIN,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
            },
        };
        i2sErr = i2s_channel_init_std_mode(txChan, &txStdCfg);
        if (i2sErr == ESP_OK) {
            i2s_channel_enable(txChan);
            Serial.println("[SPEAKER] OK: I2S TX initialized");
        } else {
            Serial.printf("[SPEAKER] i2s_channel_init_std_mode failed: %d\n", i2sErr);
        }
    }

    if (DEBUG_SERIAL && MIC_DIAG_ON_BOOT) {
        runMicDiagnostic(MIC_DIAG_DURATION_MS);
    }
    
    // Prefer PSRAM for the large audio capture buffer so that regular heap
    // stays free for HTTP/WS stacks and the WAV envelope allocation later.
    if (psramFound()) {
        audioBuffer = (uint8_t*)ps_malloc(RECORD_SIZE);
        if (audioBuffer) Serial.printf("[AUDIO] audioBuffer in PSRAM: %u bytes\n", (unsigned)RECORD_SIZE);
    }
    if (!audioBuffer) {
        audioBuffer = (uint8_t*)malloc(RECORD_SIZE);
        if (audioBuffer) Serial.printf("[AUDIO] audioBuffer in heap: %u bytes\n", (unsigned)RECORD_SIZE);
    }
    if (!audioBuffer) {
        Serial.println("[ERROR] Failed to allocate audio buffer — out of memory!");
    }
    
    // Connect WiFi
    if (!connectWiFi()) {
        showError("WiFi Failed!", "Check SSID & password in config.h");
    }
    
    Serial.println("[SETUP] ✓ Gabriel ready!");
    Serial.printf("[SETUP] API: %s%s\n", API_BASE_URL, API_ENDPOINT);
    Serial.printf("[SETUP] Audio cfg: %d Hz, shift=%d, RECORD_TIME=%ds, REC_BUF=%u bytes, MIN_UPLOAD_PEAK=%d\n",
                  I2S_SAMPLE_RATE, I2S_SAMPLE_SHIFT, RECORD_TIME, (unsigned int)RECORD_SIZE, MIN_UPLOAD_PEAK);
    Serial.println("[SETUP] Entering VAD Standby Mode...");
    
    randomSeed((uint32_t)esp_random());
    updateFaceAnimation(FACE_IDLE, true);
}

// ═══════════════════════════════════════════════════════════
// MAIN LOOP
// ═══════════════════════════════════════════════════════════

void loop() {
    unsigned long now = millis();
    
    switch (currentState) {
        case STATE_STANDBY: {
            int32_t sampleBuffer[256];
            size_t bytesRead = 0;
            
            // Read a small chunk of audio to detect voice
            if (rxChan != nullptr) {
                i2s_channel_read(rxChan, sampleBuffer, sizeof(sampleBuffer), &bytesRead, pdMS_TO_TICKS(20));
            }
            updateFaceAnimation(FACE_IDLE);
            
            int samples = bytesRead / sizeof(int32_t);
            if (samples > 0) {
                long sum = 0;
                for (int i = 0; i < samples; i++) {
                    int16_t pcm = convertI2SSampleToPCM16(sampleBuffer[i]);
                    sum += abs(pcm);
                }
                int avgEnergy = sum / samples;
                
                if (avgEnergy > VAD_THRESHOLD) {
                    if (vadHitCount < VAD_START_HITS) vadHitCount++;
                } else if (vadHitCount > 0) {
                    vadHitCount--;
                }

                if (avgEnergy > VAD_THRESHOLD && DEBUG_SERIAL) {
                    Serial.printf("[VAD] energy=%d thr=%d hits=%d/%d\n",
                                  avgEnergy, VAD_THRESHOLD, vadHitCount, VAD_START_HITS);
                }

                if (vadHitCount >= VAD_START_HITS) {
                    if (DEBUG_SERIAL) Serial.println("[VAD] Trigger confirmed -> RECORDING");
                    
                    currentState = STATE_RECORDING;
                    audioDataSize = 0;
                    recordStartMs = millis();
                    lastRecordProgressLogMs = recordStartMs;
                    recordPeakLevel = avgEnergy;
                    silenceStartMs = 0;
                    vadHitCount = 0;
                    flushI2SInput(); // flush to start clean recording
                    
                    updateFaceAnimation(FACE_LISTENING, true);
                }
            }
            break;
        }
        
        case STATE_RECORDING: {
            bool stopBySilence = false;
            bool stopByBufferFull = false;
            if (audioBuffer && audioDataSize < RECORD_SIZE) {
                int32_t i2sChunk[256];
                size_t bytesRead = 0;
                if (rxChan != nullptr) {
                    i2s_channel_read(rxChan, i2sChunk, sizeof(i2sChunk), &bytesRead, 10 / portTICK_PERIOD_MS);
                }
                size_t sampleCount = bytesRead / sizeof(int32_t);
                size_t bytesAvail = RECORD_SIZE - audioDataSize;
                size_t maxSamplesToCopy = bytesAvail / sizeof(int16_t);
                if (sampleCount > maxSamplesToCopy) sampleCount = maxSamplesToCopy;

                int16_t* out = (int16_t*)(audioBuffer + audioDataSize);
                long chunkSum = 0;
                for (size_t i = 0; i < sampleCount; i++) {
                    int16_t pcm = convertI2SSampleToPCM16(i2sChunk[i]);
                    int level = abs(pcm);
                    if (level > recordPeakLevel) recordPeakLevel = level;
                    chunkSum += level;
                    out[i] = pcm;  // raw capture; server handles speech filtering
                }
                audioDataSize += sampleCount * sizeof(int16_t);
                int chunkAvgEnergy = (sampleCount > 0) ? (int)(chunkSum / (long)sampleCount) : 0;

                unsigned long nowMs = millis();
                if (chunkAvgEnergy < VAD_STOP_THRESHOLD) {
                    if (silenceStartMs == 0) silenceStartMs = nowMs;
                } else {
                    silenceStartMs = 0;
                }

                unsigned long minRecordBytes = (unsigned long)((I2S_SAMPLE_RATE * 2UL * MIN_RECORD_MS) / 1000UL);
                if (audioDataSize >= minRecordBytes && silenceStartMs > 0 && (nowMs - silenceStartMs) >= VAD_SILENCE_MS) {
                    stopBySilence = true;
                }

                if (DEBUG_SERIAL) {
                    if (nowMs - lastRecordProgressLogMs >= 500) {
                        float recSec = (float)(nowMs - recordStartMs) / 1000.0f;
                        float bufferedSec = (float)audioDataSize / (float)(I2S_SAMPLE_RATE * 2);
                        float maxSec = (float)RECORD_SIZE / (float)(I2S_SAMPLE_RATE * 2);
                        Serial.printf("[REC] t=%.2fs | buffered=%.2fs/%.2fs | bytes=%u | peak=%d | avg=%d\n",
                                      recSec, bufferedSec, maxSec, (unsigned int)audioDataSize, recordPeakLevel, chunkAvgEnergy);
                        lastRecordProgressLogMs = nowMs;
                    }
                }
            } else {
                stopByBufferFull = true;
            }

            if (stopBySilence || stopByBufferFull) {
                currentState = STATE_PROCESSING;
                if (DEBUG_SERIAL) {
                    float finalSec = (float)audioDataSize / (float)(I2S_SAMPLE_RATE * 2);
                    Serial.printf("[REC] Done | duration=%.2fs | bytes=%u | peak=%d | reason=%s\n",
                                  finalSec, (unsigned int)audioDataSize, recordPeakLevel,
                                  stopBySilence ? "silence" : "buffer_full");
                }
                
                updateFaceAnimation(FACE_PROCESSING, true);
            }
            updateFaceAnimation(FACE_LISTENING);
            break;
        }
        
        case STATE_PROCESSING: {
            updateFaceAnimation(FACE_PROCESSING, true);
            if (DEBUG_SERIAL) Serial.printf("[AUDIO] Finished. Size: %d bytes\n", audioDataSize);

            if (postAudio()) {
                fetchCount++;
                tickerText = String(currentMessage);
                tickerOffset = 0;
                if (pendingAudioUrl.length() > 0) {
                    speakingFrame = 0;
                    playAudioFromUrl(pendingAudioUrl);
                    pendingAudioUrl = "";
                }
                // After speaking: idle face with ticker still scrolling
                // Go to DISPLAY briefly so ticker is visible, then standby
                drawFaceAndTicker(FACE_IDLE, 0);
                currentState = STATE_DISPLAY;
                stateStartTime = millis();
            } else {
                showError("Voice Failed!", currentMessage);
                currentState = STATE_DISPLAY;
                stateStartTime = millis();
            }
            break;
        }

        
        case STATE_DISPLAY: {
            // Keep ticker scrolling while displaying answer
            updateFaceAnimation(FACE_IDLE);

            // Short timeout (2s) then back to standby — mic ready again
            bool displayTimeout = (millis() - stateStartTime > 2000);

            // VAD interrupt: user speaks before timeout
            bool vadInterrupt = false;
            if (!displayTimeout && rxChan != nullptr) {
                int32_t vadBuf[64];
                size_t vadBytes = 0;
                if (i2s_channel_read(rxChan, vadBuf, sizeof(vadBuf), &vadBytes, 0) == ESP_OK && vadBytes > 0) {
                    size_t vadSamples = vadBytes / sizeof(int32_t);
                    long vadSum = 0;
                    for (size_t i = 0; i < vadSamples; i++) vadSum += abs(convertI2SSampleToPCM16(vadBuf[i]));
                    int vadEnergy = (int)(vadSum / (long)vadSamples);
                    if (vadEnergy > VAD_THRESHOLD) {
                        vadHitCount++;
                        if (vadHitCount >= VAD_START_HITS) {
                            vadInterrupt = true;
                            if (DEBUG_SERIAL) Serial.printf("[VAD] Interrupt during DISPLAY (energy=%d)\n", vadEnergy);
                        }
                    } else if (vadHitCount > 0) {
                        vadHitCount--;
                    }
                }
            }

            if (displayTimeout || vadInterrupt) {
                currentState = vadInterrupt ? STATE_RECORDING : STATE_STANDBY;
                vadHitCount = 0;

                if (vadInterrupt) {
                    audioDataSize = 0;
                    recordStartMs = millis();
                    lastRecordProgressLogMs = recordStartMs;
                    recordPeakLevel = 0;
                    silenceStartMs = 0;
                    flushI2SInput();
                    updateFaceAnimation(FACE_LISTENING, true);
                } else {
                    tickerText = "";   // clear answer text when idle
                    tickerOffset = 0;
                    updateFaceAnimation(FACE_IDLE, true);
                    flushI2SInput();
                }
            }

            break;
        }
    }
    
    // ── WiFi Watchdog ───────────────────────────────────────
    static unsigned long lastWiFiCheck = 0;
    if (now - lastWiFiCheck >= 30000) {
        if (WiFi.status() != WL_CONNECTED) {
            wifiConnected = false;
            if (DEBUG_SERIAL) Serial.println("[WiFi] Connection lost, will retry on next operation");
            ensureWiFi();
        }
        lastWiFiCheck = now;
    }

    if (USE_SOCKET_IO) {
        wsClient.loop();  // keep WebSocket alive, handle ping/pong
    }
    
    delay(10); // Small delay to prevent watchdog reset
}