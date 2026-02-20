#include <WiFi.h>
#include <PubSubClient.h>
#include <string.h>

// =========================
// CONFIG WIFI (Wokwi)
// =========================
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// =========================
// UBIDOTS MQTT (Industrial)
// =========================
const char* MQTT_HOST = "industrial.api.ubidots.com";
const int   MQTT_PORT = 1883;

// ⚠️ RECOMENDADO: revogar este token e gerar outro no Ubidots.
// Depois, não versionar no GitHub (use secrets.h).
const char* UBIDOTS_TOKEN = "BBUS-Z5TR8RRzCQ1ORUBmMLYS52BSdTFIYP";

// No Ubidots: USER = TOKEN
const char* MQTT_USER = UBIDOTS_TOKEN;

// Em muitos casos pode ser vazio; se falhar, coloque UBIDOTS_TOKEN aqui também.
const char* MQTT_PASS = "";

// Devices no Ubidots (um por motor)
const char* DEVICE_LABEL_ENGINE_A = "ship01-engine-a";
const char* DEVICE_LABEL_ENGINE_B = "ship01-engine-b";

// Tópicos Ubidots
String topicA = String("/v1.6/devices/") + DEVICE_LABEL_ENGINE_A;
String topicB = String("/v1.6/devices/") + DEVICE_LABEL_ENGINE_B;

// =========================
// TIMING
// =========================
const unsigned long PUBLISH_INTERVAL_MS = 5000; // 5s (estável)
// Para o vídeo, pode reduzir p/ 2000
unsigned long lastPublish = 0;

// =========================
// CENÁRIOS (Normal -> Stress -> Falha)
// =========================
enum Scenario { NORMAL = 0, STRESS = 1, FAILURE = 2 };
Scenario currentScenario = NORMAL;
unsigned long scenarioStart = 0;
const unsigned long SCENARIO_DURATION_MS = 60000; // 60s
// Para o vídeo, pode reduzir p/ 20000

// =========================
// CLIENTES
// =========================
WiFiClient espClient;
PubSubClient mqtt(espClient);

// =========================
// MODELO DE MOTOR
// =========================
struct EngineState {
  const char* engineId; // "A" ou "B"
  float tempC;
  float vibRms;
  int rpm;
  float oilBar;
  int healthScore;
  int anomalyFlag;
};

EngineState engineA = {"A", 78.0, 2.2, 1650, 3.4, 100, 0};
EngineState engineB = {"B", 80.0, 2.5, 1670, 3.3, 100, 0};

// =========================
// PROTOTYPES
// =========================
float frand(float minV, float maxV);
int calcHealthScore(float tempC, float vibRms, int rpm, float oilBar);
int calcAnomalyFlag(float tempC, float vibRms, int rpm, float oilBar);
void stepEngine(EngineState &e);
String buildUbidotsPayload(const EngineState &e);
void ensureMqttConnected();
void connectWifi();
void updateScenario();

// =========================
// UTIL
// =========================
float frand(float minV, float maxV) {
  return minV + (float)random(0, 10000) / 10000.0f * (maxV - minV);
}

// =========================
// REGRAS (edge intelligence)
// =========================
int calcHealthScore(float tempC, float vibRms, int rpm, float oilBar) {
  int score = 100;

  if (tempC > 85) score -= (int)((tempC - 85) * 2);
  if (tempC > 95) score -= (int)((tempC - 95) * 4);

  if (vibRms > 3.0) score -= (int)((vibRms - 3.0) * 15);
  if (vibRms > 6.0) score -= 20;

  if (rpm < 1400) score -= (1400 - rpm) / 10;
  if (rpm > 1900) score -= (rpm - 1900) / 10;

  if (oilBar < 2.8) score -= (int)((2.8 - oilBar) * 30);
  if (oilBar < 2.2) score -= 25;

  if (score < 0) score = 0;
  if (score > 100) score = 100;
  return score;
}

int calcAnomalyFlag(float tempC, float vibRms, int rpm, float oilBar) {
  bool tempCrit = tempC > 98;
  bool vibCrit  = vibRms > 6.5;
  bool oilCrit  = oilBar < 2.2;
  bool rpmCrit  = (rpm < 1350) || (rpm > 2000);
  return (tempCrit || vibCrit || oilCrit || rpmCrit) ? 1 : 0;
}

// =========================
// SIMULAÇÃO
// =========================
void stepEngine(EngineState &e) {
  float tempBase = (strcmp(e.engineId, "A") == 0) ? 78.0 : 80.0;
  float vibBase  = (strcmp(e.engineId, "A") == 0) ? 2.2 : 2.5;
  int rpmBase    = (strcmp(e.engineId, "A") == 0) ? 1650 : 1670;
  float oilBase  = (strcmp(e.engineId, "A") == 0) ? 3.4 : 3.3;

  float tempDrift = 0.0;
  float vibDrift  = 0.0;
  int rpmDrift    = 0;
  float oilDrift  = 0.0;

  if (currentScenario == NORMAL) {
    tempDrift = frand(-0.4, 0.4);
    vibDrift  = frand(-0.1, 0.1);
    rpmDrift  = (int)frand(-20, 20);
    oilDrift  = frand(-0.05, 0.05);
  } else if (currentScenario == STRESS) {
    tempDrift = frand(0.1, 0.8);
    vibDrift  = frand(0.0, 0.25);
    rpmDrift  = (int)frand(-30, 30);
    oilDrift  = frand(-0.08, 0.03);
  } else { // FAILURE
    tempDrift = frand(0.6, 1.5);
    vibDrift  = frand(0.2, 0.8);
    rpmDrift  = (int)frand(-80, 80);
    oilDrift  = frand(-0.20, -0.05);

    if (strcmp(e.engineId, "B") == 0) {
      tempDrift += frand(0.2, 0.8);
      vibDrift  += frand(0.2, 0.6);
      oilDrift  += frand(-0.10, -0.02);
    }
  }

  e.tempC   = 0.85f * e.tempC   + 0.15f * (tempBase + tempDrift);
  e.vibRms  = 0.80f * e.vibRms  + 0.20f * (vibBase  + vibDrift);
  e.rpm     = (int)(0.70f * e.rpm + 0.30f * (rpmBase + rpmDrift));
  e.oilBar  = 0.80f * e.oilBar  + 0.20f * (oilBase  + oilDrift);

  if (e.tempC < 50) e.tempC = 50;
  if (e.tempC > 120) e.tempC = 120;

  if (e.vibRms < 0.5) e.vibRms = 0.5;
  if (e.vibRms > 12.0) e.vibRms = 12.0;

  if (e.rpm < 1000) e.rpm = 1000;
  if (e.rpm > 2300) e.rpm = 2300;

  if (e.oilBar < 1.2) e.oilBar = 1.2;
  if (e.oilBar > 4.5) e.oilBar = 4.5;

  e.healthScore = calcHealthScore(e.tempC, e.vibRms, e.rpm, e.oilBar);
  e.anomalyFlag = calcAnomalyFlag(e.tempC, e.vibRms, e.rpm, e.oilBar);
}

// =========================
// UBIDOTS PAYLOAD
// =========================
String buildUbidotsPayload(const EngineState &e) {
  String sc = (currentScenario==NORMAL) ? "NORMAL" : (currentScenario==STRESS) ? "STRESS" : "FAILURE";

  String json = "{";
  json += "\"temp_c\":" + String(e.tempC, 2) + ",";
  json += "\"vib_rms\":" + String(e.vibRms, 2) + ",";
  json += "\"rpm\":" + String(e.rpm) + ",";
  json += "\"oil_bar\":" + String(e.oilBar, 2) + ",";
  json += "\"health_score\":" + String(e.healthScore) + ",";
  json += "\"anomaly_flag\":" + String(e.anomalyFlag) + ",";
  json += "\"engine_id\":\"" + String(e.engineId) + "\",";
  json += "\"scenario\":\"" + sc + "\"";
  json += "}";
  return json;
}

// =========================
// WIFI
// =========================
void connectWifi() {
  Serial.print("Conectando WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.println("\nWiFi OK. IP: " + WiFi.localIP().toString());
}

// =========================
// MQTT
// =========================
void ensureMqttConnected() {
  while (!mqtt.connected()) {
    String clientId = "wokwi-open-sea-" + String((uint32_t)random(0xFFFF), HEX);
    Serial.print("Conectando MQTT (Ubidots)... ");

    bool ok = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS);

    if (ok) {
      Serial.println("OK");
    } else {
      Serial.print("falhou (rc=");
      Serial.print(mqtt.state());
      Serial.println("). Tentando em 2s...");
      delay(2000);
    }
  }
}

// =========================
// CENÁRIO CONTROL
// =========================
void updateScenario() {
  if (scenarioStart == 0) scenarioStart = millis();
  unsigned long elapsed = millis() - scenarioStart;

  if (elapsed > SCENARIO_DURATION_MS) {
    scenarioStart = millis();
    currentScenario = (Scenario)(((int)currentScenario + 1) % 3);
    Serial.print(">>> Cenário mudou para: ");
    Serial.println((currentScenario==NORMAL)?"NORMAL":(currentScenario==STRESS)?"STRESS":"FAILURE");
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  randomSeed(esp_random());

  connectWifi();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(512); // evita problemas com payload JSON

  ensureMqttConnected();

  Serial.println("Open Sea Engine Monitor (Simulado) - iniciado.");
  Serial.println("Publicando a cada 5s em (Ubidots):");
  Serial.println(String(" - ") + topicA);
  Serial.println(String(" - ") + topicB);

  scenarioStart = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWifi();
  if (!mqtt.connected()) ensureMqttConnected();

  mqtt.loop();
  updateScenario();

  unsigned long now = millis();
  if (now - lastPublish >= PUBLISH_INTERVAL_MS) {
    lastPublish = now;

    stepEngine(engineA);
    stepEngine(engineB);

    String payloadA = buildUbidotsPayload(engineA);
    String payloadB = buildUbidotsPayload(engineB);

    mqtt.publish(topicA.c_str(), payloadA.c_str());
    mqtt.publish(topicB.c_str(), payloadB.c_str());

    Serial.println(payloadA);
    Serial.println(payloadB);
  }
}