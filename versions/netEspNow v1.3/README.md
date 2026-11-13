# netEspNow v1.3 — Red ESP-NOW Cifrada con Puerta de Enlace ECC y Plantilla Base I/O

![Estado](https://img.shields.io/badge/estado-operativo-brightgreen)
![Versión](https://img.shields.io/badge/versión-1.3-blue)
![Placa](https://img.shields.io/badge/placa-ESP8266%20NodeMCU%20v3-orange)

> **Red de sensores inalámbrica ESP-NOW con cifrado asimétrico ECC.**  
> El Bridge habla con una **Puerta de Enlace** que cifra/descifra con curvas elípticas (secp256r1). En v1.3 todos los nodos usan la **plantilla base I/O** (entrada digital + ADC + salida digital accionable remotamente) con cabecera común extendida de 21 bytes.

---

## 📋 Tabla de Contenidos

- [Novedades de v1.3](#novedades-de-v13)
- [Arquitectura](#arquitectura)
- [Componentes de Hardware](#componentes-de-hardware)
- [Pinout](#pinout)
- [Diagrama de Flujo](#diagrama-de-flujo)
- [Protocolo ESP-NOW v1.3](#protocolo-esp-now-v13)
- [Cifrado ECC](#cifrado-ecc)
- [Comandos del Bridge](#comandos-del-bridge)
- [Instalación Rápida](#instalación-rápida)
- [Puesta en Marcha y Validación](#puesta-en-marcha-y-validación)
- [Advertencias de Seguridad](#advertencias-de-seguridad)

---

## 🆕 Novedades de v1.3

- ✅ **Cabecera común extendida de 21 B** (`EspNowNodeMessageV13`) compartida por todos los dispositivos con `static_assert(sizeof == 21)`.
- ✅ **Plantilla base I/O en los 5 nodos** (`NODE_FUNC_IO = 11`): entrada digital con pull-up (D5), ADC (A0) y salida digital de actuación (D1).
- ✅ **Comando remoto `set <id> on|off`**: Bridge → Puerta → Nodo → GPIO de salida → respuesta de estado.
- ✅ **Tabla de nodos en el Bridge** con función (`FUNC`) y versión de firmware (`FW`) aprendidas dinámicamente.
- ✅ **Watchdog de red en TODA la cadena**: Bridge (10 timeouts consecutivos), Puerta (120 s), Nodos (120 s).
- ✅ **`stats` en el Bridge** muestra estado I/O por nodo (`has_io`).
- ✅ Bridge y Puerta flasheados y validados por serial (v1.3).

---

## 🏗 Arquitectura

```
                    ┌──────────────────────────────────────────────────┐
                    │                     PUERTA DE ENLACE              │
                    │              (ESP8266 cerámica)                    │
                    │                                                   │
  PC ──serial──┐   │  ┌──────────┐    ┌──────────┐    ┌──────────┐    │
               │   │  │ Descifra │    │ Envia a  │    │ Cifra    │    │
  Bridge ──────┘   │  │   ECC    │───>│  Nodo    │───>│  Resp.   │───>┐
  (NodeMCU) ──ESP-NOW CIFRADO ──>│  └──────────┘    └──────────┘    │   │
               │   │       ▲                                  │      │   │
               │   │       │           ESP-NOW PLANO (21 B)   │      │   │
               │   │       └──────────────────────────────────┘      │   │
               │   └──────────────────────────────────────────────────┘   │
               │                                                         │
               │            ┌─── Nodo 0 ───┐   ┌─── Nodo 3 ───┐          │
               │            │ ESP8266 I/O  │   │ ESP8266 I/O  │          │
               └─── serial ─┼─── Nodo 1 ───┤   ├─── Nodo 4 ───┤          │
                   (resultados) │ ESP8266 I/O  │   │ ESP8266 I/O  │          │
                                ├─── Nodo 2 ───┤   └──────────────┘          │
                                │ ESP8266 I/O  │                             │
                                └──────────────┘                             │
```

**Evolución del protocolo:**
- v1.1: Bridge ↔ Nodos (plano)
- v1.2: Bridge ↔ Puerta **CIFRADO** (ECC), Puerta ↔ Nodos plano
- v1.3: Cabecera común extendida **21 B** + plantilla I/O + comando remoto + watchdog de red

---

## 🔧 Componentes de Hardware

| ID | Dispositivo | MAC | Rol | Estado |
|----|------------|-----|-----|--------|
| Bridge | NodeMCU v3 | `48:3F:DA:05:86:36` | Recibe comandos serial, cifra y envía a Puerta | ✅ v1.3 flasheado (ttyUSB0) |
| Puerta | ESP8266 cerámica | `48:55:19:14:A8:39` | Intermediario cifrado ECC | ✅ v1.3 flasheado (ttyUSB1) |
| Nodo 0 | NodeMCU v3 | `84:F3:EB:7C:99:1E` | Plantilla base I/O | ✅ Compilado — pendiente flashear |
| Nodo 1 | NodeMCU v3 | `80:7D:3A:2B:E3:59` | Plantilla base I/O | ✅ Compilado — pendiente flashear |
| Nodo 2 | NodeMCU v3 | `8C:AA:B5:0F:39:7F` | Plantilla base I/O | ✅ Compilado — pendiente flashear |
| Nodo 3 | NodeMCU v3 | `50:02:91:DE:0D:7B` | Plantilla base I/O | ✅ Compilado — pendiente flashear |
| Nodo 4 | NodeMCU v3 | `48:3F:DA:AA:8E:6D` | Plantilla base I/O | ✅ Compilado — pendiente flashear |

---

## 📌 Pinout

### Plantilla base I/O (Nodos 0-4) — v1.3

| GPIO | Etiqueta | Modo | Componente sugerido | Nota |
|------|----------|------|---------------------|------|
| GPIO14 | D5 | INPUT_PULLUP | Botón / sensor digital (NO) | Estado leído en `i=` |
| A0 | A0 (ADC) | INPUT | Potenciómetro / LDR / divisor | Valor 0-1023 en `a=` |
| GPIO5 | D1 | OUTPUT | LED / relé / actuador on-off | Accionado con `set <id> on\|off`; estado en `o=` |
| GPIO2 | D4 | OUTPUT | LED_BUILTIN | Heartbeat (activo LOW) |

**Ejemplo de conexión típica del nodo:**

```
NodeMCU v3 (Nodo 0)
┌───────────────────────┐
│ GPIO14 (D5) ●──[btn]──┤ GND        ← entrada digital con pull-up interno
│ A0        ●──[pot]───┤ 3V3        ← ADC 0-1023
│ GPIO5 (D1) ●──[LED]───┤ GND        ← salida (con resistencia ~220 Ω)
│ GPIO2 (D4) ●──[LED]───┤ GND        ← heartbeat del firmware
│ USB                    │
└───────────────────────┘
```

### Bridge (NodeMCU v3)

| GPIO | Etiqueta | Función |
|------|----------|---------|
| GPIO1 | D9 (TX) | Serial TX a PC (115200 baud) |
| GPIO3 | D10 (RX) | Serial RX desde PC |
| GPIO2 | D4 (LED_BUILTIN) | LED indicador (activo LOW) |

### Puerta de Enlace (ESP8266 cerámica)

- **Sin GPIO accesibles** — encapsulado cerámico sin pines
- **USB**: Solo alimentación (sin serial de debug por defecto)
- **Comunicación**: Exclusivamente ESP-NOW

---

## 🔄 Diagrama de Flujo

### Ping a un nodo

```
Bridge (PC)                  Puerta de Enlace               Nodo X
    │                              │                          │
    │ 1. "ping 1" por serial       │                          │
    │ 2. Cifra con ECDH+XOR       │                          │
    │ 3. Firma con ECDSA          │                          │
    │──[msg_type=2 CIFRADO]──────>│                          │
    │                              │ 4. Verifica firma       │
    │                              │ 5. Descifra comando     │
    │                              │ 6. Envia request        │
    │                              │──[msg_type=0, 21 B]────>│
    │                              │                          │ 7. Arma respuesta 21 B
    │                              │<──[msg_type=1, 21 B]────│    (i=, a=, o=)
    │                              │ 8. Cifra respuesta       │
    │                              │ 9. Firma respuesta      │
    │<──[msg_type=3 CIFRADO]──────│                          │
    │ 10. Verifica firma           │                          │
    │ 11. Descifra y muestra       │                          │
    │     "[FUNC IO] in=x adc=x out=x"                        │
```

### Comando remoto (actuación)

```
Bridge (PC)                  Puerta de Enlace               Nodo X
    │                              │                          │
    │ 1. "set 1 on" por serial     │                          │
    │ 2. Cifra + firma             │                          │
    │──[msg_type=2 CIFRADO]──────>│                          │
    │                              │ 4. Verifica firma        │
    │                              │ 5. Descifra comando      │
    │                              │ 6. Envia COMANDO         │
    │                              │──[msg_type=2, 21 B]─────>│
    │                              │     payload[0]=0x01      │
    │                              │     payload[1]=1 (ON)    │
    │                              │                          │ 7. digitalWrite(D1, HIGH)
    │                              │<──[msg_type=1, 21 B]────│ 8. Responde estado nuevo
    │                              │ 9. Cifra + firma         │
    │<──[msg_type=3 CIFRADO]──────│                          │
    │ 10. Verifica + descifra      │                          │
    │     "[FUNC IO] in=0 adc=512 out=1"                      │
```

---

## 📡 Protocolo ESP-NOW v1.3

### Cabecera común extendida (Nodos ↔ Puerta) — 21 bytes

```c
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;        // MSG_TYPE_REQUEST=0, RESPONSE=1, COMMAND=2
  uint8_t  node_id;         // 0-4
  uint8_t  node_function;   // NODE_FUNC_IO=11 en v1.3
  uint8_t  fw_version;      // 13
  int8_t   rssi_db;         // RESERVADO = 0 (medición descartada v1.3)
  uint32_t counter;         // contador correlativo de respuestas
  uint32_t uptime_ms;       // tiempo activo del nodo
  uint8_t  payload[8];      // datos específicos de la función
} EspNowNodeMessageV13;     // 21 bytes
```

### Payload I/O (`NODE_FUNC_IO`)

| Índice | Campo | Tipo | Descripción |
|--------|-------|------|-------------|
| 0 | `PAYLOAD_IO_GPIO_IN` | uint8 | Estado entrada digital (0/1) |
| 1 | `PAYLOAD_IO_ADC_RAW` | uint16 | Valor ADC 0-1023 (little endian) |
| 3 | `PAYLOAD_IO_GPIO_OUT` | uint8 | Estado salida digital (0/1) |
| 4-7 | `PAYLOAD_IO_RESERVED` | 4 B | Reservado para futuras funciones |

### Comandos de actuación

| CMD | Valor | Descripción |
|-----|-------|-------------|
| `CMD_SET_STATE` (0x01) | payload[1] = 0/1 | Escribe `PIN_GPIO_OUT` (on/off) |
| `CMD_SET_PWM` (0x02) | — | Futuro (función PWM) |
| `CMD_GET_STATE` (0x03) | — | Fuerza respuesta inmediata |

### Mensajes Cifrados (Bridge ↔ Puerta) — 103 bytes

```c
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;       // 2=bridge->puerta, 3=puerta->bridge
  uint8_t  node_id;        // ID del nodo destino (0-4)
  uint32_t counter;        // Contador correlativo (anti-replay)
  uint8_t  payload[32];    // Datos cifrados (XOR con keystream ECDH)
  uint8_t  payload_len;    // Longitud real del payload
  uint8_t  signature[64];  // Firma ECDSA (secp256r1)
} EspNowCryptedMessage;
```

---

## 🔐 Cifrado ECC

### Algoritmo
- **Librería**: `micro-ecc` (uECC) v1.0.0
- **Curva**: secp256r1 (256 bits)
- **Clave privada**: 32 bytes
- **Clave pública**: 64 bytes (sin comprimir)

### Protecciones
| Amenaza | Protección | Implementación |
|---------|-----------|----------------|
| Suplantación | Firma ECDSA | Bridge firma, Puerta verifica |
| Escucha | Cifrado ECDH+XOR | Solo ambos extremos derivan clave |
| Replay | Contador de 32 bits | Se incrementa por mensaje |
| Clonación | EEPROM con magic | Claves persisten tras reinicio |

### Layout EEPROM (512 bytes)
| Offset | Tamaño | Contenido |
|--------|--------|-----------|
| 0 | 4 | Magic "NS13" |
| 4 | 32 | Clave privada propia |
| 36 | 64 | Clave pública propia |
| 100 | 64 | Clave pública del otro |
| 164 | 6 | MAC del otro dispositivo |

---

## ⌨️ Comandos del Bridge

```
COMANDO              DESCRIPCIÓN
───────────────────────────────────────────────────────────
ping <id>            Interrogar nodo (0-4) via Puerta de Enlace
ping all             Interrogar todos los nodos
set <id> on|off      Accionar GPIO de salida del nodo (v1.3)
list                 Mostrar tabla de nodos (función + FW)
stats                Mostrar estadísticas y estado I/O (v1.3)
auto on              Activar interrogación automática
auto off             Desactivar interrogación automática
keygen               Restaurar claves ECC de fábrica en EEPROM
learnkey             Cargar clave pública de Puerta desde EEPROM
showkey              Mostrar clave pública de este Bridge
help / ?             Mostrar ayuda
```

**Puerto serie**: `/dev/ttyUSB0` — **Baudrate**: 115200

### Ejemplos de uso

```bash
# Interrogar al nodo 1
ping 1
# → [CMD] Preguntando a Nodo 1 via Puerta de Enlace...
# → [SEGURIDAD] Firma ECDSA VALIDA - Puerta autenticada
# → RESPUESTA DESCIFRADA - Nodo #1 [FUNC IO] in=0 adc=512 out=0

# Encender la salida digital del nodo 1
set 1 on
# → [CMD] Actuando GPIO salida del Nodo 1 -> ON
# → [FUNC IO] in=0 adc=512 out=1

# Apagar la salida digital del nodo 3
set 3 off

# Ver tabla de nodos con función y versión
list

# Ver estadísticas y estado I/O
stats
```

---

## ⚡ Instalación Rápida

### Requisitos
```bash
arduino-cli core install esp8266:esp8266
arduino-cli lib install "micro-ecc"
```

### Compilar y subir Puerta de Enlace
```bash
# Bridge y Puerta requieren el flag bearssl para compilar (uECC usa bearssl.h)
BEARSSL="$HOME/.arduino15/packages/esp8266/hardware/esp8266/3.1.2/tools/sdk/include/bearssl"

arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
  --build-property "build.extra_flags=-I${BEARSSL}" \
  firmware_puerta_enlace/

# Upload SIN el flag
arduino-cli upload --fqbn esp8266:esp8266:nodemcuv2 \
  --port /dev/ttyUSB1 firmware_puerta_enlace/
```

### Compilar Bridge
```bash
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
  --build-property "build.extra_flags=-I${BEARSSL}" \
  firmware_bridge/

arduino-cli upload --fqbn esp8266:esp8266:nodemcuv2 \
  --port /dev/ttyUSB0 firmware_bridge/
```

### Compilar nodos (plantilla base I/O)
```bash
for id in 0 1 2 3 4; do
  arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
    firmware_nodo_${id}/
done

# Upload cuando el nodo esté conectado
arduino-cli upload --fqbn esp8266:esp8266:nodemcuv2 \
  --port /dev/ttyUSB2 firmware_nodo_0/
```

---

## 🚀 Puesta en Marcha y Validación

1. Conectar **Puerta de Enlace** a USB (alimentación).
2. Conectar **Bridge** a PC via USB (`/dev/ttyUSB0`).
3. Abrir monitor serial (115200 baud).
4. Ejecutar `keygen` para restaurar claves ECC de fábrica (opcional, primera vez).
5. Conectar nodos 0-4 y flashear la plantilla base I/O.
6. Probar con `ping 0` → debe mostrar `[FUNC IO] in=.. adc=.. out=..`.
7. Probar actuación con `set 0 on` → medir con multímetro 3.3 V en D1.
8. Verificar `list` (función/FW aprendidos) y `stats`.

> **Nota**: Si los nodos no están conectados físicamente, `ping` mostrará `TIMEOUT: nodo sin respuesta` — comportamiento esperado. El watchdog del Bridge reiniciará tras 10 timeouts consecutivos.

---

## ⚠️ Advertencias de Seguridad

1. **Este es un sistema para IoT con recursos limitados.** El cifrado XOR con keystream derivado de ECDH es adecuado para sensores de bajo riesgo, pero no para aplicaciones que requieran seguridad criptográfica fuerte.
2. **Las claves privadas se almacenan en EEPROM** (flash interna). Un atacante con acceso físico al dispositivo podría extraerlas.
3. **La Puerta de Enlace no tiene serial de debug** — no se puede monitorear su estado directamente.
4. **Red de prueba** — No usar en entornos de producción sin una auditoría de seguridad completa.

---

## 📁 Estructura del Proyecto

```
netEspNow v1.2/                    ← (proyecto netEspNow v1.3)
├── README.md                    ← Este archivo
├── BITACORA.md                  ← Historial del proyecto
├── PLAN.md                      ← Plan de implementación
├── AUDIT_REPORT.md              ← Informe de auditoría
├── .gitignore                   ← Archivos ignorados
├── docs/                        ← Documentación
│   ├── comandos.md
│   ├── datasheet.md             ← Documentación de usuario (v1.3)
│   ├── developer.md             ← Explicación del código (v1.3)
│   ├── hardware.md
│   ├── instalacion.md
│   ├── protocolo.md
│   └── red.md
├── firmware_comun/              ← Cabecera común (NUEVO v1.3)
│   └── netespnow_comun.h
├── firmware_bridge/             ← Bridge v1.3 (cifrado ECC)
│   ├── firmware_bridge.ino
│   └── netespnow_comun.h
├── firmware_puerta_enlace/      ← Puerta de Enlace v1.3
│   ├── firmware_puerta_enlace.ino
│   └── netespnow_comun.h
├── firmware_nodo_0/             ← Plantilla base I/O (NODE_ID=0)
│   ├── firmware_nodo_0.ino
│   └── netespnow_comun.h
├── firmware_nodo_1/             ← Nodo 1 (NODE_ID=1)
├── firmware_nodo_2/             ← Nodo 2 (NODE_ID=2)
├── firmware_nodo_3/             ← Nodo 3 (NODE_ID=3)
└── firmware_nodo_4/             ← Nodo 4 (NODE_ID=4)
```

---

## 📜 Licencia

Proyecto privado — Cuenta wanderc137

---

*netEspNow v1.3 — Julio 2026*
