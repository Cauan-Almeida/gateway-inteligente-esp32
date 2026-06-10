/*
 * LoRa Interno — Gateway Inteligente
 *
 * LÓGICA COMPLETA:
 * - Recebe pacotes do LoRa externo (temp+umid) e envia para API externa
 * - Lê DHT interno e envia para API interna
 * - COM internet: envia em tempo real (a cada 5s)
 * - SEM internet (Wi-Fi caído OU Wi-Fi ok mas HTTP falhou):
 * → salva 1 registro a cada 10 min na SPIFFS (ext e int separados)
 * → loop verifica WiFi.status() a cada ciclo
 * → quando internet volta: descarrega fila (1 registro por ciclo do loop,
 * sem delay, para nunca perder pacote LoRa)
 * - NTP não sincronizado: idade enviada como 0, nunca valor absurdo no DB
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#include <RoboCore_SMW_SX1276M0.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <DHT.h>
#include "secrets.h"

// ─── Pinos ───────────────────────────────────────────────
#define RXD2            16
#define TXD2            17
#define PINO_RESET       5
#define PINO_BOTAO       4
#define PINO_LED_STATUS  2
#define PINO_DHT        12

#define FUSO_BR               (-3 * 3600)
#define INTERVALO_TENTAR_WIFI  30000UL
#define OFFSET_TEMP            -2.5f
#define INTERVALO_ENVIO        5000UL   // coleta e envio online: 5s
#define INTERVALO_SALVAR       600000UL // throttle offline: 10 min

// ─── Struct pacote (Externo → Interno) ───────────────────
struct __attribute__((__packed__)) PacoteDados {
  float    temperatura;
  float    umidade;
  uint32_t idade_segundos;
};

DHT dht(PINO_DHT, DHT11);

// ─── LoRa ────────────────────────────────────────────────
HardwareSerial LoRaSerial(2);
SMW_SX1276M0 lorawan(LoRaSerial);
CommandResponse response;

const char DEVADDR[]     = "00000000";
const char DEVADDR_P2P[] = "00000000";
const char APPSKEY[]     = "00000000000000000000000000000000";
const char NWKSKEY[]     = "00000000000000000000000000000000";

// ─── Estado ──────────────────────────────────────────────
bool wifiOnline        = false;
bool ntpSincronizado   = false;
unsigned long tUltimaTentaWifi = 0;

// ─── Filas offline ───────────────────────────────────────
#define ARQUIVO_FILA_EXTERNA  "/fila_int.json"
#define ARQUIVO_FILA_INTERNA  "/fila_interna.json"
#define MAX_REGISTROS         500

unsigned long tUltimoSalvarExterno  = 0;
unsigned long tUltimoSalvarInterno  = 0;
bool redeEstavaOfflineExterno = false;
bool redeEstavaOfflineInterno = false;

// ─── Coleta interna ──────────────────────────────────────
// tUltimaLeituraInterna: ritmo de leitura do DHT (5s sempre)
// tUltimoSalvarInterno:  throttle de gravação offline (10 min) — independente
unsigned long tUltimaLeituraInterna = 0;

// ─── LED ─────────────────────────────────────────────────
void ledSucesso() { digitalWrite(PINO_LED_STATUS, HIGH); delay(80); digitalWrite(PINO_LED_STATUS, LOW); }
void ledErro()    { digitalWrite(PINO_LED_STATUS, HIGH); }
void ledNormal()  { digitalWrite(PINO_LED_STATUS, LOW); }

// ═════════════════════════════════════════════════════════
// Timestamp seguro — retorna 0 se NTP não sincronizou
// Evita valores absurdos (ex: 2514331190) no banco de dados
// ═════════════════════════════════════════════════════════
uint32_t getTimestampBR() {
  if (!ntpSincronizado) return 0;
  time_t t = time(NULL);
  if (t < 1000000000L) return 0; // sanidade: rejeita lixo
  return (uint32_t)(t + FUSO_BR);
}

bool hexParaBytes(const String &hex, uint8_t* out, size_t n) {
  if ((int)hex.length() < (int)(n * 2)) return false;
  for (size_t i = 0; i < n; i++) {
    char a = hex.charAt(i*2), b = hex.charAt(i*2+1);
    uint8_t v = 0;
    v += (a>='a') ? a-'a'+10 : (a>='A') ? a-'A'+10 : a-'0'; v*=16;
    v += (b>='a') ? b-'a'+10 : (b>='A') ? b-'A'+10 : b-'0';
    out[i] = v;
  }
  return true;
}

void sincronizarNTP() {
  if (WiFi.status() != WL_CONNECTED) return;
  configTime(0, 0, "pool.ntp.org", "time.google.com", "a.ntp.br");
  time_t agora = 0;
  unsigned long inicio = millis();
  while (agora < 1000000000UL && millis() - inicio < 10000) {
    delay(200); agora = time(NULL);
  }
  if (agora > 1000000000UL) {
    ntpSincronizado = true;
    Serial.println("[NTP] Sincronizado.");
  }
}

bool conectarWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.println("[WiFi] Tentando conectar na rede 1...");
  WiFi.disconnect(true); delay(200);
  WiFi.begin(SECRET_SSID_1, SECRET_PASS_1);
  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("[WiFi] Conectado: "); Serial.println(SECRET_SSID_1);
      Serial.print("[WiFi] IP: "); Serial.println(WiFi.localIP());
      wifiOnline = true; ledNormal();
      if (!ntpSincronizado) sincronizarNTP();
      return true;
    }
    delay(500);
  }
  Serial.print("[WiFi] Falha na rede 1. Status="); Serial.println((int)WiFi.status());

#if defined(SECRET_SSID_2) && defined(SECRET_PASS_2)
  Serial.println("[WiFi] Tentando conectar na rede 2...");
  WiFi.disconnect(true); delay(200);
  WiFi.begin(SECRET_SSID_2, SECRET_PASS_2);
  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("[WiFi] Conectado: "); Serial.println(SECRET_SSID_2);
      Serial.print("[WiFi] IP: "); Serial.println(WiFi.localIP());
      wifiOnline = true; ledNormal();
      if (!ntpSincronizado) sincronizarNTP();
      return true;
    }
    delay(500);
  }
  Serial.print("[WiFi] Falha na rede 2. Status="); Serial.println((int)WiFi.status());
#endif

  wifiOnline = false; ledErro();
  Serial.println("[WiFi] Sem conexao.");
  return false;
}

// ═════════════════════════════════════════════════════════
// Envia para API — retorna true só se HTTP 2xx
// ═════════════════════════════════════════════════════════
bool enviarParaAPI(const char* url, const char* apiKey, const char* deviceId,
                   float temp, float umid, uint32_t idade, bool incluirUmidade) {
  if (WiFi.status() != WL_CONNECTED) { ledErro(); return false; }
  if (!url || url[0]=='\0' || !apiKey || apiKey[0]=='\0' || !deviceId || deviceId[0]=='\0') {
    Serial.println("[API] Destino nao configurado."); return false;
  }
  String payload = String("{\"d\":\"") + deviceId + "\""
                 + ",\"t\":" + String(temp, 1);
  if (incluirUmidade) payload += ",\"h\":" + String(umid, 1);
  payload += ",\"i\":" + String(idade) + "}";

  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", apiKey);
  http.setTimeout(5000);
  int code = http.POST(payload);
  Serial.print("[API] "); Serial.print(payload);
  Serial.print(" -> HTTP "); Serial.println(code);
  http.end();
  if (code >= 200 && code < 300) { ledSucesso(); return true; }
  ledErro(); return false;
}

// ═════════════════════════════════════════════════════════
// SPIFFS — salvar
// ═════════════════════════════════════════════════════════
void salvarNaFilaExterna(float temp, float umid, uint32_t tc) {
  File f = SPIFFS.open(ARQUIVO_FILA_EXTERNA, "r");
  JsonDocument doc;
  if (f) { deserializeJson(doc, f); f.close(); }
  JsonArray arr = doc["q"].is<JsonArray>() ? doc["q"].as<JsonArray>() : doc["q"].to<JsonArray>();
  if ((int)arr.size() >= MAX_REGISTROS) arr.remove(0);
  JsonObject reg = arr.add<JsonObject>();
  reg["t"] = temp; reg["h"] = umid; reg["tc"] = tc;
  File fw = SPIFFS.open(ARQUIVO_FILA_EXTERNA, "w");
  if (fw) { serializeJson(doc, fw); fw.close(); }
  Serial.print("[FILA EXT] Guardado offline. Total: "); Serial.println(arr.size());
}

void salvarNaFilaInterna(float temp, float umid, uint32_t tc) {
  File f = SPIFFS.open(ARQUIVO_FILA_INTERNA, "r");
  JsonDocument doc;
  if (f) { deserializeJson(doc, f); f.close(); }
  JsonArray arr = doc["q"].is<JsonArray>() ? doc["q"].as<JsonArray>() : doc["q"].to<JsonArray>();
  if ((int)arr.size() >= MAX_REGISTROS) arr.remove(0);
  JsonObject reg = arr.add<JsonObject>();
  reg["t"] = temp; reg["h"] = umid; reg["tc"] = tc;
  File fw = SPIFFS.open(ARQUIVO_FILA_INTERNA, "w");
  if (fw) { serializeJson(doc, fw); fw.close(); }
  Serial.print("[FILA INT] Guardado offline. Total: "); Serial.println(arr.size());
}

// ═════════════════════════════════════════════════════════
// SPIFFS — descarregar (1 registro por chamada, sem delay)
// ═════════════════════════════════════════════════════════
bool descarregarFilaExterna() {
  if (!SPIFFS.exists(ARQUIVO_FILA_EXTERNA) || WiFi.status() != WL_CONNECTED) return false;
  File f = SPIFFS.open(ARQUIVO_FILA_EXTERNA, "r"); if (!f) return false;
  JsonDocument doc;
  if (deserializeJson(doc, f) || !doc["q"].is<JsonArray>()) { f.close(); return false; }
  f.close();
  JsonArray arr = doc["q"].as<JsonArray>();
  
  if (arr.size() == 0) { 
    SPIFFS.remove(ARQUIVO_FILA_EXTERNA); 
    redeEstavaOfflineExterno = false;
    return false; 
  }

  JsonObject reg = arr[0];
  float t = reg["t"]; float h = reg["h"]; uint32_t tc = reg["tc"];
  uint32_t agora_br = getTimestampBR();
  // Se tc=0 (NTP ausente na coleta) ou agora=0 (ainda sem NTP): envia idade=0
  uint32_t idade = (agora_br > 0 && tc > 0 && agora_br >= tc) ? (agora_br - tc) : 0;

  Serial.print("[SYNC EXT] Enviando 1 registro. Restam: "); Serial.println((int)arr.size());

  if (enviarParaAPI(SECRET_API_URL_EXTERNA, SECRET_API_KEY_EXTERNA,
                    SECRET_DEVICE_ID_EXTERNO, t, h, idade, true)) {
    JsonDocument novo; JsonArray na = novo["q"].to<JsonArray>();
    for (int i = 1; i < (int)arr.size(); i++) na.add(arr[i]);
    if (na.size() == 0) {
      SPIFFS.remove(ARQUIVO_FILA_EXTERNA);
      Serial.println("[SYNC EXT] Fila externa zerada!");
      redeEstavaOfflineExterno = false; 
    } else {
      File fw = SPIFFS.open(ARQUIVO_FILA_EXTERNA, "w");
      if (fw) { serializeJson(novo, fw); fw.close(); }
    }
    return true; // SUCESSO
  }
  return false; // FALHA
}

bool descarregarFilaInterna() {
  if (!SPIFFS.exists(ARQUIVO_FILA_INTERNA) || WiFi.status() != WL_CONNECTED) return false;
  File f = SPIFFS.open(ARQUIVO_FILA_INTERNA, "r"); if (!f) return false;
  JsonDocument doc;
  if (deserializeJson(doc, f) || !doc["q"].is<JsonArray>()) { f.close(); return false; }
  f.close();
  JsonArray arr = doc["q"].as<JsonArray>();
  
  if (arr.size() == 0) { 
    SPIFFS.remove(ARQUIVO_FILA_INTERNA); 
    redeEstavaOfflineInterno = false;
    return false; 
  }

  JsonObject reg = arr[0];
  float t = reg["t"]; float h = reg["h"]; uint32_t tc = reg["tc"];
  uint32_t agora_br = getTimestampBR();
  uint32_t idade = (agora_br > 0 && tc > 0 && agora_br >= tc) ? (agora_br - tc) : 0;

  Serial.print("[SYNC INT] Enviando 1 registro. Restam: "); Serial.println((int)arr.size());

  if (enviarParaAPI(SECRET_API_URL_INTERNA, SECRET_API_KEY_INTERNA,
                    SECRET_DEVICE_ID_INTERNO, t, h, idade, true)) {
    JsonDocument novo; JsonArray na = novo["q"].to<JsonArray>();
    for (int i = 1; i < (int)arr.size(); i++) na.add(arr[i]);
    if (na.size() == 0) {
      SPIFFS.remove(ARQUIVO_FILA_INTERNA);
      Serial.println("[SYNC INT] Fila interna zerada!");
      redeEstavaOfflineInterno = false; 
    } else {
      File fw = SPIFFS.open(ARQUIVO_FILA_INTERNA, "w");
      if (fw) { serializeJson(novo, fw); fw.close(); }
    }
    return true; // SUCESSO
  }
  return false; // FALHA
}

// ═════════════════════════════════════════════════════════
// Sensor interno (DHT)
// ═════════════════════════════════════════════════════════
void processarInterno() {
  unsigned long agora_ms = millis();
  if (agora_ms - tUltimaLeituraInterna < INTERVALO_ENVIO) return;

  float temp = dht.readTemperature() + OFFSET_TEMP;
  float umid = dht.readHumidity();

  if (isnan(temp) || isnan(umid)) {
    Serial.println("[INT] Falha na leitura DHT. Aguardando proximo ciclo.");
    return; // Não atualiza timer — tenta de novo no próximo ciclo
  }

  tUltimaLeituraInterna = agora_ms;
  Serial.print("[INT] Temp="); Serial.print(temp, 1);
  Serial.print(" Umid="); Serial.println(umid, 1);

  bool enviou = false;

  if (WiFi.status() == WL_CONNECTED) {
    enviou = enviarParaAPI(SECRET_API_URL_INTERNA, SECRET_API_KEY_INTERNA,
                           SECRET_DEVICE_ID_INTERNO, temp, umid, 0, true);
    // NÃO zera redeEstavaOfflineInterno aqui — só zera quando a fila esvazia
  }

  if (!enviou) {
    uint32_t ts_coleta = getTimestampBR(); // 0 se NTP não sincronizou — seguro
    if (!redeEstavaOfflineInterno || (agora_ms - tUltimoSalvarInterno >= INTERVALO_SALVAR)) {
      Serial.println("[INT] Offline. Salvando na SPIFFS...");
      salvarNaFilaInterna(temp, umid, ts_coleta);
      tUltimoSalvarInterno = agora_ms;
      redeEstavaOfflineInterno = true;
    } else {
      unsigned long r = (INTERVALO_SALVAR - (agora_ms - tUltimoSalvarInterno)) / 1000;
      Serial.println("[INT] Offline. Leitura descartada. Proxima gravacao em " + String(r) + "s.");
    }
  }
}

// ═════════════════════════════════════════════════════════
// Pacote LoRa recebido do externo
// ═════════════════════════════════════════════════════════
void processarLoRa(const String &hex) {
  PacoteDados p;
  if (!hexParaBytes(hex, (uint8_t*)&p, sizeof(p))) return;

  Serial.print("\n[LoRa RX] Temp="); Serial.print(p.temperatura, 1);
  Serial.print(" Umid="); Serial.println(p.umidade, 1);

  unsigned long agora_ms = millis();
  bool enviou = false;

  if (WiFi.status() == WL_CONNECTED) {
    enviou = enviarParaAPI(SECRET_API_URL_EXTERNA, SECRET_API_KEY_EXTERNA,
                           SECRET_DEVICE_ID_EXTERNO, p.temperatura, p.umidade, 0, true);
    // NÃO zera redeEstavaOfflineExterno aqui — só zera quando a fila esvazia
  }

  if (!enviou) {
    uint32_t ts_coleta = getTimestampBR();
    if (!redeEstavaOfflineExterno || (agora_ms - tUltimoSalvarExterno >= INTERVALO_SALVAR)) {
      Serial.println("[Rede] Offline. Salvando na SPIFFS...");
      salvarNaFilaExterna(p.temperatura, p.umidade, ts_coleta);
      tUltimoSalvarExterno = agora_ms;
      redeEstavaOfflineExterno = true;
    } else {
      unsigned long r = (INTERVALO_SALVAR - (agora_ms - tUltimoSalvarExterno)) / 1000;
      Serial.println("[Rede] Offline. Pacote descartado. Proxima gravacao em " + String(r) + "s.");
    }
  }
}

// ═════════════════════════════════════════════════════════
// Event handler LoRa
// ═════════════════════════════════════════════════════════
void event_handler(Event type) {
  if (type != Event::RECEIVED_X) return;
  delay(50);
  lorawan.flush();
  uint8_t port;
  Buffer buffer;
  if (lorawan.readX(port, buffer) != CommandResponse::OK) return;
  String hex = "";
  while (buffer.available()) hex += (char)buffer.read();
  processarLoRa(hex);
}

// ═════════════════════════════════════════════════════════
// Setup
// ═════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  LoRaSerial.begin(115200, SERIAL_8N1, RXD2, TXD2);

  pinMode(PINO_BOTAO, INPUT_PULLUP);
  pinMode(PINO_LED_STATUS, OUTPUT);
  ledNormal();
  delay(2000);

  dht.begin();
  delay(500);

  SPIFFS.begin(true);
  WiFi.mode(WIFI_STA);
  conectarWifi();

  lorawan.event_listener = &event_handler;
  lorawan.setPinReset(PINO_RESET);
  lorawan.reset();

  Serial.println("=== LORA INTERNO (GATEWAY INTELIGENTE) ===");
  lorawan.set_DevAddr(DEVADDR);
  lorawan.set_P2P_DevAddr(DEVADDR_P2P);
  lorawan.set_AppSKey(APPSKEY);
  lorawan.set_NwkSKey(NWKSKEY);
  lorawan.set_P2P_SyncWord(18);

  response = lorawan.set_JoinMode(SMW_SX1276M0_JOIN_MODE_P2P);
  Serial.println(response == CommandResponse::OK ? "[LoRa] P2P OK" : "[LoRa] ERRO P2P");
  Serial.println("[Sistema] Escutando LoRa continuamente...");
}

// Intervalo mínimo entre tentativas de sync quando HTTP falha (30s)
// Quando o HTTP funciona, tUltimaTentaSync é zerado para tentar o próximo imediatamente
#define INTERVALO_TENTAR_SYNC 30000UL
unsigned long tUltimaTentaSync = 0;

// ═════════════════════════════════════════════════════════
// Loop
// ═════════════════════════════════════════════════════════
void loop() {
  lorawan.listen(); // SEMPRE primeiro

  unsigned long agora = millis();

  // Detecta queda de associação Wi-Fi
  if (wifiOnline && WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Conexao perdida!");
    wifiOnline = false;
    ledErro();
  }

  // Tenta reconectar a cada 30s
  if (!wifiOnline && (agora - tUltimaTentaWifi >= INTERVALO_TENTAR_WIFI)) {
    tUltimaTentaWifi = agora;
    if (conectarWifi()) {
      Serial.println("[WiFi] Reconectado!");
    }
  }

  // Descarrega fila:
  if (WiFi.status() == WL_CONNECTED &&
      (redeEstavaOfflineExterno || redeEstavaOfflineInterno) &&
      (agora - tUltimaTentaSync >= INTERVALO_TENTAR_SYNC)) {

    tUltimaTentaSync = agora; // assume falha; se ok, será zerado abaixo

    bool sucessoExt = false;
    bool sucessoInt = false;

    if (redeEstavaOfflineExterno) {
      sucessoExt = descarregarFilaExterna();
    }

    if (redeEstavaOfflineInterno) {
      sucessoInt = descarregarFilaInterna();
    }

    // Se QUALQUER UM dos envios funcionou, zera o timer.
    // Isso garante que no próximo ciclo do loop ele tentará descarregar o próximo
    // registro imediatamente, esvaziando a fila de forma rápida e contínua.
    if (sucessoExt || sucessoInt) {
      tUltimaTentaSync = 0;
    }
  }

  processarInterno();
}