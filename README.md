# 📡 Sistema de Telemetria Ambiental Resiliente (LoRa P2P)

**Universidade de Vassouras — Campus Maricá**  
**Integrantes:** Cauan Ferreira de Almeida, Daniel Daigo Sasagawa, Victor Ângelo Bastos Ferreira  
**Data:** Junho de 2026

Este repositório contém a infraestrutura completa de comunicação sem fio e processamento de dados de um sistema de telemetria ambiental. O projeto utiliza dois módulos ESP32 com rádios LoRa (chip SMW-SX1276M0) operando em modo Ponto a Ponto (P2P), sem a necessidade de infraestrutura LoRaWAN pré-existente.

---

## 🏗️ Estrutura do Repositório (Monorepo)

- 📁 **`/transmissor`** — Código do nó cego de campo. Lê o sensor DHT11 e transmite uma struct binária via LoRa.
- 📁 **`/gateway`** — Código do nó interno (Gateway Inteligente). Recebe pacotes via LoRa, lê o próprio sensor, gerencia conectividade Wi-Fi, salva dados offline (SPIFFS) e envia tudo via HTTP POST.
- 📁 **`/utils`** — Scripts utilitários, incluindo o código de Factory Reset.

---

## 🗺️ Arquitetura do Sistema

O fluxo de dados foi desenhado para ser totalmente resiliente a quedas de energia e de internet local.

```
[LoRa Externo — Campo]          [LoRa Interno — Gateway]
       DHT11                           Wi-Fi 2.4GHz
         │                             NTP → Hora BR
  Struct 12 bytes                            │
 ─────────────── LoRa P2P ────────────────   │
         │                                   │
  SPIFFS Offline                        SPIFFS Offline
         │                                   │
         └─────── API Oracle Cloud ──────────┘
                  (InfluxDB + Front-end)
```

> **Nota:** A infraestrutura em nuvem na Oracle Cloud, banco de dados e a aplicação Front-end foram desenvolvidas pelo integrante Daniel Daigo Sasagawa.

---

## 📖 A Saga da Engenharia: Do Zero ao Sistema Resiliente

O que parecia ser uma simples conexão de dois rádios se transformou em um profundo trabalho de engenharia reversa, troubleshooting e arquitetura de software. Abaixo, detalhamos os desafios enfrentados e como foram superados.

### 1. A Ilusão do P2P Fácil e a "Máquina da Verdade"

A princípio, utilizamos os códigos de exemplo padrão da fabricante, mas os módulos travavam silenciosamente. Para entender o que ocorria dentro do rádio, transformamos o ESP32 em um **"Serial Bridge"** (uma ponte UART direta entre o PC e o rádio).

Descobrimos que a documentação sugeria o baud rate errado (usava 9600, o real era 115200) e que o rádio inicializava em modo OTAA, bloqueando e ignorando qualquer comando AT de configuração.

### 2. O Contato com o Suporte da RoboCore

Após semanas de tentativas frustradas (e até suspeitas de defeito de hardware), documentamos o comportamento e enviamos um e-mail detalhado para a engenharia da RoboCore.

A resposta deles foi decisiva: revelaram que, devido ao design do DevKit, era **obrigatório forçar um reset físico no hardware** antes de inicializar a biblioteca. Adicionar as linhas `lorawan.setPinReset(5);` e `lorawan.reset();` foi o ponto de virada para o rádio finalmente aceitar os comandos.

### 3. O Mistério da Região KR920 e o Factory Reset

Meses depois, a comunicação parou abruptamente. Após inúmeros testes, desenvolvemos um script de Factory Reset (`AT+FRESET`). Isso resolveu o travamento, mas revelou um novo problema: o reset de fábrica revertia a placa para a região **KR920** (Coreia), incompatível com a placa que estava em AU915.

A descoberta: o modo P2P funciona independentemente da região geográfica, **desde que as duas placas estejam na mesma região**. Fizemos o factory reset em ambas para pareá-las em KR920, e a comunicação foi reestabelecida.

### 4. O Gargalo do Payload (JSON vs. Struct Binária)

Ao tentar enviar os dados do sensor, percebemos que o formato JSON (`{"d":"lora_externo","t":23.8...}`) era muito pesado (~40 bytes) e sofria truncamento durante a transmissão via LoRa.

A solução de engenharia foi abandonar o JSON no ar e criar uma **Struct Binária packed**:

```cpp
struct __attribute__((__packed__)) PacoteDados {
  float    temperatura;    // 4 bytes
  float    umidade;        // 4 bytes
  uint32_t idade_segundos; // 4 bytes
};
```

Isso comprimiu a mensagem para exatos **12 bytes**, garantindo 100% de integridade nos dados transmitidos.

### 5. A Obra de Arte: O Gateway Inteligente e Auto-Recovery

Para garantir que nenhum dado do campo fosse perdido caso a internet ou o Wi-Fi do Gateway caíssem, foi desenvolvida uma lógica de **Alta Resiliência e Armazenamento Offline**:

- **Duas Filas na SPIFFS:** O gateway possui duas filas independentes na memória flash (uma para os dados recebidos via LoRa e outra para as leituras do próprio sensor interno). Se a rede cai, ele salva um registro a cada 10 minutos localmente.

- **Sincronização NTP:** No boot, o ESP32 busca a hora exata da internet (UTC-3). Quando a rede cai, ele salva o timestamp exato em que a leitura foi feita.

- **Auto-Recovery:** Assim que o Wi-Fi e a API voltam a responder, o loop do Gateway entra em modo de descarregamento rápido. Ele calcula a "idade" exata de cada registro armazenado offline e envia os pacotes sequencialmente (1 por ciclo) para a API, até que as filas estejam completamente zeradas.

---

## 🛠️ Tecnologias e Ferramentas Utilizadas

| Categoria | Tecnologia |
|---|---|
| IDE / Compilador | VS Code + PlatformIO |
| Linguagem | C++ (Arduino Framework) |
| Microcontrolador | ESP32 (Espressif) |
| Rádio LoRa | SMW-SX1276M0 |
| Sensores | DHT11 (Calibrados) |
| Armazenamento Local | SPIFFS (ESP32 Flash) |
| Gerenciamento de Tempo | NTP (pool.ntp.org) |
| Bibliotecas | RoboCore_SMW_SX1276M0, DHT sensor library, ArduinoJson |
