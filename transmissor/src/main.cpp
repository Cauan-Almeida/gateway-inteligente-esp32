/*
 * LoRa Externo — Nó Sensor de Campo (Modo Broadcaster Cego)
 * - SEM Wi-Fi e SEM SPIFFS
 * - Envia temperatura e umidade a cada 5s
 * - Não espera ACK
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#include <RoboCore_SMW_SX1276M0.h>
#include <DHT.h>

// ─── Pinos ───────────────────────────────────────────────
#define RXD2       16
#define TXD2       17
#define PINO_RESET 5
#define PINO_DHT   12
#define PINO_BOTAO 4
#define PINO_LED   2

// ─── Calibração e Intervalos ─────────────────────────────
#define OFFSET_TEMP -2.53f // ajuste fino para bater com a referência de 24.17°C
#define INTERVALO_ENVIO 5000UL // 5 segundos
#define DEBOUNCE_BOTAO  2000UL

// ─── Struct pacote dados (12 bytes) ──────────────────────
struct __attribute__((__packed__)) PacoteDados {
  float    temperatura;     // 4 bytes
  float    umidade;         // 4 bytes
  uint32_t idade_segundos;  // 4 bytes (sempre 0 agora)
};

// ─── LoRa ────────────────────────────────────────────────
HardwareSerial LoRaSerial(2);
SMW_SX1276M0 lorawan(LoRaSerial);
CommandResponse response;

const char DEVADDR[]     = "00000000";
const char DEVADDR_P2P[] = "00000000";
const char APPSKEY[]     = "00000000000000000000000000000000";
const char NWKSKEY[]     = "00000000000000000000000000000000";

// ─── DHT11 ───────────────────────────────────────────────
DHT dht(PINO_DHT, DHT11);

// ─── Timers ──────────────────────────────────────────────
unsigned long tUltimoEnvio = 0;
unsigned long tUltimoBotao = 0;

// ═════════════════════════════════════════════════════════
// Bytes → HEX
// ═════════════════════════════════════════════════════════
String bytesParaHex(const uint8_t* d, size_t n) {
  String hex = "";
  for (size_t i = 0; i < n; i++) {
    if (d[i] < 16) hex += "0";
    hex += String(d[i], HEX);
  }
  hex.toUpperCase();
  return hex;
}

// ═════════════════════════════════════════════════════════
// Lê sensor DHT11
// ═════════════════════════════════════════════════════════
bool lerSensor(float &temp, float &umid) {
  temp = dht.readTemperature() + OFFSET_TEMP;
  umid = dht.readHumidity();
  return (!isnan(temp) && !isnan(umid));
}

// ═════════════════════════════════════════════════════════
// Feedback visual do LED
// ═════════════════════════════════════════════════════════
void indicarFalha() {
  digitalWrite(PINO_LED, HIGH);
}

void indicarSucesso() {
  digitalWrite(PINO_LED, HIGH);
  delay(120);
  digitalWrite(PINO_LED, LOW);
}

// ═════════════════════════════════════════════════════════
// Envia pacote sem esperar ACK
// ═════════════════════════════════════════════════════════
bool dispararPacote(float temp, float umid) {
  PacoteDados p;
  p.temperatura = temp; 
  p.umidade = umid; 
  p.idade_segundos = 0; // Externo não controla mais idade

  Serial.print("[TX] Temp="); Serial.print(temp, 1);
  Serial.print(" Umid="); Serial.println(umid, 1);

  String payload = bytesParaHex((uint8_t*)&p, sizeof(p));
  response = lorawan.sendX(10, payload);

  if (response == CommandResponse::OK) {
    Serial.println("[LoRa] Envio OK");
    indicarSucesso();
    return true;
  }

  Serial.println("[LoRa] ERRO no envio");
  indicarFalha();
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  LoRaSerial.begin(115200, SERIAL_8N1, RXD2, TXD2);

  pinMode(PINO_BOTAO, INPUT_PULLUP);
  pinMode(PINO_LED, OUTPUT);
  digitalWrite(PINO_LED, LOW);
  dht.begin();
  delay(2000);

  lorawan.setPinReset(PINO_RESET);
  lorawan.reset();

  Serial.println("=== LORA EXTERNO (BROADCASTER CEGO) ===");
  lorawan.set_DevAddr(DEVADDR);
  lorawan.set_P2P_DevAddr(DEVADDR_P2P);
  lorawan.set_AppSKey(APPSKEY);
  lorawan.set_NwkSKey(NWKSKEY);
  lorawan.set_P2P_SyncWord(18);

  response = lorawan.set_JoinMode(SMW_SX1276M0_JOIN_MODE_P2P);
  Serial.println(response == CommandResponse::OK ? "[LoRa] P2P OK" : "[LoRa] ERRO P2P");
  Serial.println("[Sistema] Pronto! Iniciando envios a cada 5s...");
}

void loop() {
  unsigned long agora = millis();
  float temp, umid;

  // ── BOTÃO (Envio Imediato) ────────────────────────────
  if (digitalRead(PINO_BOTAO) == LOW && (agora - tUltimoBotao > DEBOUNCE_BOTAO)) {
    tUltimoBotao = agora;
    if (lerSensor(temp, umid)) {
      Serial.print("[BOTAO] ");
      dispararPacote(temp, umid);
    } else {
      Serial.println("[BOTAO] ERRO na leitura do sensor");
      indicarFalha();
    }
    return;
  }

  // ── ENVIO CONTÍNUO (5s) ───────────────────────────────
  if (agora - tUltimoEnvio >= INTERVALO_ENVIO) {
    tUltimoEnvio = agora;
    if (lerSensor(temp, umid)) {
      dispararPacote(temp, umid);
    } else {
      Serial.println("[AUTO] ERRO na leitura do sensor");
      indicarFalha();
    }
  }
}