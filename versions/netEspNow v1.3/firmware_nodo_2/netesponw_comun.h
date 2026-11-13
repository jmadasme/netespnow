/**
 * netespnow_comun.h - Cabecera comun netEspNow v1.3
 * ====================================================
 * Compartida por Bridge, Puerta de Enlace y Nodos 0-4.
 * Define la cabecera extendida de 21 B, funciones de nodo,
 * comandos de actuacion, watchdog de red y pines de la
 * plantilla base I/O.
 *
 * Fase v1.3: todos los nodos usan NODE_FUNC_IO (plantilla
 * base de I/O: entrada digital, ADC, salida digital).
 * rssi_db queda RESERVADO = 0 (medicion descartada en v1.3).
 */

#ifndef NETESPNOW_COMUN_H
#define NETESPNOW_COMUN_H

#include <stdint.h>

// ============================================================
// Tipos de mensaje (plano nodo <-> Puerta de Enlace)
// ============================================================
#define MSG_TYPE_REQUEST     0   // Puerta -> Nodo (peticion de estado)
#define MSG_TYPE_RESPONSE    1   // Nodo -> Puerta (respuesta con cabecera 21 B)
#define MSG_TYPE_COMMAND     2   // Puerta -> Nodo (comando de actuacion)

// ============================================================
// Canal ESP-NOW y version de firmware
// ============================================================
#define ESP_NOW_CHANNEL      1
#define NODE_FW_VERSION      13  // v1.3

// ============================================================
// Funciones de nodo (proposito)
// ============================================================
#define NODE_FUNC_UNDEFINED  0
#define NODE_FUNC_TEMP       1
#define NODE_FUNC_HUM        2
#define NODE_FUNC_O2         3
#define NODE_FUNC_PRESION    4
#define NODE_FUNC_PUERTA     5
#define NODE_FUNC_CONTADOR   6
#define NODE_FUNC_RELE       7
#define NODE_FUNC_LUZ        8
#define NODE_FUNC_PWM        9
#define NODE_FUNC_ADC        10
#define NODE_FUNC_IO         11   // Plantilla base I/O (v1.3)

// ============================================================
// Comandos de actuacion remota (msg_type = MSG_TYPE_COMMAND)
// ============================================================
#define CMD_SET_STATE        0x01 // payload[0]=0 off, 1 on
#define CMD_SET_PWM          0x02 // futuro (funcion PWM)
#define CMD_GET_STATE        0x03 // fuerza respuesta inmediata

// ============================================================
// Watchdog de red - TODA LA CADENA
// ============================================================
#define NODE_WDT_TIMEOUT_MS      120000UL // Nodo: sin request/CMD de la Puerta
#define PUERTA_WDT_TIMEOUT_MS    120000UL // Puerta: sin mensaje cifrado del Bridge
#define BRIDGE_WDT_TIMEOUTS      10       // Bridge: timeouts consecutivos sin respuesta

// ============================================================
// Plantilla base I/O (nodos 0-4)
// ============================================================
#define PIN_GPIO_IN          D5   // GPIO14, entrada digital con pull-up
#define PIN_ADC              A0   // ADC 0-1023
#define PIN_GPIO_OUT         D1   // GPIO5, salida digital actuador on/off

// Indices del payload[8] para NODE_FUNC_IO
#define PAYLOAD_IO_GPIO_IN   0    // uint8
#define PAYLOAD_IO_ADC_RAW   1    // uint16 (little endian)
#define PAYLOAD_IO_GPIO_OUT  3    // uint8
#define PAYLOAD_IO_RESERVED  4    // 4 bytes reservados

// ============================================================
// Cabecera comun extendida - 21 bytes
// ============================================================
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;        // MSG_TYPE_*
  uint8_t  node_id;         // 0-4
  uint8_t  node_function;   // NODE_FUNC_*
  uint8_t  fw_version;      // NODE_FW_VERSION (13)
  int8_t   rssi_db;         // RESERVADO = 0 (medicion descartada v1.3)
  uint32_t counter;         // contador correlativo de respuestas
  uint32_t uptime_ms;       // tiempo activo del nodo
  uint8_t  payload[8];      // datos especificos de la funcion
} EspNowNodeMessageV13;     // 21 bytes

static_assert(sizeof(EspNowNodeMessageV13) == 21,
              "cabecera comun debe ser 21B");

// ============================================================
// EEPROM de nodos (512 B)
// ============================================================
#define EEPROM_NODE_MAGIC   0x4E533133  // "NS13"

#endif // NETESPNOW_COMUN_H
