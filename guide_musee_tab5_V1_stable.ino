/*
 * Museum Guide Tab5 - WiFi geolocation (NN) + PNG depuis SD + mode manuel (TACTILE)
 *
 * ✅ Modifs demandées :
 * - Au démarrage : affiche directement Zone 1
 * - Plus de boutons A/B/C (Tab5 = tactile)
 * - Ajout de boutons "user friendly" en bas à gauche :
 *     [◀]  [AUTO/MANUEL]  [▶]
 * - En AUTO : scan WiFi -> NN -> change la zone si confiance >= seuil
 * - En MANUEL : ◀ / ▶ change la zone (forcée)
 * - Texte discret "En attente d'une zone (AUTO)..." si pas encore de zone détectée
 *
 * SD init : SD.begin() simple (sans SPI pins), blocage si SD KO
 * PNG : SD.open() -> FileDataWrapper -> M5.Display.drawPng(&dw,0,0)
 */

#include <M5Unified.h>
#include <WiFi.h>
#include <SD.h>
#include <math.h>
#include <lgfx/v1/misc/DataWrapper.hpp>

// ==================== CONFIG ====================
#define SCAN_INTERVAL_MS        3000
#define CONFIDENCE_THRESHOLD    0.60

// Résolution Tab5 typique
static const int SCREEN_W = 1280;
static const int SCREEN_H = 720;

// Zone texte droite 560x720
static const int TEXT_X = 720, TEXT_Y = 0, TEXT_W = 560, TEXT_H = 720;
static const int PAD = 24;

bool sd_ok = false;

// ==================== MODE ====================
enum RunMode { MODE_AUTO = 0, MODE_MANUAL = 1 };
RunMode runMode = MODE_AUTO;

// Index zone affichée
int current_zone_idx = 0;
float current_confidence = 0.0f;

// Indique si on a déjà eu une prédiction AUTO "valide"
bool has_auto_fix = false;

// ==================== UI BUTTONS (bottom-left) ====================
struct BtnRect {
  int x, y, w, h;
};

static const int BTN_H = 70;
static const int BTN_Y = SCREEN_H - BTN_H - 16;
static const int BTN_X = 16;
static const int BTN_GAP = 12;

static const int BTN_PREV_W = 90;
static const int BTN_MODE_W = 220;
static const int BTN_NEXT_W = 90;

BtnRect btnPrev { BTN_X,                               BTN_Y, BTN_PREV_W, BTN_H };
BtnRect btnMode { BTN_X + BTN_PREV_W + BTN_GAP,        BTN_Y, BTN_MODE_W, BTN_H };
BtnRect btnNext { BTN_X + BTN_PREV_W + BTN_GAP + BTN_MODE_W + BTN_GAP, BTN_Y, BTN_NEXT_W, BTN_H };

// Touch debounce
unsigned long last_touch_ms = 0;
static const unsigned long TOUCH_DEBOUNCE_MS = 180;

// ==================== NN FROM COLAB ====================
const int INPUT_SIZE  = 10;
const int HIDDEN_SIZE = 16;
const int OUTPUT_SIZE = 4;

const char* AP_MACS[INPUT_SIZE] = {
  "FE:B5:B5:CB:A5:CA",
  "EE:2E:F4:DA:C7:58",
  "3E:5B:88:C7:F1:62",
  "82:9F:F5:13:4A:77",
  "C2:69:47:51:C4:C4",
  "B0:1F:8C:1F:78:C0",
  "B0:1F:8C:1F:78:C4",
  "9A:C2:18:DC:D6:08",
  "00:00:00:00:00:00",
  "00:00:00:00:00:00"
};

const char* LOCATION_NAMES[OUTPUT_SIZE] = { "Zone 1", "Zone 2", "Zone 3", "Zone 4" };

// ====== Contenu musée (à éditer) ======
const char* IMAGE_PATHS[OUTPUT_SIZE] = {
  "/museum/zone1.png",
  "/museum/zone2.png",
  "/museum/zone3.png",
  "/museum/zone4.png"
};

const char* TITRE[OUTPUT_SIZE] = {
  "TITRE OEUVRE ZONE 1",
  "TITRE OEUVRE ZONE 2",
  "TITRE OEUVRE ZONE 3",
  "TITRE OEUVRE ZONE 4"
};

const char* AUTEUR[OUTPUT_SIZE] = { "Auteur 1", "Auteur 2", "Auteur 3", "Auteur 4" };
const char* ANNEE[OUTPUT_SIZE]  = { "Annee 1",  "Annee 2",  "Annee 3",  "Annee 4"  };

const char* DESCRIPTION[OUTPUT_SIZE] = {
  "Description de l'oeuvre en Zone 1.",
  "Description de l'oeuvre en Zone 2.",
  "Description de l'oeuvre en Zone 3.",
  "Description de l'oeuvre en Zone 4."
};

// mean/std
const float MEAN[INPUT_SIZE] = {
  -60.867867867867865, -73.26276276276276, -54.048048048048045, -69.22072072072072,
  -80.14714714714715,  -80.63663663663664, -81.95945945945945,  -82.86786786786787,
  -98.36036036036036,  -98.36036036036036
};

const float STD[INPUT_SIZE] = {
  22.120112068352512, 14.790504853066478, 17.822214369115642, 11.6067996829428,
  11.946677046854118, 11.870868401920509, 11.499471551078432, 11.051433968101597,
  4.2148588713711135, 4.2148588713711135
};

// W1
const float W1[INPUT_SIZE][HIDDEN_SIZE] = {
  {-0.23280641436576843, 0.1235593631863594, -0.4433400332927704, -0.31952571868896484, -0.7450648546218872, 0.5047873854637146, -0.2114638388156891, 0.7147148251533508, -0.5704189538955688, -0.29400160908699036, 0.31670257449150085, -0.30012020468711853, -0.7718992233276367, 0.022038938477635384, 0.00820642989128828, 0.19199565052986145},
  {0.2533307373523712, -0.47635605931282043, -0.3644878566265106, 0.892842710018158, 0.48150521516799927, 0.17269861698150635, -0.34745559096336365, -0.19901253283023834, -0.24343150854110718, -0.33536675572395325, -0.3527391254901886, -0.3474438488483429, 0.006565570831298828, -0.3071059286594391, -0.09882216900587082, -0.25768300890922546},
  {0.2951170802116394, -0.19271409511566162, 0.0005905199213884771, -0.13946421444416046, 0.635898232460022, -0.14975817501544952, 0.5210273265838623, -0.2668771743774414, 0.178350031375885, 0.24213311076164246, 0.18250826001167297, -0.12827013432979584, 0.2729359567165375, -0.21007876098155975, 0.51520836353302, -0.3660651445388794},
  {0.03580034524202347, -0.07982528954744339, -0.3923884332180023, 0.2332584410905838, -0.0687151625752449, 0.640544056892395, -0.39437928795814514, 0.7415715456008911, 0.40463197231292725, -0.6056308746337891, 0.4078812599182129, 0.11900871247053146, -0.3705845773220062, 0.8833763599395752, -0.3670879900455475, 0.19321061670780182},
  {0.0733659490942955, 0.2652904689311981, -0.207377627491951, 0.14183107018470764, -0.05354816094040871, 0.300992488861084, 0.2717365324497223, -0.11721841990947723, 0.20703622698783875, -0.14449216425418854, 0.15597014129161835, -0.28390637040138245, -0.44322338700294495, -0.19110329449176788, -0.4978348910808563, 0.17702005803585052},
  {0.6256604194641113, -0.373702734708786, -0.4998164474964142, -0.11329787969589233, 0.49355512857437134, 0.05522272735834122, -0.37934231758117676, 0.26049935817718506, -0.05338841304183006, 0.3844115734100342, -0.15306736528873444, -0.054007433354854584, -0.11788644641637802, 0.0074534108862280846, -0.0018310628365725279, -0.1494016796350479},
  {0.37972551584243774, -0.16692055761814117, -0.019647853448987007, 0.09833762794733047, 0.28430649638175964, 0.6532077193260193, -0.1585969626903534, 0.12728027999401093, 0.04360257461667061, 0.037427425384521484, -0.6500022411346436, 0.10073776543140411, 0.2690258026123047, 0.19378097355365753, -0.5615188479423523, -0.5362963676452637},
  {0.8358832597732544, -0.04773624613881111, 0.06069439277052879, 0.13304917514324188, -0.3938778042793274, 0.21671681106090546, 0.27181294560432434, 0.5110493898391724, 0.3124698996543884, -0.38256117701530457, -0.5676899552345276, -0.4185382127761841, -0.32709193229675293, 0.2155478596687317, -0.41717439889907837, -0.6446773409843445},
  {-0.452338308095932, -0.43743062019348145, 0.23894107341766357, 0.5053480267524719, 0.4054903984069824, 0.11438652873039246, 0.25981733202934265, -0.17098070681095123, 0.11808785051107407, 0.5932022333145142, -0.6779071092605591, -0.27480053901672363, -0.8726063966751099, 0.016422195360064507, -0.06288350373506546, -0.8909552097320557},
  {-0.1725054234266281, -0.2474316656589508, -0.12262440472841263, 0.003430540207773447, 0.33841362595558167, -0.42337873578071594, 0.05897413194179535, -0.15735696256160736, 0.4536992311477661, 0.6115760803222656, 0.14927591383457184, 0.18627244234085083, -0.4274643659591675, -0.036824632436037064, -0.24071188271045685, -0.5565412044525146}
};

// b1
const float b1[HIDDEN_SIZE] = {
  0.30017295479774475, 0.2939002215862274, 0.08364799618721008, 0.24640117585659027,
  0.41379210352897644, 0.15975011885166168, -0.016374187543988228, -0.10036703199148178,
  0.10719136893749237, -0.03706509619951248, 0.3898361921310425, 0.20261207222938538,
  0.3990870714187622, -0.08037443459033966, -0.2619522511959076, 0.49024054408073425
};

// W2
const float W2[HIDDEN_SIZE][OUTPUT_SIZE] = {
  {0.462940514087677, -0.5295335054397583, 0.27467280626296997, -0.32878777384757996},
  {-0.7613614797592163, 0.25121891498565674, 0.13841797411441803, 0.5197262167930603},
  {0.04154235124588013, 0.4446900188922882, -0.8461624979972839, 0.0916922464966774},
  {0.6022971272468567, -0.339897483587265, 0.4967132806777954, -0.33729079365730286},
  {-0.6587954759597778, -0.5308098793029785, 0.9977020025253296, 0.18348658084869385},
  {0.8575262427330017, 0.40465089678764343, -0.48403066396713257, -0.968302845954895},
  {-0.6243366003036499, 0.34218162298202515, -0.47099238634109497, 0.3384547531604767},
  {0.06389488279819489, 0.6619694828987122, -0.49582117795944214, -0.306430846452713},
  {-0.4137246608734131, -0.3036216199398041, 0.3978024125099182, 0.4633042514324188},
  {0.15407761931419373, 0.29965630173683167, 0.6142851114273071, -0.7976164817810059},
  {-0.21101737022399902, 0.6938064098358154, -0.4145694673061371, 0.6182035207748413},
  {0.14185018837451935, -0.20471768081188202, -0.001101127010770142, 0.4567193388938904},
  {-0.22702263295650482, 0.5286586880683899, 0.46633684635162354, 0.5089607834815979},
  {-0.22350433468818665, 0.14642615616321564, -0.17596811056137085, -0.378833532333374},
  {0.31190240383148193, 0.5668461918830872, -0.6500921845436096, -0.07537133246660233},
  {-0.4669325351715088, 0.10407599806785583, -0.18808670341968536, 0.4411890208721161}
};

// b2
const float b2[OUTPUT_SIZE] = {
  -0.10394469648599625, -0.21854086220264435, 0.2691815197467804, 0.10999491065740585
};

// ==================== DataWrapper (PNG) ====================
class FileDataWrapper : public lgfx::v1::DataWrapper {
public:
  explicit FileDataWrapper(fs::File& file) : _file(&file) {}
  int read(uint8_t* buf, uint32_t len) override { return _file ? _file->read(buf, len) : -1; }
  void skip(int32_t offset) override {
    if (!_file) return;
    uint32_t pos = _file->position();
    _file->seek(pos + offset);
  }
  bool seek(uint32_t offset) override { return _file ? _file->seek(offset) : false; }
  void close() override { if (_file && *_file) _file->close(); }
  int32_t tell() override { return _file ? (int32_t)_file->position() : -1; }
private:
  fs::File* _file = nullptr;
};

// ==================== HELPERS ====================
static bool inRect(int x, int y, const BtnRect& r) {
  return (x >= r.x && x < (r.x + r.w) && y >= r.y && y < (r.y + r.h));
}

static void drawWrappedText(const char* text_cstr, int x, int y, int w, int lineH) {
  String text(text_cstr);
  String line;
  int cursorY = y;
  int i = 0;

  while (i < (int)text.length()) {
    if (text[i] == '\n') {
      M5.Display.drawString(line, x, cursorY);
      line = "";
      cursorY += lineH;
      i++;
      continue;
    }

    int start = i;
    while (i < (int)text.length() && text[i] != ' ' && text[i] != '\n') i++;
    String word = text.substring(start, i);

    bool hasSpace = (i < (int)text.length() && text[i] == ' ');
    if (hasSpace) i++;

    String candidate = line;
    if (candidate.length()) candidate += " ";
    candidate += word;

    if (M5.Display.textWidth(candidate) <= w) {
      line = candidate;
    } else {
      if (line.length() == 0) {
        M5.Display.drawString(candidate, x, cursorY);
        cursorY += lineH;
      } else {
        M5.Display.drawString(line, x, cursorY);
        line = word;
        cursorY += lineH;
      }
    }

    if (cursorY > (TEXT_Y + TEXT_H - lineH)) break;
  }

  if (line.length() && cursorY <= (TEXT_Y + TEXT_H - lineH)) {
    M5.Display.drawString(line, x, cursorY);
  }
}

// ==================== UI DRAW ====================
static void drawButton(const BtnRect& r, const String& label, uint16_t bg, uint16_t fg) {
  // petit “style” : rectangle arrondi + bordure
  int radius = 12;
  M5.Display.fillRoundRect(r.x, r.y, r.w, r.h, radius, bg);
  M5.Display.drawRoundRect(r.x, r.y, r.w, r.h, radius, TFT_BLACK);

  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(fg, bg);
  M5.Display.drawString(label, r.x + r.w / 2, r.y + r.h / 2);
}

static void drawBottomLeftControls() {
  // bouton prev/next toujours visibles
  uint16_t bgNav = TFT_DARKGREY;
  uint16_t fgNav = TFT_WHITE;

  // bouton mode : couleur selon le mode
  uint16_t bgMode = (runMode == MODE_AUTO) ? TFT_DARKGREEN : TFT_DARKCYAN;
  uint16_t fgMode = TFT_WHITE;

  drawButton(btnPrev, "<", bgNav, fgNav);
  drawButton(btnMode, (runMode == MODE_AUTO) ? "AUTO" : "MANU", bgMode, fgMode);
  drawButton(btnNext, ">", bgNav, fgNav);

  // aide courte (sous les boutons)
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_WHITE, TFT_TRANSPARENT);
  String hint = (runMode == MODE_AUTO)
    ? "Tap AUTO->MANU"
    : "Tap < / > pour changer";
  M5.Display.drawString(hint, btnPrev.x, btnPrev.y + btnPrev.h + 6);
}

static void drawWaitingHintInPanel() {
  // texte discret en bas du panneau texte (à droite)
  M5.Display.setClipRect(TEXT_X, TEXT_Y, TEXT_W, TEXT_H);
  int x = TEXT_X + PAD;
  int y = TEXT_Y + TEXT_H - 10;

  M5.Display.setTextDatum(bottom_left);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_LIGHTGREY, TFT_TRANSPARENT);

  if (runMode == MODE_AUTO) {
    String msg = has_auto_fix ? "Zone detectee (AUTO)." : "En attente d'une zone (AUTO)...";
    M5.Display.drawString(msg, x, y);
  } else {
    M5.Display.drawString("Mode manuel : zone forcee.", x, y);
  }

  M5.Display.clearClipRect();
}

// ==================== DISPLAY ZONE ====================
static void displayZone(int idx) {
  fs::File f = SD.open(IMAGE_PATHS[idx], FILE_READ);
  if (!f) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.setTextSize(3);
    M5.Display.drawString("PNG introuvable", 640, 360);
    return;
  }

  FileDataWrapper dw(f);
  bool ok = M5.Display.drawPng(&dw, 0, 0);
  dw.close();

  if (!ok) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.setTextSize(3);
    M5.Display.drawString("Decode PNG KO", 640, 360);
    return;
  }

  // Panneau texte à droite
  M5.Display.setClipRect(TEXT_X, TEXT_Y, TEXT_W, TEXT_H);
  M5.Display.setTextColor(TFT_WHITE, TFT_TRANSPARENT);
  M5.Display.setTextDatum(top_left);

  int x = TEXT_X + PAD;
  int y = TEXT_Y + PAD;
  int w = TEXT_W - 2 * PAD;

  // petite ligne "Zone + confiance"
  M5.Display.setTextSize(1);
  String topInfo;
  if (runMode == MODE_MANUAL) {
    topInfo = String("ZONE: ") + LOCATION_NAMES[idx] + "   |   MODE: MANUEL (FORCE)";
  } else {
    topInfo = String("ZONE: ") + LOCATION_NAMES[idx] + "   |   MODE: AUTO   |   CONF: " + String((int)(current_confidence * 100)) + "%";
  }
  M5.Display.drawString(topInfo, x, y);
  y += 22;

  M5.Display.setTextSize(3);
  M5.Display.drawString(String(TITRE[idx]), x, y);
  y += 44;

  M5.Display.setTextSize(2);
  M5.Display.drawString(String(AUTEUR[idx]), x, y); y += 28;
  M5.Display.drawString(String(ANNEE[idx]),  x, y); y += 40;

  int lineH = 26;
  M5.Display.setTextSize(1);
  drawWrappedText(DESCRIPTION[idx], x, y, w, lineH);

  M5.Display.clearClipRect();

  // Boutons tactiles overlay en bas à gauche
  drawBottomLeftControls();

  // Texte discret "attente..."
  drawWaitingHintInPanel();
}

// ==================== NN ====================
static inline float relu(float x) { return (x > 0) ? x : 0; }

static void softmax(const float* input, float* output, int size) {
  float max_val = input[0];
  for (int i = 1; i < size; i++) if (input[i] > max_val) max_val = input[i];
  float sum = 0.0f;
  for (int i = 0; i < size; i++) { output[i] = expf(input[i] - max_val); sum += output[i]; }
  for (int i = 0; i < size; i++) output[i] /= sum;
}

static void predict(const float* input, float* output) {
  float x_norm[INPUT_SIZE];
  for (int i = 0; i < INPUT_SIZE; i++) x_norm[i] = (input[i] - MEAN[i]) / STD[i];

  float hidden[HIDDEN_SIZE];
  for (int i = 0; i < HIDDEN_SIZE; i++) {
    float sum = b1[i];
    for (int j = 0; j < INPUT_SIZE; j++) sum += x_norm[j] * W1[j][i];
    hidden[i] = relu(sum);
  }

  float logits[OUTPUT_SIZE];
  for (int i = 0; i < OUTPUT_SIZE; i++) {
    float sum = b2[i];
    for (int j = 0; j < HIDDEN_SIZE; j++) sum += hidden[j] * W2[j][i];
    logits[i] = sum;
  }

  softmax(logits, output, OUTPUT_SIZE);
}

// ==================== ZONE CHANGE ====================
static void setZoneManual(int idx) {
  if (idx < 0) idx = OUTPUT_SIZE - 1;
  if (idx >= OUTPUT_SIZE) idx = 0;

  current_zone_idx = idx;
  current_confidence = 1.0f; // forcé
  displayZone(current_zone_idx);
}

// ==================== TOUCH HANDLER ====================
static void handleTouch() {
  auto t = M5.Touch.getDetail();
  if (!t.wasPressed()) return;

  unsigned long now = millis();
  if (now - last_touch_ms < TOUCH_DEBOUNCE_MS) return;
  last_touch_ms = now;

  int tx = t.x;
  int ty = t.y;

  // MODE toggle
  if (inRect(tx, ty, btnMode)) {
    runMode = (runMode == MODE_AUTO) ? MODE_MANUAL : MODE_AUTO;

    // en manuel, on force la zone affichée (et on redraw pour refléter le mode)
    if (runMode == MODE_MANUAL) {
      current_confidence = 1.0f;
    }
    displayZone(current_zone_idx);
    return;
  }

  // prev / next (actifs seulement en MANUEL)
  if (runMode == MODE_MANUAL) {
    if (inRect(tx, ty, btnPrev)) { setZoneManual(current_zone_idx - 1); return; }
    if (inRect(tx, ty, btnNext)) { setZoneManual(current_zone_idx + 1); return; }
  }
}

// ==================== AUTO ====================
static void scanAndPredictAuto() {
  int n = WiFi.scanNetworks();
  if (n <= 0) return;

  float rssi_values[INPUT_SIZE];
  for (int i = 0; i < INPUT_SIZE; i++) rssi_values[i] = -100.0f;

  for (int i = 0; i < n; i++) {
    String mac = WiFi.BSSIDstr(i);
    int rssi = WiFi.RSSI(i);
    for (int j = 0; j < INPUT_SIZE; j++) {
      if (mac == String(AP_MACS[j])) { rssi_values[j] = (float)rssi; break; }
    }
  }

  float probs[OUTPUT_SIZE];
  predict(rssi_values, probs);

  int best = 0;
  for (int i = 1; i < OUTPUT_SIZE; i++) if (probs[i] > probs[best]) best = i;
  float confidence = probs[best];

  Serial.print("Probs: ");
  for (int i = 0; i < OUTPUT_SIZE; i++) Serial.printf("%s=%.1f%% ", LOCATION_NAMES[i], probs[i] * 100.0f);
  Serial.println();

  if (confidence >= CONFIDENCE_THRESHOLD) {
    has_auto_fix = true;
    current_confidence = confidence;

    if (best != current_zone_idx) {
      current_zone_idx = best;
      displayZone(current_zone_idx);
    } else {
      // même zone, si on veut refléter "has_auto_fix" dès le 1er fix :
      // on redraw une seule fois lorsque ça vient de passer à true
      // (ici has_auto_fix est déjà true, donc pas nécessaire)
    }
  } else {
    // pas de fix -> on ne change pas la zone affichée
    // (le texte "attente..." reste visible au prochain redraw)
  }
}

// ==================== LOOP ====================
unsigned long last_scan_time = 0;

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  delay(300);

  M5.Display.setRotation(3);
  M5.Display.fillScreen(TFT_BLACK);

  // WiFi pins comme ton collecteur
  WiFi.setPins(12, 13, 11, 10, 9, 8, 15);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // SD init stable
  if (!SD.begin()) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(3);
    M5.Display.drawString("ERREUR SD", 640, 360);
    while (1) delay(100);
  }
  sd_ok = true;

  // ✅ Au démarrage : afficher Zone 1 directement
  current_zone_idx = 0;
  current_confidence = 0.0f;
  has_auto_fix = false;
  runMode = MODE_AUTO;

  displayZone(current_zone_idx);

  Serial.println("Ready.");
}

void loop() {
  M5.update();

  // Tactile (boutons en bas à gauche)
  handleTouch();

  // Auto scan
  if (runMode == MODE_AUTO) {
    unsigned long now = millis();
    if (now - last_scan_time >= SCAN_INTERVAL_MS) {
      last_scan_time = now;
      scanAndPredictAuto();

      // Si tu veux que le texte "en attente..." se mette à jour même sans changement de zone,
      // tu peux redessiner seulement le panneau / hint à intervalle régulier.
      // Ici on évite pour limiter le flicker.
    }
  }

  delay(20);
}
