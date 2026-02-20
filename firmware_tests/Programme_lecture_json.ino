#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <math.h>

// ======================================================
// HYBRID B — COLLECTE + ENVOI MQTT + IA EMBARQUÉE
// - Tablette envoie TOP 5 AP par scan (MAC + RSSI)
// - Colab entraîne un modèle sur TOP 15 MAC globales
// - Ici: scan TOP5 -> reconstruit vecteur 15 via ap_order modèle -> prédiction
// ======================================================

// ==================== WIFI & MQTT ====================
#define WIFI_SSID "Wifi 42"
#define WIFI_PASSWORD "61454300"

#define MQTT_BROKER "2bcf898148934fe9b985e2edd298658a.s1.eu.hivemq.cloud"
#define MQTT_PORT 8883
#define MQTT_USERNAME "Asmaa"
#define MQTT_PASSWORD "07121416Li"
#define MQTT_CLIENT_ID "Asmaa"

#define TOPIC_DATA   "LaVilette/data"
#define TOPIC_STATUS "LaVilette/status"
#define TOPIC_PRED   "LaVilette/pred"

// ==================== MODEL FILE ====================
#define MODEL_PATH "/nn_weights.json"

// ==================== ZONES ====================
#define NUM_LOCATIONS 4

// ==================== TOP-K SCAN ====================
#define TOP_K_AP 5

// Collecte: 1 sample = moyenne de plusieurs scans rapides
#define SAMPLES_PER_SAMPLE 2
#define SAMPLE_DELAY_MS 80
#define SCAN_MAX_MS_PER_CHAN 90

// Batch
#define BATCH_SCANS 50
#define BATCH_TOTAL_MS 180000UL // 3 minutes

// Envoi dataset en morceaux
#define CHUNK_SAMPLES 5

// Prédiction: moyenne de plusieurs scans
#define SAMPLES_PER_PRED 3
#define PRED_DELAY_MS 120

// ==================== MODEL SHAPES (fixes pour ton projet) ====================
#define INPUT_SIZE 15
#define HIDDEN_SIZE 16

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
TouchButton predictButton;

// ==================== MQTT ====================
WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

// ==================== MODES ====================
enum ScanMode { MODE_SINGLE = 0, MODE_BATCH = 1 };
ScanMode currentMode = MODE_SINGLE;

// ==================== VARIABLES ====================
int selected_location = -1;
int total_samples = 0;
int zone_scan_count[NUM_LOCATIONS] = {0};

// ==================== TOP-K AP struct ====================
struct ApEntry {
  String mac;
  int rssi;
};

// ==================== MODEL STORAGE ====================
static bool modelLoaded = false;

static float W1[INPUT_SIZE][HIDDEN_SIZE];
static float b1[HIDDEN_SIZE];
static float W2[HIDDEN_SIZE][NUM_LOCATIONS];
static float b2[NUM_LOCATIONS];
static float mean_[INPUT_SIZE];
static float std_[INPUT_SIZE];

static String ap_order_model[INPUT_SIZE];         // TOP15 MAC depuis JSON
static String loc_names[NUM_LOCATIONS];           // noms zones depuis JSON

// ======================================================
// UTILITAIRES
// ======================================================
String scanLabel(int n) { return (n == 1) ? "1 scan" : String(n) + " scans"; }
String modeLabel() { return (currentMode == MODE_SINGLE) ? "MODE: 1x" : "MODE: 50x/3min"; }

void resetZoneCounts() {
  for (int i = 0; i < NUM_LOCATIONS; i++) zone_scan_count[i] = 0;
}

// ======================================================
// SD CHECK
// ======================================================
bool checkSDCard() {
  if (!SD.begin()) {
    M5.Display.fillScreen(TFT_RED);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(3);
    M5.Display.drawString("CARTE SD ABSENTE !", M5.Display.width()/2, M5.Display.height()/2);
    delay(1500);
    return false;
  }
  return true;
}

// ======================================================
// UI
// ======================================================
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
  int rows = (NUM_LOCATIONS + 1) / 2; // 4 zones -> 2 rows

  int bw = (screenW - 2 * margin - (cols - 1) * gapX) / cols;
  int bh = (zonesH - (rows - 1) * gapY) / rows;

  for (int i = 0; i < NUM_LOCATIONS; i++) {
    int col = i % 2;
    int row = i / 2;

    String shown = "Zone " + String(i + 1);
    if (modelLoaded && loc_names[i].length() > 0) shown = loc_names[i];

    locationButtons[i] = {
      margin + col * (bw + gapX),
      zonesTop + row * (bh + gapY),
      bw, bh,
      shown,
      TFT_DARKGREY
    };
  }

  // 2 gros boutons en bas
  int bigW = (screenW - 2 * margin - gapX) / 2;
  int bigH = 120;
  int y = screenH - bigH - margin;

  predictButton = { margin, y, bigW, bigH, "PREDIRE", TFT_ORANGE };
  sendMqttButton = { margin + bigW + gapX, y, bigW, bigH, "ENVOYER\nHIVEMQ", TFT_BLUE };

  resetDataButton = { screenW - margin - 190, 10, 190, 60, "RESET SD", TFT_RED };
  modeButton = { margin, 10, 230, 60, "MODE", TFT_DARKCYAN };
}

void drawUI(const String& statusLine = "") {
  int screenW = M5.Display.width();
  M5.Display.clear(TFT_BLACK);

  M5.Display.setTextDatum(top_center);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextSize(2);
  M5.Display.drawString("COLLECTE + IA EMBARQUEE", screenW/2, 10);

  // MODE
  M5.Display.fillRoundRect(modeButton.x, modeButton.y, modeButton.w, modeButton.h, 10, modeButton.color);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  M5.Display.drawString(modeLabel(), modeButton.x + modeButton.w/2, modeButton.y + modeButton.h/2);

  // RESET
  M5.Display.fillRoundRect(resetDataButton.x, resetDataButton.y, resetDataButton.w, resetDataButton.h, 10, resetDataButton.color);
  M5.Display.drawString(resetDataButton.label,
                        resetDataButton.x + resetDataButton.w/2,
                        resetDataButton.y + resetDataButton.h/2);

  // status
  if (statusLine.length() > 0) {
    M5.Display.setTextDatum(top_center);
    M5.Display.setTextSize(1);
    M5.Display.drawString(statusLine, screenW/2, 55);
  }

  // ZONES
  for (int i = 0; i < NUM_LOCATIONS; i++) {
    uint16_t col = (i == selected_location) ? TFT_GREEN : locationButtons[i].color;

    M5.Display.fillRoundRect(locationButtons[i].x, locationButtons[i].y,
                             locationButtons[i].w, locationButtons[i].h, 12, col);

    int cx = locationButtons[i].x + locationButtons[i].w/2;
    int cy = locationButtons[i].y + locationButtons[i].h/2;

    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(2);
    M5.Display.drawString(locationButtons[i].label, cx, cy - 16);
    M5.Display.setTextSize(1);
    M5.Display.drawString(scanLabel(zone_scan_count[i]), cx, cy + 18);
  }

  // PREDIRE
  M5.Display.fillRoundRect(predictButton.x, predictButton.y, predictButton.w, predictButton.h, 14, predictButton.color);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  M5.Display.drawString(predictButton.label, predictButton.x + predictButton.w/2, predictButton.y + predictButton.h/2);

  // ENVOYER
  M5.Display.fillRoundRect(sendMqttButton.x, sendMqttButton.y, sendMqttButton.w, sendMqttButton.h, 14, sendMqttButton.color);
  M5.Display.drawString(sendMqttButton.label,
                        sendMqttButton.x + sendMqttButton.w/2,
                        sendMqttButton.y + sendMqttButton.h/2);
}

void drawProgressScreen(const String& zoneName, int done, int total,
                        unsigned long elapsedMs, unsigned long totalMs) {
  int w = M5.Display.width();
  int h = M5.Display.height();

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextDatum(top_center);

  M5.Display.setTextSize(3);
  M5.Display.drawString(zoneName, w/2, 20);

  M5.Display.setTextSize(2);
  M5.Display.drawString("Batch: " + String(done) + "/" + String(total), w/2, 75);

  unsigned long remainingMs = (elapsedMs >= totalMs) ? 0 : (totalMs - elapsedMs);
  int remainingSec = (int)(remainingMs / 1000UL);
  int min = remainingSec / 60;
  int sec = remainingSec % 60;

  char buf[32];
  snprintf(buf, sizeof(buf), "Temps restant: %02d:%02d", min, sec);
  M5.Display.drawString(String(buf), w/2, 115);

  int barX = 40, barY = 165, barW = w - 80, barH = 35;
  M5.Display.drawRect(barX, barY, barW, barH, TFT_WHITE);

  float ratio = (totalMs == 0) ? 1.0f : ((float)elapsedMs / (float)totalMs);
  if (ratio < 0) ratio = 0;
  if (ratio > 1) ratio = 1;

  int fillW = (int)(barW * ratio);
  if (fillW > 2) M5.Display.fillRect(barX + 1, barY + 1, fillW - 2, barH - 2, TFT_GREEN);

  M5.Display.setTextSize(2);
  M5.Display.drawString("Deplace-toi dans la zone", w/2, 225);
  M5.Display.setTextSize(1);
  M5.Display.drawString("Astuce: touche RESET pour stopper (annule batch)", w/2, h - 25);
}

// ======================================================
// TOP-K Scan helpers
// ======================================================
void sortByRssiDesc(ApEntry* arr, int n) {
  for (int i = 0; i < n - 1; i++) {
    for (int j = 0; j < n - 1 - i; j++) {
      if (arr[j].rssi < arr[j+1].rssi) {
        ApEntry tmp = arr[j];
        arr[j] = arr[j+1];
        arr[j+1] = tmp;
      }
    }
  }
}

int scanTopK(ApEntry outTop[TOP_K_AP]) {
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

  if (cap > 1) sortByRssiDesc(tmp, cap);

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

// ======================================================
// RESET SD (efface CSV)
// ======================================================
void resetAllData() {
  if (!checkSDCard()) { drawUI("SD absente"); return; }

  M5.Display.clear(TFT_MAROON);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(3);
  M5.Display.drawString("EFFACEMENT SD...", M5.Display.width()/2, M5.Display.height()/2);

  for (int i = 0; i < NUM_LOCATIONS; i++) {
    String filename = "/" + String("Zone ") + String(i+1) + ".csv";
    if (SD.exists(filename)) SD.remove(filename);
  }

  total_samples = 0;
  selected_location = -1;
  resetZoneCounts();

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.drawString("SD VIDE !", M5.Display.width()/2, M5.Display.height()/2);
  delay(900);

  drawUI("SD vide");
}

// ======================================================
// COLLECTE CSV (TOP5 MAC+RSSI)
// Format ligne:
// Zone X,mac1,rssi1,mac2,rssi2,mac3,rssi3,mac4,rssi4,mac5,rssi5
// ======================================================
bool collectOneSample(int zoneIndex) {
  if (!checkSDCard()) return false;

  // Accumulate MAC over multiple scans
  struct Acc { String mac; float sum; int cnt; };
  Acc acc[TOP_K_AP * SAMPLES_PER_SAMPLE];
  int accN = 0;

  auto addPoint = [&](const String& mac, int rssi) {
    for (int i = 0; i < accN; i++) {
      if (acc[i].mac == mac) { acc[i].sum += rssi; acc[i].cnt++; return; }
    }
    if (accN < (TOP_K_AP * SAMPLES_PER_SAMPLE)) acc[accN++] = {mac, (float)rssi, 1};
  };

  for (int s = 0; s < SAMPLES_PER_SAMPLE; s++) {
    ApEntry top[TOP_K_AP];
    int got = scanTopK(top);
    for (int k = 0; k < got; k++) addPoint(top[k].mac, top[k].rssi);
    delay(SAMPLE_DELAY_MS);
  }

  // Average and keep TOP5 by RSSI
  ApEntry avg[TOP_K_AP * SAMPLES_PER_SAMPLE];
  int avgN = 0;
  for (int i = 0; i < accN; i++) {
    int meanRssi = (acc[i].cnt > 0) ? (int)(acc[i].sum / acc[i].cnt) : -200;
    avg[avgN++] = {acc[i].mac, meanRssi};
  }
  if (avgN > 1) sortByRssiDesc(avg, avgN);

  int keep = (avgN < TOP_K_AP) ? avgN : TOP_K_AP;

  String filename = "/" + String("Zone ") + String(zoneIndex+1) + ".csv";
  File f = SD.open(filename, FILE_APPEND);
  if (!f) return false;

  f.print(String("Zone ") + String(zoneIndex+1));
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

  M5.Display.fillRect(0, M5.Display.height()/2 - 50, M5.Display.width(), 100, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextSize(3);
  M5.Display.drawString("Scan...", M5.Display.width()/2, M5.Display.height()/2);

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
    // cancel if press RESET
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

    drawProgressScreen(String("Zone ") + String(zoneIndex+1), k, totalScans, elapsedMs, totalMs);

    unsigned long targetNext = startMs + (unsigned long)k * intervalMs;
    nowMs = millis();
    if (nowMs < targetNext) delay((int)(targetNext - nowMs));
  }

  drawUI("Batch termine");
}

// ======================================================
// MQTT helpers
// ======================================================
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

// ======================================================
// Dataset publish (CSV -> JSON chunks)
// Format Colab attendu: {"location":"Zone 1","samples":[{"aps":[{"mac":..,"rssi":..}, ...]} ...]}
// ======================================================
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
  if (!connectWiFi()) { drawUI("ECHEC WIFI"); delay(1200); drawUI(); return; }

  drawUI("Connexion MQTT...");
  if (!connectMQTT()) { mqttClient.disconnect(); WiFi.disconnect(); drawUI("ECHEC MQTT"); delay(1200); drawUI(); return; }

  for (int zone = 0; zone < NUM_LOCATIONS; zone++) {
    String loc = String("Zone ") + String(zone+1);
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

// ======================================================
// MODEL LOAD from SD
// ======================================================
bool loadModelFromSD(const char* path = MODEL_PATH) {
  if (!checkSDCard()) return false;

  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.println(String("Model: introuvable ") + path);
    return false;
  }

  // JSON doc size: dépend de tes poids. 90k est souvent OK sur ESP32.
  DynamicJsonDocument doc(90000);
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    Serial.print("Model: JSON error: ");
    Serial.println(err.c_str());
    return false;
  }

  int inSize  = doc["input_size"]  | 0;
  int hidSize = doc["hidden_size"] | 0;
  int outSize = doc["output_size"] | 0;

  if (inSize != INPUT_SIZE || hidSize != HIDDEN_SIZE || outSize != NUM_LOCATIONS) {
    Serial.println("Model: tailles inattendues");
    return false;
  }

  for (int i = 0; i < INPUT_SIZE; i++) {
    mean_[i] = doc["mean"][i].as<float>();
    std_[i]  = doc["std"][i].as<float>();
    ap_order_model[i] = String((const char*)doc["ap_order"][i]);
  }

  for (int k = 0; k < NUM_LOCATIONS; k++) {
    loc_names[k] = String((const char*)doc["locations"][k]);
  }

  for (int i = 0; i < INPUT_SIZE; i++)
    for (int j = 0; j < HIDDEN_SIZE; j++)
      W1[i][j] = doc["W1"][i][j].as<float>();

  for (int j = 0; j < HIDDEN_SIZE; j++)
    b1[j] = doc["b1"][j].as<float>();

  for (int j = 0; j < HIDDEN_SIZE; j++)
    for (int k = 0; k < NUM_LOCATIONS; k++)
      W2[j][k] = doc["W2"][j][k].as<float>();

  for (int k = 0; k < NUM_LOCATIONS; k++)
    b2[k] = doc["b2"][k].as<float>();

  Serial.println("Model: charge OK");
  return true;
}

// ======================================================
// ANN inference
// ======================================================
static inline float relu(float x) { return x > 0 ? x : 0; }

static void softmax(float* z, int n) {
  float maxv = z[0];
  for (int i = 1; i < n; i++) if (z[i] > maxv) maxv = z[i];
  float sum = 0;
  for (int i = 0; i < n; i++) { z[i] = expf(z[i] - maxv); sum += z[i]; }
  if (sum < 1e-9f) sum = 1e-9f;
  for (int i = 0; i < n; i++) z[i] /= sum;
}

int predictANN(const float x_raw[INPUT_SIZE], float &conf_out) {
  // normalize
  float x[INPUT_SIZE];
  for (int i = 0; i < INPUT_SIZE; i++) {
    float s = (fabsf(std_[i]) < 1e-6f) ? 1.0f : std_[i];
    x[i] = (x_raw[i] - mean_[i]) / s;
  }

  // hidden
  float h[HIDDEN_SIZE];
  for (int j = 0; j < HIDDEN_SIZE; j++) {
    float acc = b1[j];
    for (int i = 0; i < INPUT_SIZE; i++) acc += x[i] * W1[i][j];
    h[j] = relu(acc);
  }

  // output
  float y[NUM_LOCATIONS];
  for (int k = 0; k < NUM_LOCATIONS; k++) {
    float acc = b2[k];
    for (int j = 0; j < HIDDEN_SIZE; j++) acc += h[j] * W2[j][k];
    y[k] = acc;
  }

  softmax(y, NUM_LOCATIONS);

  int best = 0;
  for (int k = 1; k < NUM_LOCATIONS; k++) if (y[k] > y[best]) best = k;
  conf_out = y[best];
  return best;
}

// construit vecteur 15 depuis scans TOP5 (moyennés) + ap_order_model
void buildInputAveraged(float x_out[INPUT_SIZE]) {
  float sum[INPUT_SIZE];
  int cnt[INPUT_SIZE];
  for (int i = 0; i < INPUT_SIZE; i++) { sum[i] = 0.0f; cnt[i] = 0; }

  for (int s = 0; s < SAMPLES_PER_PRED; s++) {
    ApEntry top[TOP_K_AP];
    int got = scanTopK(top);

    for (int k = 0; k < got; k++) {
      String mac = top[k].mac;
      int rssi = top[k].rssi;

      // si ce mac est dans ap_order_model, on accumule
      for (int i = 0; i < INPUT_SIZE; i++) {
        if (ap_order_model[i] == mac) {
          sum[i] += (float)rssi;
          cnt[i] += 1;
          break;
        }
      }
    }
    delay(PRED_DELAY_MS);
  }

  for (int i = 0; i < INPUT_SIZE; i++) {
    x_out[i] = (cnt[i] > 0) ? (sum[i] / (float)cnt[i]) : -100.0f;
  }
}

void publishPrediction(const String& zoneName, float conf) {
  DynamicJsonDocument doc(256);
  doc["device_id"] = "tab5_01";
  doc["zone"] = zoneName;
  doc["confidence"] = conf;

  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(TOPIC_PRED, payload.c_str());
}

// prédire + afficher + envoyer MQTT
void doPredictAndSend() {
  if (!modelLoaded) {
    drawUI("Modele non charge (/nn_weights.json)");
    delay(1200);
    drawUI();
    return;
  }

  drawUI("Scan WiFi (prediction)...");
  float x_raw[INPUT_SIZE];
  buildInputAveraged(x_raw);

  float conf = 0.0f;
  int idx = predictANN(x_raw, conf);

  String zoneName = (idx >= 0 && idx < NUM_LOCATIONS) ? loc_names[idx] : String("Zone ?");
  if (zoneName.length() == 0) zoneName = String("Zone ") + String(idx + 1);

  // Affichage résultat
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(TFT_WHITE);

  M5.Display.setTextSize(3);
  M5.Display.drawString(zoneName, M5.Display.width()/2, M5.Display.height()/2 - 25);

  M5.Display.setTextSize(2);
  M5.Display.drawString("Conf: " + String(conf, 2), M5.Display.width()/2, M5.Display.height()/2 + 25);

  // Envoi MQTT
  if (!connectWiFi()) {
    drawUI("ECHEC WIFI");
    delay(1000);
    drawUI();
    return;
  }
  if (!connectMQTT()) {
    mqttClient.disconnect();
    WiFi.disconnect();
    drawUI("ECHEC MQTT");
    delay(1000);
    drawUI();
    return;
  }

  publishPrediction(zoneName, conf);
  mqttClient.publish(TOPIC_STATUS, "PRED_SENT");

  mqttClient.disconnect();
  WiFi.disconnect();

  delay(900);
  drawUI("Prediction envoyee");
  delay(600);
  drawUI();
}

// ======================================================
// SETUP & LOOP
// ======================================================
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(3);
  Serial.begin(115200);

  if (!SD.begin()) {
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(3);
    M5.Display.drawString("ERREUR SD", M5.Display.width()/2, M5.Display.height()/2);
    while (1);
  }

  WiFi.setPins(12, 13, 11, 10, 9, 8, 15);

  modelLoaded = loadModelFromSD(MODEL_PATH);

  resetZoneCounts();
  initButtons();
  drawUI(modelLoaded ? "Modele OK" : "Modele KO (mettre /nn_weights.json)");
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

  // ZONES (collecte)
  for (int i = 0; i < NUM_LOCATIONS; i++) {
    if (t.x > locationButtons[i].x && t.x < locationButtons[i].x + locationButtons[i].w &&
        t.y > locationButtons[i].y && t.y < locationButtons[i].y + locationButtons[i].h) {
      selected_location = i;
      if (currentMode == MODE_SINGLE) collectSingle(i);
      else collectBatch(i);
      return;
    }
  }

  // PREDIRE
  if (t.x > predictButton.x && t.x < predictButton.x + predictButton.w &&
      t.y > predictButton.y && t.y < predictButton.y + predictButton.h) {
    doPredictAndSend();
    return;
  }

  // ENVOYER DATASET
  if (t.x > sendMqttButton.x && t.x < sendMqttButton.x + sendMqttButton.w &&
      t.y > sendMqttButton.y && t.y < sendMqttButton.y + sendMqttButton.h) {
    syncMQTT();
    return;
  }
}