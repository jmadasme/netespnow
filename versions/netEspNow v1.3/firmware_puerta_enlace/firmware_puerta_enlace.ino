/**
 * Firmware ESP8266 - Puerta de Enlace (Cifrado Asimetrico ECC) netEspNow v1.3
 * ==============================================================
 * Placa: ESP8266EX ceramica (sin GPIO accesibles)
 * 
 * Funcion:
 *   - Intermediario cifrado entre Bridge (PC) y nodos sensores 0-4
 *   - Recibe comandos cifrados del Bridge via ESP-NOW
 *   - Descifra usando ECC (clave privada en EEPROM)
 *   - Reenvia al nodo destino en formato plano extendido
 *     (cabecera comun de 21 B, msg_type=0 request / msg_type=2 comando)
 *   - Espera respuesta del nodo (cabecera comun de 21 B, msg_type=1)
 *   - Cifra la respuesta (texto corto con estado I/O) y la reenvia al Bridge
 *   - Watchdog de red: reinicia si no recibe NINGUN mensaje cifrado
 *     del Bridge en PUERTA_WDT_TIMEOUT_MS
 * 
 * Flujo:
 *   Bridge --(ESP-NOW cifrado)--> Puerta de Enlace
 *   Puerta de Enlace --(ESP-NOW plano 21 B)--> Nodo X
 *   Nodo X --(ESP-NOW plano 21 B)--> Puerta de Enlace
 *   Puerta de Enlace --(ESP-NOW cifrado)--> Bridge
 * 
 * Cifrado:
 *   - ECC secp256r1 (uECC library)
 *   - ECDH para derivar secreto compartido
 *   - XOR con keystream derivado del secreto compartido + contador
 *   - ECDSA para firma/verificacion de autenticidad
 *   - Claves almacenadas en EEPROM
 * 
 * Version: 1.3
 */

// ============================================================
// Descomentar SOLO si el USB de la placa ceramica soporta
// datos (TX/RX). Por defecto esta deshabilitado porque la
// placa ceramica solo tiene USB para alimentacion.
// ============================================================
#define ENABLE_SERIAL_DEBUG

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <EEPROM.h>
#include <uECC.h>
#include <bearssl/bearssl.h>
#include "netesponw_comun.h"

// ============================================================
// Configuracion general
// ============================================================

#ifdef ENABLE_SERIAL_DEBUG
  #define SERIAL_BAUD 115200
  #define DEBUG_PRINT(x)   Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(...)
#endif

#define NODE_TIMEOUT_MS     1000    // Timeout espera respuesta de nodo
#define HEARTBEAT_MS        2000    // LED virtual (latido interno)

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
  uint32_t counter;        // Contador correlativo (anti-replay)
  uint8_t  payload[CRYPTED_PAYLOAD_MAX]; // Datos cifrados
  uint8_t  payload_len;    // Longitud real del payload
  uint8_t  signature[64];  // Firma ECDSA (Bridge firma, Puerta verifica)
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

// Tamanos ECC (secp256r1)
#define ECC_PRIV_KEY_SIZE  32
#define ECC_PUB_KEY_SIZE   64

// ============================================================
// Estado de la maquina de estados
// ============================================================
typedef enum {
  STATE_IDLE,                  // Esperando comando cifrado del Bridge
  STATE_ESP_RESP_NODO,         // Esperando respuesta del nodo
  STATE_CRYPT_AND_SEND        // Cifrando respuesta y enviando al Bridge
} PuertaState;

// ============================================================
// Variables globales
// ============================================================

// Claves ECC
static uint8_t my_private_key[ECC_PRIV_KEY_SIZE];
static uint8_t my_public_key[ECC_PUB_KEY_SIZE];
static uint8_t peer_public_key[ECC_PUB_KEY_SIZE];
static bool    keys_initialized = false;
static bool    peer_key_known = false;

// MAC del Bridge (peer)
static uint8_t bridge_mac[6];
static bool    bridge_mac_known = false;

// MAC de broadcast
const uint8_t ESP_NOW_BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Estado actual
static PuertaState state = STATE_IDLE;
static unsigned long state_enter_time = 0;

// Mensaje actual en procesamiento
static EspNowCryptedMessage current_crypted_msg;
static EspNowNodeMessageV13 current_nodo_response;

// Buffer ISR para mensajes recibidos
static volatile bool   g_crypt_msg_received = false;
static volatile uint8_t g_crypt_sender_mac[6];
static volatile uint8_t g_crypt_buffer[sizeof(EspNowCryptedMessage)];

static volatile bool   g_nodo_response_received = false;
static volatile uint8_t g_nodo_sender_mac[6];
static volatile uint8_t g_nodo_buffer[sizeof(EspNowNodeMessageV13)];

// Contadores
static uint32_t msg_counter = 0;
static uint32_t total_commands_decrypted = 0;
static uint32_t total_responses_encrypted = 0;
static uint32_t total_timeouts = 0;
static uint32_t total_errors = 0;

// Heartbeat interno (no tenemos LED)
static unsigned long last_heartbeat = 0;
static uint32_t heartbeat_count = 0;

// Watchdog de red: ultimo mensaje recibido del Bridge
static unsigned long last_bridge_ms = 0;

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
}

void loadKeysFromEEPROM() {
  EEPROM.get(EEPROM_ADDR_PRIVKEY, my_private_key);
  EEPROM.get(EEPROM_ADDR_PUBKEY, my_public_key);
}

void savePeerKeyToEEPROM() {
  EEPROM.put(EEPROM_ADDR_PEER_PUB, peer_public_key);
  EEPROM.put(EEPROM_ADDR_PEER_MAC, bridge_mac);
  EEPROM.commit();
}

bool loadPeerKeyFromEEPROM() {
  uint8_t test[ECC_PUB_KEY_SIZE];
  EEPROM.get(EEPROM_ADDR_PEER_PUB, test);
  
  // Verificar que no sea todo ceros
  bool all_zero = true;
  for (int i = 0; i < ECC_PUB_KEY_SIZE; i++) {
    if (test[i] != 0) { all_zero = false; break; }
  }
  if (all_zero) return false;
  
  EEPROM.get(EEPROM_ADDR_PEER_PUB, peer_public_key);
  EEPROM.get(EEPROM_ADDR_PEER_MAC, bridge_mac);
  peer_key_known = true;
  bridge_mac_known = true;
  return true;
}

// ============================================================
// CLAVES ASIMETRICAS EN DURO (mismo par para Bridge y Puerta)
// Generadas externamente con secp256r1, verificadas con ECDSA
// ============================================================
// Bridge MAC:    48:3F:DA:05:86:36
// Puerta MAC:    C4:5B:BE:74:DB:4A

// Clave privada COMPARTIDA (32 bytes) - misma en Bridge y Puerta
static const uint8_t HARDCODED_PRIV_KEY[ECC_PRIV_KEY_SIZE] = {
  0x35, 0xDF, 0xFC, 0xEB, 0xBD, 0x3F, 0x65, 0x14,
  0x13, 0x8C, 0x4D, 0x65, 0xC5, 0x08, 0x4E, 0xCC,
  0x00, 0x33, 0x27, 0xF1, 0xA0, 0x32, 0x02, 0xB1,
  0x10, 0xA9, 0xFA, 0x53, 0x05, 0xDF, 0x06, 0x3A,
};

// Clave publica COMPARTIDA (64 bytes, X+Y sin prefijo 04)
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

// Clave publica del peer (es la misma compartida)
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

// MAC del Bridge (conocida)
static const uint8_t HARDCODED_PEER_MAC[6] = {
  0x48, 0x3F, 0xDA, 0x05, 0x86, 0x36
};

// ============================================================
// Funciones criptograficas ECC
// ============================================================

// RNG para uECC (usa hardware ESP8266)
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
  // Registrar RNG para uECC (necesario para firmar/verificar)
  uECC_set_rng(&ecc_rng);
  
  // Copiar claves hardcodeadas a las variables globales
  memcpy(my_private_key, HARDCODED_PRIV_KEY, ECC_PRIV_KEY_SIZE);
  memcpy(my_public_key, HARDCODED_PUB_KEY, ECC_PUB_KEY_SIZE);
  memcpy(peer_public_key, HARDCODED_PEER_PUB_KEY, ECC_PUB_KEY_SIZE);
  memcpy(bridge_mac, HARDCODED_PEER_MAC, 6);
  
  keys_initialized = true;
  peer_key_known = true;
  bridge_mac_known = true;
  
  DEBUG_PRINTLN("[OK] Claves ECC hardcodeadas cargadas en RAM (RNG registrado)");
}

// Derivar keystream para XOR a partir del secreto compartido ECDH + contador
void deriveXorKey(const uint8_t *shared_secret, uint32_t counter, 
                  uint8_t *output, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    uint8_t idx = (uint8_t)((counter + i) % 32);
    uint8_t scramble = (uint8_t)((counter >> (8 * (i % 4))) & 0xFF);
    output[i] = shared_secret[idx] ^ scramble;
  }
}

// Cifrar: XOR del plaintext con keystream derivado
void encryptXor(const uint8_t *plain, uint8_t *cipher, uint8_t len, 
                const uint8_t *shared_secret, uint32_t counter) {
  uint8_t key_stream[CRYPTED_PAYLOAD_MAX];
  deriveXorKey(shared_secret, counter, key_stream, len);
  for (uint8_t i = 0; i < len; i++) {
    cipher[i] = plain[i] ^ key_stream[i];
  }
}

// Descifrar: mismo XOR (simetrico)
#define decryptXor(plain, cipher, len, secret, counter) \
  encryptXor(cipher, plain, len, secret, counter)

// Firmar mensaje con ECDSA
bool signMessage(const uint8_t *msg, uint8_t msg_len, 
                 uint8_t *signature, const uint8_t *private_key) {
  const struct uECC_Curve_t *curve = uECC_secp256r1();
  
  // Hash simple: XOR de todos los bytes para reducir a 32 bytes
  uint8_t hash[32];
  memset(hash, 0, 32);
  for (uint8_t i = 0; i < msg_len; i++) {
    hash[i % 32] ^= msg[i];
  }
  
  // uECC_sign_deterministic (RFC 6979): no usa RNG ni reintenta, por lo que
  // el tiempo esta acotado. Deshabilitamos el soft WDT durante la operacion
  // (el HARD WDT de ~6-8s sigue activo como red de seguridad).
  ESP.wdtDisable();
  unsigned long t0 = millis();
  
  uint8_t tmp[32 + 32 + 64];
  SHA256_HashContext hash_ctx = {
    {&init_SHA256, &update_SHA256, &finish_SHA256, 64, 32, tmp}
  };
  int ret = uECC_sign_deterministic(private_key, hash, 32,
                                    &hash_ctx.uECC, signature, curve);
  
  unsigned long dt = millis() - t0;
  ESP.wdtEnable(0);
  DEBUG_PRINTF("[TIMING] Firma ECDSA: %lu ms\n", dt);
  
  return (ret == 1);
}

// Verificar firma ECDSA
bool verifySignature(const uint8_t *msg, uint8_t msg_len,
                     const uint8_t *signature, const uint8_t *public_key) {
  const struct uECC_Curve_t *curve = uECC_secp256r1();
  
  uint8_t hash[32];
  memset(hash, 0, 32);
  for (uint8_t i = 0; i < msg_len; i++) {
    hash[i % 32] ^= msg[i];
  }
  
  // Igual que en signMessage: deshabilitar soft WDT durante la verificacion.
  ESP.wdtDisable();
  unsigned long t0 = millis();
  
  int ret = uECC_verify(public_key, hash, 32, signature, curve);
  
  unsigned long dt = millis() - t0;
  ESP.wdtEnable(0);
  DEBUG_PRINTF("[TIMING] Verificacion ECDSA: %lu ms\n", dt);
  
  return (ret == 1);
}

// ============================================================
// Callbacks ISR ESP-NOW
// ============================================================

// Datos recibidos
void onDataReceived(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len < 1) return;
  
  uint8_t msg_type = data[0];
  
  if (msg_type == MSG_TYPE_CRYPT_REQ) {
    // Mensaje cifrado del Bridge
    if (len >= 8) {
      memcpy((void*)g_crypt_sender_mac, mac, 6);
      memcpy((void*)g_crypt_buffer, data, len);
      g_crypt_msg_received = true;
    }
  }
  else if (msg_type == MSG_TYPE_RESPONSE) {
    // Respuesta plana de un nodo (cabecera comun extendida 21 B)
    if (len >= sizeof(EspNowNodeMessageV13)) {
      memcpy((void*)g_nodo_sender_mac, mac, 6);
      memcpy((void*)g_nodo_buffer, data, len);
      g_nodo_response_received = true;
    }
  }
  // Ignorar otros tipos
}

// Confirmacion de envio
void onDataSent(uint8_t *mac, uint8_t status) {
  (void)mac;
  if (status != 0) {
    DEBUG_PRINTF("[WARN] Envio ESP-NOW fallo (status=%d)\n", status);
  }
}

// ============================================================
// Enviar mensaje cifrado al Bridge
// ============================================================
bool sendEncryptedToBridge(uint8_t node_id, const uint8_t *data, uint8_t data_len) {
  if (!bridge_mac_known || !keys_initialized || !peer_key_known) {
    DEBUG_PRINTLN("[ERROR] No se puede cifrar: faltan claves o MAC del Bridge");
    return false;
  }
  
  // Defensa: nunca cifrar mas de CRYPTED_PAYLOAD_MAX bytes
  if (data_len > CRYPTED_PAYLOAD_MAX) {
    DEBUG_PRINTF("[WARN] data_len=%d excede CRYPTED_PAYLOAD_MAX, recortando\n", data_len);
    data_len = CRYPTED_PAYLOAD_MAX;
  }
  
  // Calcular secreto compartido DETERMINISTA
  uint8_t shared_secret[ECC_PRIV_KEY_SIZE];
  for (int i = 0; i < ECC_PRIV_KEY_SIZE; i++) {
    shared_secret[i] = my_private_key[i] ^ peer_public_key[i];
  }
  
  // Cifrar payload con XOR
  EspNowCryptedMessage msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_type = MSG_TYPE_CRYPT_RESP;
  msg.node_id = node_id;
  msg.counter = msg_counter++;
  msg.payload_len = data_len;
  
  encryptXor(data, msg.payload, data_len, shared_secret, msg.counter);
  
  // Firmar el mensaje (autenticidad): msg_type + node_id + counter + CIPHERTEXT + payload_len
  uint8_t msg_to_sign[4 + CRYPTED_PAYLOAD_MAX + 2];
  uint16_t sig_idx = 0;
  msg_to_sign[sig_idx++] = msg.msg_type;
  msg_to_sign[sig_idx++] = msg.node_id;
  memcpy(msg_to_sign + sig_idx, &msg.counter, 4); sig_idx += 4;
  memcpy(msg_to_sign + sig_idx, msg.payload, data_len); sig_idx += data_len;  // CIPHERTEXT!
  msg_to_sign[sig_idx++] = msg.payload_len;
  
  if (!signMessage(msg_to_sign, sig_idx, msg.signature, my_private_key)) {
    DEBUG_PRINTLN("[ERROR] Firma ECDSA fallo");
    total_errors++;
    return false;
  }
  
  // Enviar al Bridge
  int ret = esp_now_send(bridge_mac, (uint8_t*)&msg, sizeof(EspNowCryptedMessage));
  if (ret == 0) {
    DEBUG_PRINTF("[ENVIO] Respuesta cifrada -> Bridge (nodo=%d, len=%d)\n", 
                 node_id, data_len);
    total_responses_encrypted++;
    return true;
  } else {
    DEBUG_PRINTF("[ERROR] Fallo envio al Bridge (codigo=%d)\n", ret);
    total_errors++;
    return false;
  }
}

// ============================================================
// Enviar request plano a un nodo (cabecera comun extendida 21 B)
// ============================================================
bool sendMessageToNode(const EspNowNodeMessageV13 *msg) {
  // Enviar por broadcast (cualquier nodo que escuche y tenga ese ID respondera)
  // Nota: la API ESP8266 esp_now_send no marca el puntero como const.
  int ret = esp_now_send((uint8_t*)ESP_NOW_BROADCAST_MAC, 
                         (uint8_t*)msg, sizeof(EspNowNodeMessageV13));
  
  if (ret == 0) {
    DEBUG_PRINTF("[ENVIO] Msg plano -> Nodo %d (tipo=%d)\n", msg->node_id, msg->msg_type);
    return true;
  } else {
    DEBUG_PRINTF("[ERROR] Fallo envio a nodo %d (codigo=%d)\n", msg->node_id, ret);
    return false;
  }
}

// ============================================================
// Procesar comando descifrado del Bridge
// ============================================================
void processDecryptedCommand(const uint8_t *plaintext, uint8_t len, uint8_t node_id) {
  DEBUG_PRINTF("[CMD] Comando descifrado: nodo=%d, len=%d\n", node_id, len);
  DEBUG_PRINT("[CMD] Contenido: ");
  for (uint8_t i = 0; i < len; i++) {
    DEBUG_PRINT((char)plaintext[i]);
  }
  DEBUG_PRINTLN();
  
  // Parsear el comando en texto: "ping N" | "set N on" | "set N off"
  // Construir mensaje plano extendido hacia el nodo
  EspNowNodeMessageV13 msg;
  memset(&msg, 0, sizeof(msg));
  msg.node_id = node_id;
  msg.node_function = NODE_FUNC_IO;   // plantilla base I/O
  msg.fw_version = NODE_FW_VERSION;
  msg.rssi_db = 0;
  msg.counter = 0;
  msg.uptime_ms = 0;
  
  char cmd_text[CRYPTED_PAYLOAD_MAX + 1];
  uint8_t copy_len = (len < CRYPTED_PAYLOAD_MAX) ? len : CRYPTED_PAYLOAD_MAX;
  memcpy(cmd_text, plaintext, copy_len);
  cmd_text[copy_len] = '\0';
  
  // Comando "set <id> on|off"
  if (strncmp(cmd_text, "set ", 4) == 0) {
    char *p = cmd_text + 4;
    while (*p == ' ') p++;
    // saltar id
    while (*p >= '0' && *p <= '9') p++;
    while (*p == ' ') p++;
    msg.msg_type = MSG_TYPE_COMMAND;
    msg.payload[0] = CMD_SET_STATE;
    if (strncmp(p, "on", 2) == 0) {
      msg.payload[1] = 1;
    } else {
      msg.payload[1] = 0;
    }
    DEBUG_PRINTF("[CMD] SET_STATE nodo=%d valor=%u\n", node_id, msg.payload[1]);
  } else {
    // Cualquier otro comando: request de estado (ping)
    msg.msg_type = MSG_TYPE_REQUEST;
    msg.payload[0] = CMD_GET_STATE;
  }
  
  // Enviar al nodo y esperar respuesta
  state = STATE_ESP_RESP_NODO;
  state_enter_time = millis();
  
  if (!sendMessageToNode(&msg)) {
    DEBUG_PRINTLN("[ERROR] No se pudo enviar mensaje al nodo");
    state = STATE_IDLE;
    
    // Enviar error al Bridge
    const char *err_msg = "ERROR: nodo no disponible";
    sendEncryptedToBridge(node_id, (uint8_t*)err_msg, strlen(err_msg));
  }
}

// ============================================================
// Verificar y descifrar mensaje del Bridge
// ============================================================
bool verifyAndDecrypt(EspNowCryptedMessage *msg, uint8_t *plaintext, uint8_t *plain_len) {
  if (!keys_initialized || !peer_key_known) {
    DEBUG_PRINTLN("[ERROR] No hay claves para descifrar");
    return false;
  }
  
  uint8_t data_to_verify[4 + CRYPTED_PAYLOAD_MAX + 2];
  uint16_t idx = 0;
  data_to_verify[idx++] = msg->msg_type;
  data_to_verify[idx++] = msg->node_id;
  memcpy(data_to_verify + idx, &msg->counter, 4); idx += 4;
  memcpy(data_to_verify + idx, msg->payload, msg->payload_len); idx += msg->payload_len;
  data_to_verify[idx++] = msg->payload_len;
  
  if (!verifySignature(data_to_verify, idx, msg->signature, peer_public_key)) {
    DEBUG_PRINTLN("[SEGURIDAD] Firma ECDSA INVALIDA - mensaje rechazado");
    total_errors++;
    return false;
  }
  DEBUG_PRINTLN("[SEGURIDAD] Firma ECDSA VALIDA - Bridge autenticado");
  
  // Ahora descifrar (secreto determinista)
  uint8_t shared_secret[ECC_PRIV_KEY_SIZE];
  for (int i = 0; i < ECC_PRIV_KEY_SIZE; i++) {
    shared_secret[i] = my_private_key[i] ^ peer_public_key[i];
  }
  
  decryptXor(plaintext, msg->payload, msg->payload_len, shared_secret, msg->counter);
  *plain_len = msg->payload_len;
  
  DEBUG_PRINT("[OK] Mensaje descifrado: ");
  for (uint8_t i = 0; i < *plain_len; i++) {
    DEBUG_PRINT((char)plaintext[i]);
  }
  DEBUG_PRINTLN();
  
  total_commands_decrypted++;
  return true;
}

// ============================================================
// Procesar respuesta del nodo (cabecera comun extendida 21 B)
// ============================================================
void processNodoResponse() {
  if (!g_nodo_response_received) return;
  g_nodo_response_received = false;
  
  EspNowNodeMessageV13 resp;
  memcpy(&resp, (const void*)g_nodo_buffer, sizeof(EspNowNodeMessageV13));
  
  uint8_t node_id = resp.node_id;
  uint16_t adc_raw = (uint16_t)(resp.payload[PAYLOAD_IO_ADC_RAW] |
                                (resp.payload[PAYLOAD_IO_ADC_RAW+1] << 8));
  
  DEBUG_PRINTF("[RX] Respuesta del nodo %d: c=%u u=%u ms, func=%d fw=%d rssi=%d i=%d a=%d o=%d\n",
               node_id, resp.counter, resp.uptime_ms,
               resp.node_function, resp.fw_version, resp.rssi_db,
               resp.payload[PAYLOAD_IO_GPIO_IN], adc_raw,
               resp.payload[PAYLOAD_IO_GPIO_OUT]);
  
  // Cifrar la respuesta y enviarla al Bridge en texto corto con estado I/O
  // Formato: "N4 c=9 u=100ms i=0 a=512 o=1" (max 32 B, recorte defensivo)
  char plaintext[CRYPTED_PAYLOAD_MAX + 1];
  int plain_len = snprintf(plaintext, sizeof(plaintext),
                           "N%d c=%u u=%ums i=%u a=%u o=%u",
                           node_id, resp.counter, resp.uptime_ms,
                           resp.payload[PAYLOAD_IO_GPIO_IN], adc_raw,
                           resp.payload[PAYLOAD_IO_GPIO_OUT]);
  if (plain_len > CRYPTED_PAYLOAD_MAX) plain_len = CRYPTED_PAYLOAD_MAX;
  
  state = STATE_CRYPT_AND_SEND;
  unsigned long t_send = millis();
  if (sendEncryptedToBridge(node_id, (uint8_t*)plaintext, plain_len)) {
    DEBUG_PRINTF("[OK] Respuesta del nodo %d cifrada y enviada al Bridge (%lu ms)\n",
                 node_id, millis() - t_send);
  } else {
    DEBUG_PRINTF("[ERROR] No se pudo enviar respuesta cifrada al Bridge\n");
  }
  
  state = STATE_IDLE;
}

// ============================================================
// Verificar timeout en espera de nodo
// ============================================================
void checkTimeouts() {
  unsigned long now = millis();
  
  if (state == STATE_ESP_RESP_NODO) {
    if (now - state_enter_time > NODE_TIMEOUT_MS) {
      DEBUG_PRINTLN("[TIMEOUT] No se recibio respuesta del nodo");
      total_timeouts++;
      
      // Informar al Bridge
      const char *err_msg = "TIMEOUT: nodo sin respuesta";
      if (bridge_mac_known) {
        sendEncryptedToBridge(current_crypted_msg.node_id, 
                              (uint8_t*)err_msg, strlen(err_msg));
      }
      
      state = STATE_IDLE;
    }
  }
}

// ============================================================
// Watchdog de red de la Puerta de Enlace
// ============================================================
void checkNetworkWatchdog() {
  unsigned long now = millis();
  if (now - last_bridge_ms > PUERTA_WDT_TIMEOUT_MS) {
    DEBUG_PRINTLN("[WDT-RED] Sin mensajes del Bridge, reiniciando...");
    delay(100);
    ESP.restart();
  }
}

// ============================================================
// Setup
// ============================================================
void setup() {
#ifdef ENABLE_SERIAL_DEBUG
  Serial.begin(SERIAL_BAUD);
  Serial.println();
  Serial.println("=============================================");
  Serial.println("  ESP8266 - PUERTA DE ENLACE (Cifrado ECC)");
  Serial.println("  netEspNow v1.3");
  Serial.println("=============================================");
  Serial.println();
#endif

  // Inicializar uECC RNG siempre (necesario para ECDSA)
  uECC_set_rng(&ecc_rng);
  
  // Inicializar EEPROM
  initEEPROM();
  
  // Cargar claves desde EEPROM o grabarlas desde las hardcodeadas
  if (hasKeysInEEPROM()) {
    loadKeysFromEEPROM();
    DEBUG_PRINTLN("[EEPROM] Claves ECC cargadas de EEPROM");
    
    // Cargar clave publica del Bridge
    if (loadPeerKeyFromEEPROM()) {
      DEBUG_PRINTLN("[EEPROM] Clave publica del Bridge cargada");
    } else {
      // Si no esta en EEPROM, copiar de hardcodeadas
      DEBUG_PRINTLN("[EEPROM] Copiando clave publica hardcodeada del Bridge...");
      memcpy(peer_public_key, HARDCODED_PEER_PUB_KEY, ECC_PUB_KEY_SIZE);
      memcpy(bridge_mac, HARDCODED_PEER_MAC, 6);
      peer_key_known = true;
      bridge_mac_known = true;
      savePeerKeyToEEPROM();
    }
  } else {
    DEBUG_PRINTLN("[EEPROM] Primera ejecucion. Grabando claves hardcodeadas...");
    loadHardcodedKeys();
    saveKeysToEEPROM();
    savePeerKeyToEEPROM();
    DEBUG_PRINTLN("[OK] Claves ECC hardcodeadas guardadas en EEPROM");
  }
  
  keys_initialized = true;
  
  // Inicializar WiFi en modo Station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  // NOTA: NO usar delay() aqui. Bug del core ESP8266 3.1.2.
  
  // Fijar canal WiFi 1 explícitamente.
  wifi_set_channel(ESP_NOW_CHANNEL);
  
  DEBUG_PRINT("MAC de esta Puerta de Enlace: ");
  DEBUG_PRINTLN(WiFi.macAddress());
  DEBUG_PRINT("Canal WiFi: ");
  DEBUG_PRINTLN(WiFi.channel());
  
  // Inicializar ESP-NOW
  int ret = esp_now_init();
  if (ret != 0) {
    DEBUG_PRINTF("[FATAL] Error esp_now_init(): %d\n", ret);
    while (1) { delay(1000); }
  }
  
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onDataReceived);
  esp_now_register_send_cb(onDataSent);
  
  // Agregar peer broadcast (para hablar con nodos)
  ret = esp_now_add_peer((uint8_t*)ESP_NOW_BROADCAST_MAC, ESP_NOW_ROLE_COMBO, ESP_NOW_CHANNEL, NULL, 0);
  if (ret != 0) {
    DEBUG_PRINTF("[WARN] Peer broadcast no agregado (codigo=%d)\n", ret);
  }
  
  // Agregar Bridge como peer (para poder enviarle respuestas)
  if (bridge_mac_known) {
    ret = esp_now_add_peer((uint8_t*)bridge_mac, ESP_NOW_ROLE_COMBO, ESP_NOW_CHANNEL, NULL, 0);
    if (ret == 0) {
      DEBUG_PRINTLN("[OK] Bridge agregado como peer ESP-NOW");
    } else {
      DEBUG_PRINTF("[WARN] Peer Bridge no agregado (codigo=%d)\n", ret);
    }
  }
  
  DEBUG_PRINTLN("[OK] ESP-NOW inicializado");
  DEBUG_PRINTLN("[OK] Puerta de Enlace lista, esperando comandos cifrados...");
  DEBUG_PRINTLN();
  
  last_heartbeat = millis();
  last_bridge_ms = millis();
}

// ============================================================
// Loop principal
// ============================================================
void loop() {
  unsigned long now = millis();
  
  // Heartbeat interno
  if (now - last_heartbeat >= HEARTBEAT_MS) {
    last_heartbeat = now;
    heartbeat_count++;
    
    // Cada 30 heartbeats (60s), mostrar estado
    if (heartbeat_count % 30 == 0) {
      DEBUG_PRINTF("[STATS] Cmd descifrados=%u | Resp cifradas=%u | Timeouts=%u | Errores=%u\n",
                   total_commands_decrypted, total_responses_encrypted,
                   total_timeouts, total_errors);
    }
  }
  
  // 1. Verificar si llego un mensaje cifrado del Bridge
  if (g_crypt_msg_received) {
    g_crypt_msg_received = false;
    last_bridge_ms = millis();   // Watchdog de red: hay actividad del Bridge
    
    // Copiar del buffer ISR
    EspNowCryptedMessage crypt_msg;
    uint8_t sender_mac[6];
    memcpy(sender_mac, (const void*)g_crypt_sender_mac, 6);
    memcpy(&crypt_msg, (const void*)g_crypt_buffer, sizeof(EspNowCryptedMessage));
    
    // Aprender MAC del Bridge si no la conocemos
    if (!bridge_mac_known) {
      memcpy(bridge_mac, sender_mac, 6);
      bridge_mac_known = true;
      
      // Agregar peer Bridge
      if (!esp_now_is_peer_exist(bridge_mac)) {
        esp_now_add_peer(bridge_mac, ESP_NOW_ROLE_COMBO, ESP_NOW_CHANNEL, NULL, 0);
      }
      DEBUG_PRINTF("[APRENDIZAJE] MAC del Bridge: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   bridge_mac[0], bridge_mac[1], bridge_mac[2],
                   bridge_mac[3], bridge_mac[4], bridge_mac[5]);
    }
    
    // Verificar que tengamos la clave publica del Bridge
    if (!peer_key_known) {
      if (!loadPeerKeyFromEEPROM()) {
        DEBUG_PRINTLN("[WARN] No hay clave publica del Bridge en EEPROM");
        DEBUG_PRINTLN("[WARN] Esperando intercambio de claves...");
        return;
      }
    }
    
    // Verificar firma y descifrar
    uint8_t plaintext[CRYPTED_PAYLOAD_MAX];
    uint8_t plain_len = 0;
    
    if (verifyAndDecrypt(&crypt_msg, plaintext, &plain_len)) {
      // Clonar el mensaje actual para referencia
      memcpy(&current_crypted_msg, &crypt_msg, sizeof(EspNowCryptedMessage));
      
      processDecryptedCommand(plaintext, plain_len, crypt_msg.node_id);
    } else {
      DEBUG_PRINTLN("[SEGURIDAD] Mensaje cifrado RECHAZADO - firma invalida");
    }
  }
  
  // 2. Procesar respuesta de nodo (si estamos esperando una)
  if (state == STATE_ESP_RESP_NODO) {
    processNodoResponse();
  }
  
  // 3. Verificar timeouts
  checkTimeouts();
  
  // 4. Watchdog de red
  checkNetworkWatchdog();
  
  delay(1);
}
