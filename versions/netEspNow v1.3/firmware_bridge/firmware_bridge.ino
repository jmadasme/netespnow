/**
 * Firmware ESP8266 - Bridge Central con Cifrado ECC (netEspNow v1.3)
 * ====================================================================
 * Placa: NodeMCU v3 (ESP-12E, CP2102)
 * 
 * Funcion:
 *   - Recibe comandos del usuario via Serial (115200 baud)
 *   - CIFRA los comandos con ECC asimetrico y los envia a la Puerta de Enlace
 *   - La Puerta de Enlace descifra, interroga al nodo, y devuelve respuesta cifrada
 *   - Bridge descifra la respuesta y la muestra por Serial
 *   - Comando de actuacion remota: "set <id> on|off" (GPIO salida del nodo)
 *   - Tabla de nodos con node_function y fw_version (aprendidos dinamicamente)
 *   - Watchdog de red: reinicia tras BRIDGE_WDT_TIMEOUTS timeouts consecutivos
 * 
 * Protocolo ESP-NOW:
 *   Bridge <-> Puerta: Mensajes cifrados (msg_type=2/3) con ECC
 *   Puerta <-> Nodos:  Mensajes planos con cabecera comun extendida (21 B)
 * 
 * Hardware:
 *   - D4 (GPIO2) -> LED_BUILTIN (azul, activo en LOW)
 *   - D9 (GPIO1) -> TX (Serial, 115200 baud)
 *   - D10(GPIO3) -> RX (Serial)
 * 
 * Version: 1.3
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <EEPROM.h>
#include <uECC.h>
#include <bearssl/bearssl.h>
#include "netesponw_comun.h"

// ============================================================
// Configuracion general
// ============================================================

#define SERIAL_BAUD         115200
#define POLL_INTERVAL_MS    3000     // Intervalo entre auto-polls
#define RESPONSE_TIMEOUT_MS 5000     // Timeout espera respuesta Puerta de Enlace
#define LED_PIN             LED_BUILTIN
#define LED_ON              LOW
#define LED_OFF             HIGH

// MAC de broadcast
const uint8_t ESP_NOW_BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ============================================================
// MAC de la Puerta de Enlace (conocida, hardcodeada tras 
// aprendizaje. Inicialmente se envia a broadcast)
// ============================================================
static uint8_t puerta_mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static bool    puerta_mac_known = false;

// ============================================================
// Tipos de mensaje ESP-NOW
// ============================================================
#define MSG_TYPE_CRYPT_REQ   2   // Cifrado: Bridge -> Puerta de Enlace
#define MSG_TYPE_CRYPT_RESP  3   // Cifrado: Puerta de Enlace -> Bridge

// ============================================================
// Estructura: Mensaje cifrado (Bridge <-> Puerta de Enlace)
// ============================================================
#define CRYPTED_PAYLOAD_MAX 32
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;       // 2=bridge->puerta, 3=puerta->bridge
  uint8_t  node_id;        // ID del nodo destino (0-4) o 0xFF
  uint32_t counter;        // Contador correlativo
  uint8_t  payload[CRYPTED_PAYLOAD_MAX]; // Datos cifrados
  uint8_t  payload_len;    // Longitud real del payload
  uint8_t  signature[64];  // Firma ECDSA
} EspNowCryptedMessage;

// ============================================================
// EEPROM Layout
// ============================================================
#define EEPROM_SIZE          512
#define EEPROM_MAGIC         0x4E533137  // "NS17"
#define EEPROM_ADDR_MAGIC    0
#define EEPROM_ADDR_PRIVKEY  4      // 32 bytes
#define EEPROM_ADDR_PUBKEY   36     // 64 bytes
#define EEPROM_ADDR_PEER_PUB 100    // 64 bytes
#define EEPROM_ADDR_PEER_MAC 164    // 6 bytes
// 170-511: Reservado (342 bytes)

#define ECC_PRIV_KEY_SIZE  32
#define ECC_PUB_KEY_SIZE   64

// ============================================================
// Tabla de nodos (informacion local para comandos list/stats)
// ============================================================
#define NUM_NODES 5

typedef struct {
  uint8_t  id;
  uint8_t  mac[6];
  bool     known;
  // Aprendido dinamicamente de cada respuesta (v1.3)
  uint8_t  node_function;
  uint8_t  fw_version;
  uint8_t  gpio_in;
  uint16_t adc_raw;
  uint8_t  gpio_out;
  bool     has_io;   // true si recibio respuesta con estado I/O
} EspNowNode;

// MACs de los nodos sensores (las mismas que v1.1)
EspNowNode nodes[NUM_NODES] = {
  { 0, {0x84, 0xF3, 0xEB, 0x7C, 0x99, 0x1E}, true },
  { 1, {0x80, 0x7D, 0x3A, 0x2B, 0xE3, 0x59}, true },
  { 2, {0x8C, 0xAA, 0xB5, 0x0F, 0x39, 0x7F}, true },
  { 3, {0x50, 0x02, 0x91, 0xDE, 0x0D, 0x7B}, true },
  { 4, {0x48, 0x3F, 0xDA, 0xAA, 0x8E, 0x6D}, true },
};

// ============================================================
// Variables de estado
// ============================================================

// Claves ECC
static uint8_t my_private_key[ECC_PRIV_KEY_SIZE];
static uint8_t my_public_key[ECC_PUB_KEY_SIZE];
static uint8_t peer_public_key[ECC_PUB_KEY_SIZE];
static bool    keys_initialized = false;
static bool    peer_key_known = false;

// Estado de comunicacion
typedef enum {
  BRIDGE_IDLE,
  BRIDGE_WAITING_RESPONSE  // Esperando respuesta cifrada de Puerta
} BridgeState;

static BridgeState bridge_state = BRIDGE_IDLE;
static unsigned long request_sent_time = 0;
static uint8_t last_polled_node_id = 0;
static bool    auto_poll_enabled = true;
static unsigned long last_poll_time = 0;

// Buffer de respuesta cifrada (desde ISR)
static volatile bool   g_crypt_response_received = false;
static volatile uint8_t g_crypt_sender_mac[6];
static volatile uint8_t g_crypt_buffer[sizeof(EspNowCryptedMessage)];

// Estadisticas
static uint32_t total_requests = 0;
static uint32_t total_responses = 0;
static uint32_t total_timeouts = 0;
static uint32_t msg_counter = 0;

// Watchdog de red (Bridge)
static uint32_t consecutive_timeouts = 0;

// MAC propia
static uint8_t my_mac[6];

// ============================================================
// Funciones EEPROM
// ============================================================

void initEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
}

bool hasKeysInEEPROM() {
  uint32_t magic;
  EEPROM.get(EEPROM_ADDR_MAGIC, magic);
  return (magic == EEPROM_MAGIC);
}

void saveKeysToEEPROM() {
  uint32_t magic = EEPROM_MAGIC;
  EEPROM.put(EEPROM_ADDR_MAGIC, magic);
  EEPROM.put(EEPROM_ADDR_PRIVKEY, my_private_key);
  EEPROM.put(EEPROM_ADDR_PUBKEY, my_public_key);
  EEPROM.commit();
  Serial.println("[EEPROM] Claves propias guardadas");
}

void loadKeysFromEEPROM() {
  EEPROM.get(EEPROM_ADDR_PRIVKEY, my_private_key);
  EEPROM.get(EEPROM_ADDR_PUBKEY, my_public_key);
}

void savePeerKeyToEEPROM() {
  EEPROM.put(EEPROM_ADDR_PEER_PUB, peer_public_key);
  EEPROM.put(EEPROM_ADDR_PEER_MAC, puerta_mac);
  EEPROM.commit();
  Serial.println("[EEPROM] Clave publica de Puerta de Enlace guardada");
}

bool loadPeerKeyFromEEPROM() {
  uint8_t test[ECC_PUB_KEY_SIZE];
  EEPROM.get(EEPROM_ADDR_PEER_PUB, test);
  
  bool all_zero = true;
  for (int i = 0; i < ECC_PUB_KEY_SIZE; i++) {
    if (test[i] != 0) { all_zero = false; break; }
  }
  if (all_zero) return false;
  
  EEPROM.get(EEPROM_ADDR_PEER_PUB, peer_public_key);
  EEPROM.get(EEPROM_ADDR_PEER_MAC, puerta_mac);
  peer_key_known = true;
  puerta_mac_known = true;
  return true;
}

// ============================================================
// CLAVES ASIMETRICAS EN DURO (mismo par para Bridge y Puerta)
// ============================================================
// Bridge MAC:    48:3F:DA:05:86:36
// Puerta MAC:    C4:5B:BE:74:DB:4A

static const uint8_t HARDCODED_PRIV_KEY[ECC_PRIV_KEY_SIZE] = {
  0x35, 0xDF, 0xFC, 0xEB, 0xBD, 0x3F, 0x65, 0x14,
  0x13, 0x8C, 0x4D, 0x65, 0xC5, 0x08, 0x4E, 0xCC,
  0x00, 0x33, 0x27, 0xF1, 0xA0, 0x32, 0x02, 0xB1,
  0x10, 0xA9, 0xFA, 0x53, 0x05, 0xDF, 0x06, 0x3A,
};

static const uint8_t HARDCODED_PUB_KEY[ECC_PUB_KEY_SIZE] = {
  0xD9, 0xC7, 0x8B, 0xAC, 0x36, 0x57, 0x62, 0xFC,
  0x2D, 0x95, 0x33, 0x49, 0xC7, 0xEB, 0x55, 0x27,
  0xD5, 0xF9, 0x4C, 0x07, 0x33, 0x00, 0x69, 0x04,
  0xBB, 0xE2, 0xC1, 0xA6, 0x83, 0xEB, 0xAA, 0x9E,
  0xBE, 0x9F, 0xD4, 0xA9, 0x78, 0x6D, 0x42, 0x1B,
  0x6E, 0x71, 0x39, 0x2C, 0x39, 0xB4, 0xEB, 0x33,
  0x88, 0x66, 0xEA, 0xA1, 0x35, 0x5E, 0x47, 0xA6,
  0x9D, 0x2B, 0x9A, 0x14, 0xF8, 0x86, 0x24, 0xB5,
};

static const uint8_t HARDCODED_PEER_PUB_KEY[ECC_PUB_KEY_SIZE] = {
  0xD9, 0xC7, 0x8B, 0xAC, 0x36, 0x57, 0x62, 0xFC,
  0x2D, 0x95, 0x33, 0x49, 0xC7, 0xEB, 0x55, 0x27,
  0xD5, 0xF9, 0x4C, 0x07, 0x33, 0x00, 0x69, 0x04,
  0xBB, 0xE2, 0xC1, 0xA6, 0x83, 0xEB, 0xAA, 0x9E,
  0xBE, 0x9F, 0xD4, 0xA9, 0x78, 0x6D, 0x42, 0x1B,
  0x6E, 0x71, 0x39, 0x2C, 0x39, 0xB4, 0xEB, 0x33,
  0x88, 0x66, 0xEA, 0xA1, 0x35, 0x5E, 0x47, 0xA6,
  0x9D, 0x2B, 0x9A, 0x14, 0xF8, 0x86, 0x24, 0xB5,
};

static const uint8_t HARDCODED_PEER_MAC[6] = {
  0xC4, 0x5B, 0xBE, 0x74, 0xDB, 0x4A
};

// ============================================================
// Funciones criptograficas ECC
// ============================================================

static int ecc_rng(uint8_t *dest, unsigned size) {
  for (unsigned i = 0; i < size; i++) {
    dest[i] = (uint8_t)os_random();
  }
  return 1;
}

// ============================================================
// Hash context SHA256 (BearSSL) para uECC_sign_deterministic
// ============================================================
typedef struct {
  uECC_HashContext uECC;
  br_sha256_context ctx;
} SHA256_HashContext;

static void init_SHA256(const uECC_HashContext *base) {
  SHA256_HashContext *context = (SHA256_HashContext *)base;
  br_sha256_init(&context->ctx);
}

static void update_SHA256(const uECC_HashContext *base,
                          const uint8_t *message,
                          unsigned message_size) {
  SHA256_HashContext *context = (SHA256_HashContext *)base;
  br_sha256_update(&context->ctx, message, message_size);
}

static void finish_SHA256(const uECC_HashContext *base, uint8_t *hash_result) {
  SHA256_HashContext *context = (SHA256_HashContext *)base;
  br_sha256_out(&context->ctx, hash_result);
}

void loadHardcodedKeys() {
  uECC_set_rng(&ecc_rng);
  memcpy(my_private_key, HARDCODED_PRIV_KEY, ECC_PRIV_KEY_SIZE);
  memcpy(my_public_key, HARDCODED_PUB_KEY, ECC_PUB_KEY_SIZE);
  memcpy(peer_public_key, HARDCODED_PEER_PUB_KEY, ECC_PUB_KEY_SIZE);
  memcpy(puerta_mac, HARDCODED_PEER_MAC, 6);
  
  keys_initialized = true;
  peer_key_known = true;
  puerta_mac_known = true;
  
  Serial.println("[OK] Claves ECC hardcodeadas cargadas en RAM");
}

void deriveXorKey(const uint8_t *shared_secret, uint32_t counter,
                  uint8_t *output, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    uint8_t idx = (uint8_t)((counter + i) % 32);
    uint8_t scramble = (uint8_t)((counter >> (8 * (i % 4))) & 0xFF);
    output[i] = shared_secret[idx] ^ scramble;
  }
}

void encryptXor(const uint8_t *plain, uint8_t *cipher, uint8_t len,
                const uint8_t *shared_secret, uint32_t counter) {
  uint8_t key_stream[CRYPTED_PAYLOAD_MAX];
  deriveXorKey(shared_secret, counter, key_stream, len);
  for (uint8_t i = 0; i < len; i++) {
    cipher[i] = plain[i] ^ key_stream[i];
  }
}

#define decryptXor(plain, cipher, len, secret, counter) \
  encryptXor(cipher, plain, len, secret, counter)

bool signMessage(const uint8_t *msg, uint8_t msg_len,
                 uint8_t *signature) {
  const struct uECC_Curve_t *curve = uECC_secp256r1();
  uint8_t hash[32];
  memset(hash, 0, 32);
  for (uint8_t i = 0; i < msg_len; i++) {
    hash[i % 32] ^= msg[i];
  }
  ESP.wdtDisable();
  unsigned long t0 = millis();
  uint8_t tmp[32 + 32 + 64];
  SHA256_HashContext hash_ctx = {
    {&init_SHA256, &update_SHA256, &finish_SHA256, 64, 32, tmp}
  };
  int ret = uECC_sign_deterministic(my_private_key, hash, 32,
                                    &hash_ctx.uECC, signature, curve);
  unsigned long dt = millis() - t0;
  ESP.wdtEnable(0);
  Serial.printf("[TIMING] Firma ECDSA: %lu ms\n", dt);
  return (ret == 1);
}

bool verifySignature(const uint8_t *msg, uint8_t msg_len,
                     const uint8_t *signature) {
  const struct uECC_Curve_t *curve = uECC_secp256r1();
  uint8_t hash[32];
  memset(hash, 0, 32);
  for (uint8_t i = 0; i < msg_len; i++) {
    hash[i % 32] ^= msg[i];
  }
  ESP.wdtDisable();
  unsigned long t0 = millis();
  int ret = uECC_verify(peer_public_key, hash, 32, signature, curve);
  unsigned long dt = millis() - t0;
  ESP.wdtEnable(0);
  Serial.printf("[TIMING] Verificacion ECDSA: %lu ms\n", dt);
  return (ret == 1);
}

// ============================================================
// Callbacks ISR ESP-NOW
// ============================================================

void onDataReceived(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len < 1) return;
  
  uint8_t msg_type = data[0];
  
  if (msg_type == MSG_TYPE_CRYPT_RESP) {
    // Respuesta cifrada de la Puerta de Enlace
    if (len >= 8) {
      memcpy((void*)g_crypt_sender_mac, mac, 6);
      size_t copy_len = (len < sizeof(EspNowCryptedMessage)) ? len : sizeof(EspNowCryptedMessage);
      memcpy((void*)g_crypt_buffer, data, copy_len);
      g_crypt_response_received = true;
    }
  }
  // Ignoramos mensajes tipo request/response planos
}

void onDataSent(uint8_t *mac, uint8_t status) {
  (void)mac;
  (void)status;
}

// ============================================================
// Enviar comando cifrado a la Puerta de Enlace
// ============================================================
bool sendEncryptedCommand(uint8_t node_id, const char *command) {
  if (!keys_initialized || !peer_key_known) {
    Serial.println("[ERROR] No hay claves ECC. Genere claves primero.");
    return false;
  }
  
  if (!puerta_mac_known) {
    Serial.println("[ERROR] No se conoce la MAC de la Puerta de Enlace.");
    return false;
  }
  
  // Calcular secreto compartido DETERMINISTA
  uint8_t shared_secret[ECC_PRIV_KEY_SIZE];
  for (int i = 0; i < ECC_PRIV_KEY_SIZE; i++) {
    shared_secret[i] = my_private_key[i] ^ peer_public_key[i];
  }
  
  EspNowCryptedMessage msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_type = MSG_TYPE_CRYPT_REQ;
  msg.node_id = node_id;
  msg.counter = msg_counter++;
  
  uint8_t cmd_len = strlen(command);
  if (cmd_len > CRYPTED_PAYLOAD_MAX) cmd_len = CRYPTED_PAYLOAD_MAX;
  msg.payload_len = cmd_len;
  
  encryptXor((uint8_t*)command, msg.payload, cmd_len, shared_secret, msg.counter);
  
  // Firmar: msg_type + node_id + counter + CIPHERTEXT_payload + payload_len
  uint8_t data_to_sign[4 + CRYPTED_PAYLOAD_MAX + 2];
  uint16_t idx = 0;
  data_to_sign[idx++] = msg.msg_type;
  data_to_sign[idx++] = msg.node_id;
  memcpy(data_to_sign + idx, &msg.counter, 4); idx += 4;
  memcpy(data_to_sign + idx, msg.payload, cmd_len); idx += cmd_len;
  data_to_sign[idx++] = msg.payload_len;
  
  if (!signMessage(data_to_sign, idx, msg.signature)) {
    Serial.println("[ERROR] Firma ECDSA fallo");
    return false;
  }
  
  if (!esp_now_is_peer_exist(puerta_mac)) {
    esp_now_add_peer(puerta_mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
  }
  
  int ret = esp_now_send(puerta_mac, (uint8_t*)&msg, sizeof(EspNowCryptedMessage));
  
  if (ret == 0) {
    Serial.printf("[ENVIO CIFRADO] Comando -> Puerta de Enlace (nodo=%d, cmd=%s)\n",
                  node_id, command);
    total_requests++;
    bridge_state = BRIDGE_WAITING_RESPONSE;
    request_sent_time = millis();
    return true;
  } else {
    Serial.printf("[ERROR] Fallo envio cifrado (codigo=%d)\n", ret);
    bridge_state = BRIDGE_IDLE;
    return false;
  }
}

// ============================================================
// Parsear respuesta con estado I/O
// Formato: "N4 c=9 u=100ms i=0 a=512 o=1"
// ============================================================
void parseIOState(const char *text, uint8_t node_id) {
  if (node_id >= NUM_NODES) return;
  
  EspNowNode *n = &nodes[node_id];
  // Buscar "i=", "a=", "o=" en el texto
  const char *p;
  bool found_i = false, found_a = false, found_o = false;
  
  p = strstr(text, "i=");
  if (p) { n->gpio_in = (uint8_t)atoi(p + 2); found_i = true; }
  
  p = strstr(text, "a=");
  if (p) { n->adc_raw = (uint16_t)atoi(p + 2); found_a = true; }
  
  p = strstr(text, "o=");
  if (p) { n->gpio_out = (uint8_t)atoi(p + 2); found_o = true; }
  
  // Solo marcar has_io si el texto contiene estado I/O real
  // (un "TIMEOUT: nodo sin respuesta" no debe marcar has_io)
  if (found_i && found_a && found_o) {
    n->has_io = true;
    // En v1.3 todos los nodos usan NODE_FUNC_IO y fw 13
    n->node_function = NODE_FUNC_IO;
    n->fw_version = NODE_FW_VERSION;
  }
}

// ============================================================
// Procesar respuesta cifrada de la Puerta de Enlace
// ============================================================
void processEncryptedResponse() {
  if (!g_crypt_response_received) return;
  g_crypt_response_received = false;
  
  EspNowCryptedMessage resp;
  memcpy(&resp, (const void*)g_crypt_buffer, sizeof(EspNowCryptedMessage));
  
  // Aprender MAC de Puerta si no la conocemos
  if (!puerta_mac_known) {
    memcpy(puerta_mac, (const void*)g_crypt_sender_mac, 6);
    puerta_mac_known = true;
    Serial.printf("[APRENDIZAJE] MAC Puerta de Enlace: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  puerta_mac[0], puerta_mac[1], puerta_mac[2],
                  puerta_mac[3], puerta_mac[4], puerta_mac[5]);
    
    if (!esp_now_is_peer_exist(puerta_mac)) {
      esp_now_add_peer(puerta_mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    }
  }
  
  if (!peer_key_known) {
    if (!loadPeerKeyFromEEPROM()) {
      Serial.println("[ERROR] No hay clave publica de Puerta de Enlace");
      bridge_state = BRIDGE_IDLE;
      return;
    }
  }
  
  // Verificar firma
  uint8_t data_to_verify[4 + CRYPTED_PAYLOAD_MAX + 2];
  uint16_t idx = 0;
  data_to_verify[idx++] = resp.msg_type;
  data_to_verify[idx++] = resp.node_id;
  memcpy(data_to_verify + idx, &resp.counter, 4); idx += 4;
  memcpy(data_to_verify + idx, resp.payload, resp.payload_len); idx += resp.payload_len;
  data_to_verify[idx++] = resp.payload_len;
  
  if (!verifySignature(data_to_verify, idx, resp.signature)) {
    Serial.println("[SEGURIDAD] Firma ECDSA INVALIDA - respuesta rechazada");
    bridge_state = BRIDGE_IDLE;
    return;
  }
  Serial.println("[SEGURIDAD] Firma ECDSA VALIDA - Puerta autenticada");
  
  // Descifrar payload
  uint8_t shared_secret[ECC_PRIV_KEY_SIZE];
  for (int i = 0; i < ECC_PRIV_KEY_SIZE; i++) {
    shared_secret[i] = my_private_key[i] ^ peer_public_key[i];
  }
  
  uint8_t plaintext[CRYPTED_PAYLOAD_MAX + 1];
  decryptXor(plaintext, resp.payload, resp.payload_len, shared_secret, resp.counter);
  plaintext[resp.payload_len] = '\0';
  
  total_responses++;
  bridge_state = BRIDGE_IDLE;
  
  // Watchdog de red: respuesta valida resetea el contador de timeouts
  consecutive_timeouts = 0;
  
  // Aprender funcion/version y estado I/O del nodo
  uint8_t node_id = resp.node_id;
  if (node_id < NUM_NODES) {
    parseIOState((const char*)plaintext, node_id);
  }
  
  // Mostrar resultado
  uint32_t rtt = millis() - request_sent_time;
  Serial.println("----------------------------------------");
  Serial.printf("RESPUESTA DESCIFRADA - Nodo #%d\n", resp.node_id);
  Serial.printf("  %s\n", (char*)plaintext);
  if (node_id < NUM_NODES && nodes[node_id].has_io) {
    Serial.printf("  [FUNC IO] in=%d adc=%d out=%d\n",
                  nodes[node_id].gpio_in, nodes[node_id].adc_raw, nodes[node_id].gpio_out);
  }
  Serial.printf("  RTT (via Puerta): %u ms\n", rtt);
  Serial.println("----------------------------------------");
  
  // Parpadeo de confirmacion
  digitalWrite(LED_PIN, LED_ON);
  delay(20);
  digitalWrite(LED_PIN, LED_OFF);
}

// ============================================================
// Comandos seriales
// ============================================================

void printHelp() {
  Serial.println();
  Serial.println("=== COMANDOS DISPONIBLES (netEspNow v1.3) ===");
  Serial.println("  ping <id>    : Preguntar a un nodo (0-4) via Puerta de Enlace");
  Serial.println("  ping all     : Preguntar a todos los nodos");
  Serial.println("  set <id> on|off : Actuar GPIO salida del nodo (0-4)");
  Serial.println("  list         : Mostrar tabla de nodos (funcion/version)");
  Serial.println("  stats        : Mostrar estadisticas y estado I/O por nodo");
  Serial.println("  auto on      : Activar interrogacion automatica");
  Serial.println("  auto off     : Desactivar interrogacion automatica");
  Serial.println("  keygen       : Restaurar par de claves ECC hardcodeadas");
  Serial.println("  learnkey     : Aprender clave publica de Puerta desde EEPROM");
  Serial.println("  showkey      : Mostrar clave publica de este Bridge");
  Serial.println("  help / ?     : Mostrar esta ayuda");
  Serial.println("=============================================");
  Serial.println();
}

void printList() {
  Serial.println("=== TABLA DE NODOS ===");
  for (int i = 0; i < NUM_NODES; i++) {
    const char *func_name = "?";
    if (nodes[i].node_function == NODE_FUNC_IO) func_name = "IO";
    else if (nodes[i].node_function == NODE_FUNC_UNDEFINED) func_name = "-";
    Serial.printf("  Nodo %d: %s [%02X:%02X:%02X:%02X:%02X:%02X] FUNC=%s FW=%d\n",
                  nodes[i].id,
                  nodes[i].known ? "CONOCIDO" : "DESCONOCIDO",
                  nodes[i].mac[0], nodes[i].mac[1], nodes[i].mac[2],
                  nodes[i].mac[3], nodes[i].mac[4], nodes[i].mac[5],
                  func_name, nodes[i].fw_version);
  }
  Serial.printf("  Puerta de Enlace: %s [%02X:%02X:%02X:%02X:%02X:%02X]\n",
                puerta_mac_known ? "CONOCIDA" : "DESCONOCIDA",
                puerta_mac[0], puerta_mac[1], puerta_mac[2],
                puerta_mac[3], puerta_mac[4], puerta_mac[5]);
  Serial.printf("  Cifrado ECC: %s | Clave Puerta: %s\n",
                keys_initialized ? "ACTIVO" : "INACTIVO",
                peer_key_known ? "CONOCIDA" : "DESCONOCIDA");
  Serial.println("========================");
}

void printStats() {
  Serial.println("=== ESTADISTICAS ===");
  Serial.printf("  Peticiones enviadas:  %u\n", total_requests);
  Serial.printf("  Respuestas OK:        %u\n", total_responses);
  Serial.printf("  Timeouts:             %u\n", total_timeouts);
  if (total_requests > 0) {
    Serial.printf("  Tasa de exito:        %u%%\n",
                  (total_responses * 100) / total_requests);
  }
  Serial.printf("  Claves ECC:           %s\n",
                keys_initialized ? "GENERADAS" : "NO GENERADAS");
  Serial.printf("  Clave Puerta:         %s\n",
                peer_key_known ? "CONOCIDA" : "DESCONOCIDA");
  Serial.println("  --- Estado I/O por nodo (ultima respuesta) ---");
  for (int i = 0; i < NUM_NODES; i++) {
    if (nodes[i].has_io) {
      Serial.printf("  Nodo %d: FUNC=IO FW=%d in=%d adc=%d out=%d\n",
                    nodes[i].id, nodes[i].fw_version,
                    nodes[i].gpio_in, nodes[i].adc_raw, nodes[i].gpio_out);
    } else {
      Serial.printf("  Nodo %d: sin respuesta I/O aun\n", nodes[i].id);
    }
  }
  Serial.println("====================");
}

void showPublicKey() {
  if (!keys_initialized) {
    Serial.println("[ERROR] No hay claves. Use 'keygen' primero.");
    return;
  }
  Serial.println("=== CLAVE PUBLICA DEL BRIDGE ===");
  Serial.print("  Hex: ");
  for (int i = 0; i < ECC_PUB_KEY_SIZE; i++) {
    if (i > 0) Serial.print(" ");
    Serial.printf("%02X", my_public_key[i]);
  }
  Serial.println();
  Serial.println("=================================");
}

void learnPeerKey() {
  if (loadPeerKeyFromEEPROM()) {
    Serial.println("[OK] Clave publica de Puerta de Enlace cargada de EEPROM");
    Serial.printf("  MAC Puerta: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  puerta_mac[0], puerta_mac[1], puerta_mac[2],
                  puerta_mac[3], puerta_mac[4], puerta_mac[5]);
  } else {
    Serial.println("[INFO] No hay clave de Puerta en EEPROM.");
  }
}

void processSerialCommand(const char *cmd) {
  while (*cmd == ' ') cmd++;
  
  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
    printHelp();
    return;
  }
  
  if (strcmp(cmd, "list") == 0) {
    printList();
    return;
  }
  
  if (strcmp(cmd, "stats") == 0) {
    printStats();
    return;
  }
  
  if (strcmp(cmd, "auto on") == 0) {
    auto_poll_enabled = true;
    last_poll_time = millis() - POLL_INTERVAL_MS;
    Serial.println("[OK] Interrogacion automatica ACTIVADA");
    return;
  }
  
  if (strcmp(cmd, "auto off") == 0) {
    auto_poll_enabled = false;
    Serial.println("[OK] Interrogacion automatica DESACTIVADA");
    return;
  }
  
  if (strcmp(cmd, "keygen") == 0) {
    Serial.println("[INFO] Restaurando claves ECC hardcodeadas de fabrica...");
    loadHardcodedKeys();
    saveKeysToEEPROM();
    savePeerKeyToEEPROM();
    Serial.println("[OK] Claves ECC de fabrica restauradas en EEPROM");
    return;
  }
  
  if (strcmp(cmd, "learnkey") == 0) {
    learnPeerKey();
    return;
  }
  
  if (strcmp(cmd, "showkey") == 0) {
    showPublicKey();
    return;
  }
  
  // Comando: set <id> on|off
  if (strncmp(cmd, "set", 3) == 0) {
    cmd += 3;
    while (*cmd == ' ') cmd++;
    
    if (bridge_state != BRIDGE_IDLE) {
      Serial.println("[WARN] Esperando respuesta de la Puerta de Enlace...");
      return;
    }
    
    int id = atoi(cmd);
    while (*cmd >= '0' && *cmd <= '9') cmd++;
    while (*cmd == ' ') cmd++;
    
    if (id < 0 || id >= NUM_NODES) {
      Serial.printf("[ERROR] ID invalido: %d (use 0-%d)\n", id, NUM_NODES - 1);
      return;
    }
    
    if (strncmp(cmd, "on", 2) == 0 || strncmp(cmd, "1", 1) == 0) {
      Serial.printf("[CMD] Actuando GPIO salida del Nodo %d -> ON\n", id);
      last_polled_node_id = id;
      char cmd_buf[16];
      snprintf(cmd_buf, sizeof(cmd_buf), "set %d on", id);
      if (!sendEncryptedCommand(id, cmd_buf)) {
        Serial.println("[ERROR] No se pudo enviar comando cifrado");
      }
    } else if (strncmp(cmd, "off", 3) == 0 || strncmp(cmd, "0", 1) == 0) {
      Serial.printf("[CMD] Actuando GPIO salida del Nodo %d -> OFF\n", id);
      last_polled_node_id = id;
      char cmd_buf[16];
      snprintf(cmd_buf, sizeof(cmd_buf), "set %d off", id);
      if (!sendEncryptedCommand(id, cmd_buf)) {
        Serial.println("[ERROR] No se pudo enviar comando cifrado");
      }
    } else {
      Serial.println("[ERROR] Valor invalido: use 'set <id> on|off'");
    }
    return;
  }
  
  // Comando: ping <id> o ping all
  if (strncmp(cmd, "ping", 4) == 0) {
    cmd += 4;
    while (*cmd == ' ') cmd++;
    
    if (bridge_state != BRIDGE_IDLE) {
      Serial.println("[WARN] Esperando respuesta de la Puerta de Enlace...");
      return;
    }
    
    if (strcmp(cmd, "all") == 0) {
      Serial.println("[CMD] Preguntando a TODOS los nodos...");
      auto_poll_enabled = true;
      last_polled_node_id = 0;
      last_poll_time = millis() - POLL_INTERVAL_MS;
      return;
    }
    
    int id = atoi(cmd);
    if (id >= 0 && id < NUM_NODES) {
      Serial.printf("[CMD] Preguntando a Nodo %d via Puerta de Enlace...\n", id);
      last_polled_node_id = id;
      
      char cmd_buf[16];
      snprintf(cmd_buf, sizeof(cmd_buf), "ping %d", id);
      
      if (!sendEncryptedCommand(id, cmd_buf)) {
        Serial.println("[ERROR] No se pudo enviar comando cifrado");
      }
      return;
    }
    
    Serial.printf("[ERROR] ID invalido: %s (use 0-%d)\n", cmd, NUM_NODES - 1);
    return;
  }
  
  Serial.printf("[ERROR] Comando desconocido: '%s' (escriba 'help')\n", cmd);
}

// ============================================================
// Lectura de comandos seriales
// ============================================================
#define SERIAL_BUF_SIZE 32
static char serial_buffer[SERIAL_BUF_SIZE];
static uint8_t serial_idx = 0;

void readSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    
    if (c == '\n' || c == '\r') {
      if (serial_idx > 0) {
        serial_buffer[serial_idx] = '\0';
        processSerialCommand(serial_buffer);
        serial_idx = 0;
      }
    } else if (serial_idx < SERIAL_BUF_SIZE - 1) {
      serial_buffer[serial_idx++] = c;
    }
  }
}

// ============================================================
// Watchdog de red del Bridge
// ============================================================
void checkNetworkWatchdog() {
  if (consecutive_timeouts >= BRIDGE_WDT_TIMEOUTS) {
    Serial.printf("[WDT-RED] %d timeouts consecutivos, reiniciando...\n",
                  BRIDGE_WDT_TIMEOUTS);
    delay(100);
    ESP.restart();
  }
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.println();
  Serial.println("============================================");
  Serial.println("  ESP8266 BRIDGE - Red ESP-NOW Cifrada v1.3");
  Serial.println("  Bridge -> Puerta de Enlace (ECC)");
  Serial.println("============================================");
  Serial.println();
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);
  
  uECC_set_rng(&ecc_rng);
  initEEPROM();
  
  if (hasKeysInEEPROM()) {
    loadKeysFromEEPROM();
    Serial.println("[EEPROM] Claves ECC cargadas");
    
    if (loadPeerKeyFromEEPROM()) {
      Serial.println("[EEPROM] Clave publica de Puerta de Enlace cargada");
      Serial.printf("  MAC Puerta: %02X:%02X:%02X:%02X:%02X:%02X\n",
                    puerta_mac[0], puerta_mac[1], puerta_mac[2],
                    puerta_mac[3], puerta_mac[4], puerta_mac[5]);
    } else {
      Serial.println("[EEPROM] Copiando clave publica hardcodeada de Puerta...");
      memcpy(peer_public_key, HARDCODED_PEER_PUB_KEY, ECC_PUB_KEY_SIZE);
      memcpy(puerta_mac, HARDCODED_PEER_MAC, 6);
      peer_key_known = true;
      puerta_mac_known = true;
      savePeerKeyToEEPROM();
    }
  } else {
    Serial.println("[EEPROM] Primera ejecucion. Grabando claves hardcodeadas...");
    loadHardcodedKeys();
    saveKeysToEEPROM();
    savePeerKeyToEEPROM();
    Serial.println("[OK] Claves ECC hardcodeadas guardadas en EEPROM");
  }
  
  keys_initialized = true;
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  // NOTA: NO usar delay() aqui. Bug del core ESP8266 3.1.2.
  
  // Fijar canal WiFi 1 explícitamente.
  wifi_set_channel(ESP_NOW_CHANNEL);
  
  Serial.print("MAC del Bridge: ");
  Serial.println(WiFi.macAddress());
  WiFi.macAddress(my_mac);
  Serial.printf("Canal WiFi: %d\n", WiFi.channel());
  
  int ret = esp_now_init();
  if (ret != 0) {
    Serial.println("[FATAL] Error al inicializar ESP-NOW");
    delay(5000);
    ESP.restart();
  }
  
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onDataReceived);
  esp_now_register_send_cb(onDataSent);
  
  esp_now_add_peer((uint8_t*)ESP_NOW_BROADCAST_MAC, ESP_NOW_ROLE_COMBO, ESP_NOW_CHANNEL, NULL, 0);
  
  Serial.println("[OK] ESP-NOW inicializado");
  Serial.println("[OK] Bridge listo. Use 'help' para comandos.");
  Serial.println();
  Serial.printf("Auto-polling: %s\n", auto_poll_enabled ? "ACTIVADO" : "DESACTIVADO");
  Serial.printf("Intervalo: %d ms\n", POLL_INTERVAL_MS);
  Serial.println("----------------------------------------");
  Serial.println();
  
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, LED_ON);
    delay(100);
    digitalWrite(LED_PIN, LED_OFF);
    delay(100);
  }
  
  last_poll_time = millis();
}

// ============================================================
// Loop principal
// ============================================================
void loop() {
  unsigned long now = millis();
  
  // 1. Procesar respuesta cifrada de Puerta de Enlace
  processEncryptedResponse();
  
  // 2. Leer comandos serial del usuario
  readSerialCommands();
  
  // 3. Verificar timeout de respuesta
  if (bridge_state == BRIDGE_WAITING_RESPONSE) {
    if (now - request_sent_time > RESPONSE_TIMEOUT_MS) {
      bridge_state = BRIDGE_IDLE;
      total_timeouts++;
      consecutive_timeouts++;
      Serial.printf("[TIMEOUT] Puerta de Enlace - Sin respuesta (%u ms)\n",
                    RESPONSE_TIMEOUT_MS);
      Serial.println("[INFO] Verifique que la Puerta de Enlace este encendida y dentro del alcance");
    }
  }
  
  // 4. Auto-polling (si activo)
  if (auto_poll_enabled && bridge_state == BRIDGE_IDLE && 
      (now - last_poll_time >= POLL_INTERVAL_MS)) {
    last_poll_time = now;
    
    last_polled_node_id = last_polled_node_id % NUM_NODES;
    
    char cmd_buf[16];
    snprintf(cmd_buf, sizeof(cmd_buf), "ping %d", last_polled_node_id);
    
    Serial.printf("[AUTO] Preguntando a Nodo %d...\n", last_polled_node_id);
    sendEncryptedCommand(last_polled_node_id, cmd_buf);
    
    last_polled_node_id = (last_polled_node_id + 1) % NUM_NODES;
  }
  
  // 5. Stats periodicos (cada 30s si auto-poll activo)
  static unsigned long last_stats_time = 0;
  if (auto_poll_enabled && (now - last_stats_time >= 30000)) {
    last_stats_time = now;
    printStats();
  }
  
  // 6. Watchdog de red
  checkNetworkWatchdog();
  
  delay(1);
}
