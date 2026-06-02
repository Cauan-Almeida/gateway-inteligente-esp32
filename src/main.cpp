/*
 * LoRa Interno — Gateway Inteligente
 * - Recebe struct 12 bytes do Externo
 * - Wi-Fi OK → Sobe para API em tempo real
 * - Wi-Fi OFF → Salva 1 registro a cada 10 minutos na SPIFFS (Throttling)
 * - Não envia mais ACK de volta
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#include <RoboCore_SMW_SX1276M0.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include "secrets.h"

// ─── Pinos ───────────────────────────────────────────────
#define RXD2       16
#define TXD2       17
#define PINO_RESET 5
#define PINO_BOTAO 4

#define FUSO_BR (-3 * 3600)
#define INTERVALO_TENTAR_WIFI  30000UL
#define DEBOUNCE_BOTAO         2000UL

// ─── Struct pacote dados (Externo → Interno) ─────────────
struct __attribute__((__packed__)) PacoteDados {
  float    temperatura;     // 4 bytes
  float    umidade;         // 4 bytes
  uint32_t idade_segundos;  // 4 bytes
};

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
unsigned long tUltimoBotao     = 0;

// ─── Controle Offline (Throttling) ───────────────────────
#define ARQUIVO_FILA  "/fila_int.json"
#define MAX_REGISTROS 500
#define INTERVALO_SALVAR 600000UL // 10 minutos em ms
unsigned long tUltimoSalvar = 0;
bool redeEstavaOffline = false;

// ═════════════════════════════════════════════════════════
// Funções Auxiliares (Tempo, Hex, NTP, WiFi)
// ═════════════════════════════════════════════════════════
uint32_t getTimestampBR() { return (uint32_t)(time(NULL) + FUSO_BR); }

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
  if (agora > 1000000000UL) ntpSincronizado = true;
}

bool conectarWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.println("[WiFi] Tentando conectar na rede 1...");
  WiFi.disconnect(true);
  delay(200);
  WiFi.begin(SECRET_SSID_1, SECRET_PASS_1);
  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("[WiFi] Conectado: "); Serial.println(SECRET_SSID_1);
      Serial.print("[WiFi] IP: "); Serial.println(WiFi.localIP());
      wifiOnline = true;
      if (!ntpSincronizado) sincronizarNTP();
      return true;
    }
    delay(500);
  }

  Serial.print("[WiFi] Falha na rede 1. Status=");
  Serial.println((int)WiFi.status());

  Serial.println("[WiFi] Tentando conectar na rede 2...");
  WiFi.disconnect(true);
  delay(200);

#if defined(SECRET_SSID_2) && defined(SECRET_PASS_2)
  WiFi.begin(SECRET_SSID_2, SECRET_PASS_2);
  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("[WiFi] Conectado: "); Serial.println(SECRET_SSID_2);
      Serial.print("[WiFi] IP: "); Serial.println(WiFi.localIP());
      wifiOnline = true;
      if (!ntpSincronizado) sincronizarNTP();
      return true;
    }
    delay(500);
  }

  Serial.print("[WiFi] Falha na rede 2. Status=");
  Serial.println((int)WiFi.status());
#endif

  wifiOnline = false;
  Serial.println("[WiFi] Sem conexão.");
  return false;
}

// ═════════════════════════════════════════════════════════
// Envia para API Oracle
// ═════════════════════════════════════════════════════════
bool enviarParaAPI(float temp, float umid, uint32_t idade) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String payload = String("{\"d\":\"") + SECRET_DEVICE_ID + "\""
                 + String(",\"t\":") + String(temp, 1)
                 + String(",\"h\":") + String(umid, 1)
                 + String(",\"i\":") + String(idade) + "}";

  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, SECRET_API_URL)) return false;
  
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", SECRET_API_KEY);
  http.setTimeout(5000);

  int code = http.POST(payload);
  Serial.print("[API] "); Serial.print(payload);
  Serial.print(" → HTTP "); Serial.println(code);
  http.end();

  return (code >= 200 && code < 300);
}

// ═════════════════════════════════════════════════════════
// Salva na fila SPIFFS
// ═════════════════════════════════════════════════════════
void salvarNaFila(float temp, float umid, uint32_t tc) {
  File f = SPIFFS.open(ARQUIVO_FILA, "r");
  JsonDocument doc;
  if (f) { deserializeJson(doc, f); f.close(); }

  JsonArray arr = doc["q"].is<JsonArray>() ? doc["q"].as<JsonArray>() : doc["q"].to<JsonArray>();
  if ((int)arr.size() >= MAX_REGISTROS) arr.remove(0);

  JsonObject reg = arr.add<JsonObject>();
  reg["t"] = temp; reg["h"] = umid; reg["tc"] = tc;

  File fw = SPIFFS.open(ARQUIVO_FILA, "w");
  if (fw) { serializeJson(doc, fw); fw.close(); }
  Serial.print("[FILA] Guardado offline. Total: "); Serial.println(arr.size());
}

// ═════════════════════════════════════════════════════════
// Descarrega fila → API
// ═════════════════════════════════════════════════════════
void descarregarFila() {
  if (!SPIFFS.exists(ARQUIVO_FILA) || WiFi.status() != WL_CONNECTED) return;

  File f = SPIFFS.open(ARQUIVO_FILA, "r");
  if (!f) return;

  JsonDocument doc;
  if (deserializeJson(doc, f) || !doc["q"].is<JsonArray>()) { f.close(); return; }
  f.close();

  JsonArray arr = doc["q"].as<JsonArray>();
  int total = arr.size();
  if (total == 0) return;

  Serial.print("[SYNC] Descarregando "); Serial.print(total); Serial.println(" registros...");
  uint32_t agora_br = getTimestampBR();
  int enviados = 0;

  for (JsonObject reg : arr) {
    float t = reg["t"]; float h = reg["h"]; uint32_t tc = reg["tc"];
    uint32_t idade = (agora_br >= tc) ? (agora_br - tc) : 0;
    
    if (enviarParaAPI(t, h, idade)) { 
      enviados++; 
      delay(500); // Pausa para não sobrecarregar API
    } else break;
  }

  if (enviados == total) {
    SPIFFS.remove(ARQUIVO_FILA);
    Serial.println("[SYNC] Fila zerada!");
  } else {
    JsonDocument novo;
    JsonArray na = novo["q"].to<JsonArray>();
    for (int i = enviados; i < total; i++) na.add(arr[i]);
    File fw = SPIFFS.open(ARQUIVO_FILA, "w");
    if (fw) { serializeJson(novo, fw); fw.close(); }
  }
}

// ═════════════════════════════════════════════════════════
// Processa pacote LoRa recebido
// ═════════════════════════════════════════════════════════
void processarLoRa(const String &hex) {
  PacoteDados p;
  if (!hexParaBytes(hex, (uint8_t*)&p, sizeof(p))) return;

  Serial.print("\n[LoRa RX] Temp="); Serial.print(p.temperatura, 1);
  Serial.print(" Umid="); Serial.println(p.umidade, 1);

  uint32_t ts_coleta = getTimestampBR(); // Carimbo exato do recebimento

  if (wifiOnline && WiFi.status() == WL_CONNECTED) {
    // === COM INTERNET ===
    redeEstavaOffline = false;
    enviarParaAPI(p.temperatura, p.umidade, 0); // Idade 0 (tempo real)
    descarregarFila();
  } else {
    // === SEM INTERNET ===
    unsigned long agora_ms = millis();
    
    if (!redeEstavaOffline || (agora_ms - tUltimoSalvar >= INTERVALO_SALVAR)) {
      Serial.println("[Rede] Offline. Condicao atingida. Salvando na SPIFFS...");
      salvarNaFila(p.temperatura, p.umidade, ts_coleta);
      tUltimoSalvar = agora_ms;
      redeEstavaOffline = true;
    } else {
      unsigned long tempoRestante = (INTERVALO_SALVAR - (agora_ms - tUltimoSalvar)) / 1000;
      Serial.println("[Rede] Offline. Pacote descartado. Proxima gravacao em " + String(tempoRestante) + "s.");
    }
  }
}

// ═════════════════════════════════════════════════════════
// Event handler LoRa (Obrigatório para RoboCore P2P)
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

void setup() {
  Serial.begin(115200);
  delay(500);
  LoRaSerial.begin(115200, SERIAL_8N1, RXD2, TXD2);

  pinMode(PINO_BOTAO, INPUT_PULLUP);
  delay(2000);

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

void loop() {
  lorawan.listen(); // Mantém o rádio ouvindo ativamente
  unsigned long agora = millis();

  if (wifiOnline && WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Conexão perdida!");
    wifiOnline = false;
  }

  // Tenta reconectar o Wi-Fi se caiu
  if (!wifiOnline && (agora - tUltimaTentaWifi >= INTERVALO_TENTAR_WIFI)) {
    tUltimaTentaWifi = agora;
    bool eraOffline = !wifiOnline;
    if (conectarWifi() && eraOffline) {
      Serial.println("[WiFi] Reconectado! Iniciando fila...");
      descarregarFila();
    }
  }
}