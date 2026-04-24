// ============================================================
// ☁️ GABRIEL — Cloud Messenger for ESP32-S3
// Fetch AI-generated messages and display on OLED
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include <mbedtls/base64.h>
#include "config.h"

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
const size_t RECORD_SIZE = I2S_SAMPLE_RATE * 2 * RECORD_TIME;
uint8_t* audioBuffer = nullptr;
size_t audioDataSize = 0;
i2s_chan_handle_t rxChan = nullptr;

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

// ── App States ──────────────────────────────────────────────
enum AppState {
    STATE_STANDBY,
    STATE_RECORDING,
    STATE_PROCESSING,
    STATE_DISPLAY
};
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

void showFetchingAnimation() {
    for (int frame = 0; frame < 6; frame++) {
        display.clearBuffer();
        display.setFont(u8g2_font_6x10_tr);
        
        // Cloud icon (simple ASCII art)
        const char* cloud = "( ^_^ )";
        int cw = display.getStrWidth(cloud);
        display.drawStr((128 - cw) / 2, 24, cloud);
        
        // Animated text
        char fetchText[24];
        snprintf(fetchText, sizeof(fetchText), "Fetching%.*s", (frame % 3) + 1, "...");
        int fw = display.getStrWidth(fetchText);
        display.drawStr((128 - fw) / 2, 44, fetchText);
        
        // Spinning indicator
        const char* spinner[] = { "|", "/", "-", "\\" };
        display.drawStr(118, 58, spinner[frame % 4]);
        
        display.sendBuffer();
        delay(150);
    }
}

// Word-wrap dan tampilkan teks di area body
void drawWrappedText(const char* text, int startY, int maxWidth, int maxLines) {
    display.setFont(u8g2_font_5x7_tr);
    
    int len = strlen(text);
    int lineNum = 0;
    int pos = 0;
    
    while (pos < len && lineNum < maxLines) {
        // Hitung berapa karakter muat di satu baris
        int lineEnd = pos + MAX_CHARS_LINE;
        if (lineEnd >= len) {
            lineEnd = len;
        } else {
            // Cari spasi terakhir untuk word-wrap
            int lastSpace = lineEnd;
            while (lastSpace > pos && text[lastSpace] != ' ') {
                lastSpace--;
            }
            if (lastSpace > pos) {
                lineEnd = lastSpace;
            }
        }
        
        // Copy line ke buffer
        char line[MAX_CHARS_LINE + 1];
        int lineLen = lineEnd - pos;
        if (lineLen > MAX_CHARS_LINE) lineLen = MAX_CHARS_LINE;
        strncpy(line, text + pos, lineLen);
        line[lineLen] = '\0';
        
        // Draw line
        int y = startY + (lineNum * LINE_HEIGHT);
        display.drawStr(2, y, line);
        
        lineNum++;
        pos = lineEnd;
        
        // Skip spasi di awal baris baru
        while (pos < len && text[pos] == ' ') pos++;
    }
}

void showMessage() {
    display.clearBuffer();
    
    // ── Header Bar ──────────────────────────────────────────
    display.setDrawColor(1);
    display.drawBox(0, 0, 128, HEADER_HEIGHT);
    display.setDrawColor(0);
    display.setFont(u8g2_font_5x8_tr);
    
    // Category label
    char header[24];
    if (strcmp(currentCategory, "tip") == 0) {
        snprintf(header, sizeof(header), " TIPS");
    } else if (strcmp(currentCategory, "news") == 0) {
        snprintf(header, sizeof(header), " NEWS");
    } else if (strcmp(currentCategory, "motivation") == 0) {
        snprintf(header, sizeof(header), " MOTIVATION");
    } else if (strcmp(currentCategory, "fact") == 0) {
        snprintf(header, sizeof(header), " FUN FACT");
    } else {
        snprintf(header, sizeof(header), " GABRIEL");
    }
    display.drawStr(2, 10, header);
    
    // WiFi indicator (kanan atas)
    if (WiFi.status() == WL_CONNECTED) {
        display.drawStr(108, 10, "WiFi");
    } else {
        display.drawStr(112, 10, "!!");
    }
    
    display.setDrawColor(1);
    
    // ── Body: Message ───────────────────────────────────────
    int maxLines = BODY_HEIGHT / LINE_HEIGHT;
    drawWrappedText(currentMessage, BODY_Y_START + 8, 128, maxLines);
    
    // ── Footer Bar ──────────────────────────────────────────
    int footerY = 64 - FOOTER_HEIGHT;
    display.drawHLine(0, footerY, 128);
    display.setFont(u8g2_font_micro_tr);
    
    // Footer stats
    char footer[32];
    snprintf(footer, sizeof(footer), "Chats: %d", fetchCount);
    display.drawStr(2, 63, footer);
    
    // RSSI indicator
    if (WiFi.status() == WL_CONNECTED) {
        int rssi = WiFi.RSSI();
        char rssiStr[12];
        snprintf(rssiStr, sizeof(rssiStr), "%ddBm", rssi);
        int rw = display.getStrWidth(rssiStr);
        display.drawStr(128 - rw - 2, 63, rssiStr);
    }
    
    display.sendBuffer();
}

// Typewriter effect untuk pesan baru
void showMessageWithTypewriter() {
    int len = strlen(currentMessage);
    
    for (int i = 1; i <= len; i++) {
        // Temporary truncated message
        char partial[MAX_MSG_LENGTH];
        strncpy(partial, currentMessage, i);
        partial[i] = '\0';
        
        // Simpan full message, tampilkan partial
        char fullMsg[MAX_MSG_LENGTH];
        strncpy(fullMsg, currentMessage, MAX_MSG_LENGTH);
        strncpy(currentMessage, partial, MAX_MSG_LENGTH);
        
        showMessage();
        
        // Restore full message
        strncpy(currentMessage, fullMsg, MAX_MSG_LENGTH);
        
        // Skip beberapa frame untuk kecepatan
        if (i > 10 && i % 2 != 0) continue;
        
        delay(SCROLL_SPEED_MS / 2);
    }
    
    // Final display dengan full message
    showMessage();
}

void showError(const char* title, const char* detail) {
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tr);
    
    int tw = display.getStrWidth(title);
    display.drawStr((128 - tw) / 2, 20, title);
    
    display.setFont(u8g2_font_5x7_tr);
    drawWrappedText(detail, 34, 128, 3);
    
    // Retry hint
    display.setFont(u8g2_font_micro_tr);
    const char* hint = "Press BOOT to retry";
    int hw = display.getStrWidth(hint);
    display.drawStr((128 - hw) / 2, 62, hint);
    
    display.sendBuffer();
}

// ═══════════════════════════════════════════════════════════
// AUDIO UPLOAD
// ═══════════════════════════════════════════════════════════

bool postAudio() {
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
    uint8_t* wavBuffer = (uint8_t*)malloc(wavSize);
    if (!wavBuffer) {
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
    requestBody.reserve(audioBase64.length() + 32);
    requestBody = "{\"audio\":\"";
    requestBody += audioBase64;
    requestBody += "\"}";

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

    if (DEBUG_SERIAL && MIC_DIAG_ON_BOOT) {
        runMicDiagnostic(MIC_DIAG_DURATION_MS);
    }
    
    audioBuffer = (uint8_t*)malloc(RECORD_SIZE);
    if (!audioBuffer) {
        Serial.println("[ERROR] Failed to allocate audio buffer");
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
    
    // Initial UI
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(36, 36, "Standby");
    display.sendBuffer();
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
                i2s_channel_read(rxChan, sampleBuffer, sizeof(sampleBuffer), &bytesRead, portMAX_DELAY);
            }
            
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
                    
                    display.clearBuffer();
                    display.setFont(u8g2_font_6x10_tr);
                    display.drawStr(28, 36, "Listening...");
                    display.sendBuffer();
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
                
                display.clearBuffer();
                display.setFont(u8g2_font_6x10_tr);
                display.drawStr(28, 36, "Processing...");
                display.sendBuffer();
            }
            break;
        }
        
        case STATE_PROCESSING: {
            if (DEBUG_SERIAL) Serial.printf("[AUDIO] Finished. Size: %d bytes\n", audioDataSize);
            
            if (postAudio()) {
                fetchCount++;
                showMessageWithTypewriter();
            } else {
                showError("Voice Failed!", currentMessage);
            }
            
            currentState = STATE_DISPLAY;
            stateStartTime = millis();
            break;
        }
        
        case STATE_DISPLAY: {
            if (millis() - stateStartTime > MESSAGE_DISPLAY_MS) {
                currentState = STATE_STANDBY;
                currentMessage[0] = '\0';
                
                display.clearBuffer();
                display.setFont(u8g2_font_6x10_tr);
                display.drawStr(36, 36, "Standby");
                display.sendBuffer();
                
                flushI2SInput(); // flush residual audio before standby
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
    
    delay(10); // Small delay to prevent watchdog reset
}