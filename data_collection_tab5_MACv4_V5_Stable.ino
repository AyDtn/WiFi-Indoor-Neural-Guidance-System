#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ======================================================
// COLLECTE WIFI "TOP 5 AP" + ENVOI MQTT
// - 1 ligne CSV = Zone, mac1,rssi1, ... mac5,rssi5
// - Vérifie présence SD avant scan
// ======================================================

#define TOP_K_AP 5

// ==================== CONFIG WIFI & MQTT ====================
#define WIFI_SSID "Wifi 42"
#define WIFI_PASSWORD "61454300"

#define MQTT_BROKER "2bcf898148934fe9b985e2edd298658a.s1.eu.hivemq.cloud"
#define MQTT_PORT 8883
#define MQTT_USERNAME "Asmaa"
#define MQTT_PASSWORD "07121416Li"
#define MQTT_CLIENT_ID "Asmaa"

#define TOPIC_DATA   "LaVilette/data"
#define TOPIC_STATUS "LaVilette/status"

// ==================== PARAMÈTRES SCAN ====================
#define SAMPLES_PER_SAMPLE 2
#define SAMPLE_DELAY_MS 80
#define SCAN_MAX_MS_PER_CHAN 90

// Batch
#define BATCH_SCANS 50
#define BATCH_TOTAL_MS 180000UL

// MQTT chunks
#define CHUNK_SAMPLES 5

// ==================== NB ZONES ====================
#define NUM_LOCATIONS 4

// ==================== STRUCTURES UI ====================
struct TouchButton {
  int x, y, w, h;
  String label;
  uint16_t color;
};

TouchButton locationButtons[NUM_LOCATIONS];
TouchButton sendMqttButton;
TouchButton resetDataButton;
TouchButton modeButton;

// ==================== VARIABLES ====================
int selected_location = -1;
int total_samples = 0;
int zone_scan_count[NUM_LOCATIONS] = {0};

WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

enum ScanMode { MODE_SINGLE = 0, MODE_BATCH = 1 };
ScanMode currentMode = MODE_SINGLE;

// ==================== TOP 5 AP STRUCTS (DECLARE AVANT FONCTIONS) ====================
struct ApEntry {
  String mac;
  int rssi;
};

// ==================== UTILITAIRES ====================
String scanLabel(int n) {
  if (n == 1) return "1 scan";
  return String(n) + " scans";
}

void resetZoneCounts() {
  for (int i = 0; i < NUM_LOCATIONS; i++) zone_scan_count[i] = 0;
}

String modeLabel() {
  return (currentMode == MODE_SINGLE) ? "MODE: 1x" : "MODE: 50x/3min";
}

// ==================== SD CHECK ====================
bool checkSDCard() {
  if (!SD.begin()) {
    M5.Display.fillScreen(TFT_RED);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(3);
    M5.Display.drawString("CARTE SD ABSENTE !",
                          M5.Display.width() / 2,
                          M5.Display.height() / 2);
    delay(2000);
    return false;
  }
  return true;
}

// ==================== UI ====================
void initButtons() {
  int screenW = M5.Display.width();
  int screenH = M5.Display.height();

  const int margin = 20;
  const int gapX = 20;
  const int gapY = 18;

  const int headerH = 70;
  const int footerH = 170;

  int zonesTop = headerH + margin;
  int zonesBottom = screenH - footerH - margin;
  int zonesH = zonesBottom - zonesTop;

  int cols = 2;
  int rows = (NUM_LOCATIONS + 1) / 2;

  int bw = (screenW - 2 * margin - (cols - 1) * gapX) / cols;
  int bh = (zonesH - (rows - 1) * gapY) / rows;

  for (int i = 0; i < NUM_LOCATIONS; i++) {
    int col = i % 2;
    int row = i / 2;

    locationButtons[i] = {
      margin + col * (bw + gapX),
      zonesTop + row * (bh + gapY),
      bw, bh,
      "Zone " + String(i + 1),
      TFT_DARKGREY
    };
  }

  int bigW = screenW - 2 * margin;
  int bigH = 120;
  int y = screenH - bigH - margin;

  sendMqttButton = { margin, y, bigW, bigH, "ENVOYER VERS HIVEMQ", TFT_BLUE };

  resetDataButton = { screenW - margin - 190, 10, 190, 60, "RESET SD", TFT_RED };

  modeButton = { margin, 10, 230, 60, "MODE", TFT_DARKCYAN };
}

void drawUI(const String& statusLine = "") {
  int screenW = M5.Display.width();

  M5.Display.clear(TFT_BLACK);

  M5.Display.setTextDatum(top_center);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextSize(2);
  M5.Display.drawString("COLLECTE WIFI - TOP 5 AP", screenW / 2, 10);

  M5.Display.fillRoundRect(modeButton.x, modeButton.y, modeButton.w, modeButton.h, 10, modeButton.color);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  M5.Display.drawString(modeLabel(), modeButton.x + modeButton.w / 2, modeButton.y + modeButton.h / 2);

  M5.Display.fillRoundRect(resetDataButton.x, resetDataButton.y, resetDataButton.w, resetDataButton.h, 10, resetDataButton.color);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  M5.Display.drawString(resetDataButton.label,
                        resetDataButton.x + resetDataButton.w / 2,
                        resetDataButton.y + resetDataButton.h / 2);

  if (statusLine.length() > 0) {
    M5.Display.setTextDatum(top_center);
    M5.Display.setTextSize(1);
    M5.Display.drawString(statusLine, screenW / 2, 55);
  }

  for (int i = 0; i < NUM_LOCATIONS; i++) {
    uint16_t col = (i == selected_location) ? TFT_GREEN : locationButtons[i].color;

    M5.Display.fillRoundRect(locationButtons[i].x, locationButtons[i].y,
                             locationButtons[i].w, locationButtons[i].h, 12, col);

    int cx = locationButtons[i].x + locationButtons[i].w / 2;
    int cy = locationButtons[i].y + locationButtons[i].h / 2;

    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE);

    M5.Display.setTextSize(2);
    M5.Display.drawString(locationButtons[i].label, cx, cy - 16);

    M5.Display.setTextSize(1);
    M5.Display.drawString(scanLabel(zone_scan_count[i]), cx, cy + 18);
  }

  M5.Display.fillRoundRect(sendMqttButton.x, sendMqttButton.y, sendMqttButton.w, sendMqttButton.h, 14, sendMqttButton.color);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  M5.Display.drawString(sendMqttButton.label,
                        sendMqttButton.x + sendMqttButton.w / 2,
                        sendMqttButton.y + sendMqttButton.h / 2);
}

void drawProgressScreen(const String& zoneName, int done, int total,
                        unsigned long elapsedMs, unsigned long totalMs) {
  int w = M5.Display.width();
  int h = M5.Display.height();

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextDatum(top_center);

  M5.Display.setTextSize(3);
  M5.Display.drawString(zoneName, w / 2, 20);

  M5.Display.setTextSize(2);
  M5.Display.drawString("Batch: " + String(done) + "/" + String(total), w / 2, 75);

  unsigned long remainingMs = (elapsedMs >= totalMs) ? 0 : (totalMs - elapsedMs);
  int remainingSec = (int)(remainingMs / 1000UL);
  int min = remainingSec / 60;
  int sec = remainingSec % 60;
  char buf[32];
  snprintf(buf, sizeof(buf), "Temps restant: %02d:%02d", min, sec);
  M5.Display.drawString(String(buf), w / 2, 115);

  int barX = 40;
  int barY = 165;
  int barW = w - 80;
  int barH = 35;

  M5.Display.drawRect(barX, barY, barW, barH, TFT_WHITE);

  float ratio = (totalMs == 0) ? 1.0f : ((float)elapsedMs / (float)totalMs);
  if (ratio < 0) ratio = 0;
  if (ratio > 1) ratio = 1;

  int fillW = (int)(barW * ratio);
  if (fillW > 2) M5.Display.fillRect(barX + 1, barY + 1, fillW - 2, barH - 2, TFT_GREEN);

  M5.Display.setTextSize(2);
  M5.Display.drawString("Deplace-toi dans la zone", w / 2, 225);

  M5.Display.setTextSize(1);
  M5.Display.drawString("Astuce: touche RESET pour stopper (annule batch)", w / 2, h - 25);
}

// ==================== RESET SD ====================
void resetAllData() {
  if (!checkSDCard()) { drawUI("SD absente"); return; }

  M5.Display.clear(TFT_MAROON);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(3);
  M5.Display.drawString("EFFACEMENT SD...", M5.Display.width() / 2, M5.Display.height() / 2);

  for (int i = 0; i < NUM_LOCATIONS; i++) {
    String filename = "/" + locationButtons[i].label + ".csv";
    if (SD.exists(filename)) SD.remove(filename);
  }

  total_samples = 0;
  selected_location = -1;
  resetZoneCounts();

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.drawString("SD VIDE !", M5.Display.width() / 2, M5.Display.height() / 2);
  delay(900);

  drawUI("SD vide");
}

// ==================== TOP 5 AP (scan & tri) ====================
// Renommé pour éviter conflit: sortByRssiDescAP / scanTopKAP
void sortByRssiDescAP(ApEntry* arr, int n) {
  for (int i = 0; i < n - 1; i++) {
    for (int j = 0; j < n - 1 - i; j++) {
      if (arr[j].rssi < arr[j + 1].rssi) {
        ApEntry tmp = arr[j];
        arr[j] = arr[j + 1];
        arr[j + 1] = tmp;
      }
    }
  }
}

int scanTopKAP(ApEntry outTop[TOP_K_AP]) {
  WiFi.scanDelete();
  int n = WiFi.scanNetworks(false, false, false, SCAN_MAX_MS_PER_CHAN);

  for (int k = 0; k < TOP_K_AP; k++) {
    outTop[k].mac = "";
    outTop[k].rssi = -200;
  }
  if (n <= 0) return 0;

  int cap = (n > 40) ? 40 : n;
  ApEntry tmp[40];

  for (int i = 0; i < cap; i++) {
    tmp[i].mac = WiFi.BSSIDstr(i);
    tmp[i].rssi = WiFi.RSSI(i);
  }

  if (cap > 1) sortByRssiDescAP(tmp, cap);

  int picked = 0;
  for (int i = 0; i < cap && picked < TOP_K_AP; i++) {
    bool dup = false;
    for (int j = 0; j < picked; j++) {
      if (outTop[j].mac == tmp[i].mac) { dup = true; break; }
    }
    if (dup) continue;
    outTop[picked++] = tmp[i];
  }
  return picked;
}

// ==================== COLLECTE CSV ====================
// 1 sample = Zone, mac1,rssi1, ... mac5,rssi5
bool collectOneSample(int zoneIndex) {
  if (!checkSDCard()) return false;

  struct Acc { String mac; float sum; int cnt; };
  Acc acc[TOP_K_AP * SAMPLES_PER_SAMPLE];
  int accN = 0;

  auto addPoint = [&](const String& mac, int rssi) {
    for (int i = 0; i < accN; i++) {
      if (acc[i].mac == mac) { acc[i].sum += rssi; acc[i].cnt++; return; }
    }
    if (accN < (TOP_K_AP * SAMPLES_PER_SAMPLE)) {
      acc[accN++] = {mac, (float)rssi, 1};
    }
  };

  for (int s = 0; s < SAMPLES_PER_SAMPLE; s++) {
    ApEntry top[TOP_K_AP];
    int got = scanTopKAP(top);
    for (int k = 0; k < got; k++) addPoint(top[k].mac, top[k].rssi);
    delay(SAMPLE_DELAY_MS);
  }

  ApEntry avg[TOP_K_AP * SAMPLES_PER_SAMPLE];
  int avgN = 0;
  for (int i = 0; i < accN; i++) {
    int meanRssi = (acc[i].cnt > 0) ? (int)(acc[i].sum / acc[i].cnt) : -200;
    avg[avgN++] = {acc[i].mac, meanRssi};
  }
  if (avgN > 1) sortByRssiDescAP(avg, avgN);

  int keep = (avgN < TOP_K_AP) ? avgN : TOP_K_AP;

  String filename = "/" + locationButtons[zoneIndex].label + ".csv";
  File f = SD.open(filename, FILE_APPEND);
  if (!f) return false;

  f.print(locationButtons[zoneIndex].label);
  for (int k = 0; k < TOP_K_AP; k++) {
    f.print(",");
    if (k < keep && avg[k].mac.length()) {
      f.print(avg[k].mac);
      f.print(",");
      f.print(avg[k].rssi);
    } else {
      f.print("NA");
      f.print(",");
      f.print(-100);
    }
  }
  f.println();
  f.close();

  total_samples++;
  zone_scan_count[zoneIndex]++;
  return true;
}

void collectSingle(int zoneIndex) {
  if (!checkSDCard()) { drawUI("SD absente"); return; }

  selected_location = zoneIndex;

  M5.Display.fillRect(0, M5.Display.height() / 2 - 50, M5.Display.width(), 100, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextSize(3);
  M5.Display.drawString("Scan...", M5.Display.width() / 2, M5.Display.height() / 2);

  collectOneSample(zoneIndex);
  drawUI("Scan ajoute");
}

void collectBatch(int zoneIndex) {
  if (!checkSDCard()) { drawUI("SD absente"); return; }

  selected_location = zoneIndex;

  const unsigned long totalMs = BATCH_TOTAL_MS;
  const int totalScans = BATCH_SCANS;
  const unsigned long intervalMs = totalMs / (unsigned long)totalScans;

  unsigned long startMs = millis();

  for (int k = 1; k <= totalScans; k++) {
    // annulation via RESET (sans effacer)
    M5.update();
    auto t = M5.Touch.getDetail();
    if (t.wasPressed()) {
      if (t.x > resetDataButton.x && t.x < resetDataButton.x + resetDataButton.w &&
          t.y > resetDataButton.y && t.y < resetDataButton.y + resetDataButton.h) {
        break;
      }
    }

    if (!checkSDCard()) { drawUI("SD absente"); return; }

    collectOneSample(zoneIndex);

    unsigned long nowMs = millis();
    unsigned long elapsedMs = nowMs - startMs;
    if (elapsedMs > totalMs) elapsedMs = totalMs;

    drawProgressScreen(locationButtons[zoneIndex].label, k, totalScans, elapsedMs, totalMs);

    unsigned long targetNext = startMs + (unsigned long)k * intervalMs;
    nowMs = millis();
    if (nowMs < targetNext) delay((int)(targetNext - nowMs));
  }

  drawUI("Batch termine");
}

// ==================== MQTT HELPERS ====================
bool connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long startWait = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startWait < 9000) delay(250);
  return (WiFi.status() == WL_CONNECTED);
}

bool connectMQTT() {
  wifiClient.setInsecure();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setBufferSize(16384);
  return mqttClient.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD);
}

// ==================== ENVOI DATASET (CSV -> JSON chunks) ====================
bool publishChunk(DynamicJsonDocument& doc) {
  String payload;
  serializeJson(doc, payload);
  return mqttClient.publish(TOPIC_DATA, payload.c_str());
}

void initChunkDoc(DynamicJsonDocument& doc, const String& loc) {
  doc.clear();
  doc["location"] = loc;
  doc.createNestedArray("samples");
}

void addSampleFromCsvLine(JsonArray samplesArray, const String& line) {
  int p = line.indexOf(',');
  if (p < 0) return;
  p += 1;

  JsonObject sObj = samplesArray.createNestedObject();
  JsonArray aps = sObj.createNestedArray("aps");

  for (int k = 0; k < TOP_K_AP; k++) {
    int c1 = line.indexOf(',', p);
    if (c1 < 0) break;
    String mac = line.substring(p, c1); mac.trim();
    p = c1 + 1;

    int c2 = line.indexOf(',', p);
    String rssiStr = (c2 < 0) ? line.substring(p) : line.substring(p, c2);
    rssiStr.trim();
    p = (c2 < 0) ? line.length() : (c2 + 1);

    if (mac == "NA" || mac.length() < 11) continue;

    JsonObject ap = aps.createNestedObject();
    ap["mac"] = mac;
    ap["rssi"] = rssiStr.toFloat();
  }
}

void syncMQTT() {
  if (!checkSDCard()) { drawUI("SD absente"); return; }

  drawUI("Connexion WiFi...");
  if (!connectWiFi()) {
    drawUI("ECHEC WIFI");
    delay(1200);
    drawUI();
    return;
  }

  drawUI("Connexion MQTT...");
  if (!connectMQTT()) {
    mqttClient.disconnect();
    WiFi.disconnect();
    drawUI("ECHEC MQTT");
    delay(1200);
    drawUI();
    return;
  }

  for (int zone = 0; zone < NUM_LOCATIONS; zone++) {
    String loc = locationButtons[zone].label;
    String filename = "/" + loc + ".csv";
    if (!SD.exists(filename)) continue;

    File f = SD.open(filename);
    if (!f) continue;

    DynamicJsonDocument doc(8192);
    initChunkDoc(doc, loc);
    JsonArray samplesArray = doc["samples"].as<JsonArray>();
    int chunkCount = 0;

    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() < 5) continue;

      addSampleFromCsvLine(samplesArray, line);
      chunkCount++;

      if (chunkCount >= CHUNK_SAMPLES) {
        if (!publishChunk(doc)) {
          f.close();
          mqttClient.publish(TOPIC_STATUS, "PUBLISH_FAIL");
          mqttClient.disconnect();
          WiFi.disconnect();
          drawUI("PUBLISH_FAIL");
          return;
        }
        delay(120);
        initChunkDoc(doc, loc);
        samplesArray = doc["samples"].as<JsonArray>();
        chunkCount = 0;
      }
    }

    if (chunkCount > 0) {
      if (!publishChunk(doc)) {
        f.close();
        mqttClient.publish(TOPIC_STATUS, "PUBLISH_FAIL");
        mqttClient.disconnect();
        WiFi.disconnect();
        drawUI("PUBLISH_FAIL");
        return;
      }
      delay(120);
    }

    f.close();
  }

  mqttClient.publish(TOPIC_STATUS, "SUCCESS");
  mqttClient.disconnect();
  WiFi.disconnect();

  resetZoneCounts();
  selected_location = -1;
  drawUI("Dataset envoye");
  delay(800);
  drawUI();
}

// ==================== SETUP & LOOP ====================
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(3);
  Serial.begin(115200);

  if (!SD.begin()) {
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(3);
    M5.Display.drawString("ERREUR SD", M5.Display.width() / 2, M5.Display.height() / 2);
    while (1);
  }

  WiFi.setPins(12, 13, 11, 10, 9, 8, 15);

  resetZoneCounts();
  initButtons();
  drawUI("Pret");
}

void loop() {
  M5.update();
  auto t = M5.Touch.getDetail();
  if (!t.wasPressed()) return;

  // MODE
  if (t.x > modeButton.x && t.x < modeButton.x + modeButton.w &&
      t.y > modeButton.y && t.y < modeButton.y + modeButton.h) {
    currentMode = (currentMode == MODE_SINGLE) ? MODE_BATCH : MODE_SINGLE;
    drawUI();
    return;
  }

  // RESET SD
  if (t.x > resetDataButton.x && t.x < resetDataButton.x + resetDataButton.w &&
      t.y > resetDataButton.y && t.y < resetDataButton.y + resetDataButton.h) {
    resetAllData();
    return;
  }

  // Zones
  for (int i = 0; i < NUM_LOCATIONS; i++) {
    if (t.x > locationButtons[i].x && t.x < locationButtons[i].x + locationButtons[i].w &&
        t.y > locationButtons[i].y && t.y < locationButtons[i].y + locationButtons[i].h) {
      if (currentMode == MODE_SINGLE) collectSingle(i);
      else collectBatch(i);
      return;
    }
  }

  // ENVOYER DATASET
  if (t.x > sendMqttButton.x && t.x < sendMqttButton.x + sendMqttButton.w &&
      t.y > sendMqttButton.y && t.y < sendMqttButton.y + sendMqttButton.h) {
    syncMQTT();
    return;
  }
}
