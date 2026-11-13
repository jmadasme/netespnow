# Bitácora del Proyecto — netEspNow

> Léelo al inicio de cada sesión para retomar sin re-explicar.

---

## 1. ¿Dónde está cada cosa?

| Recurso | Ruta / URL |
|---------|-----------|
| **Código v1.2** | `proyectos/netEspNow v1.2/` — https://github.com/wanderc137/netEspNow-v1.2 |
| **Código v1.3** | `proyectos/netEspNow v1.3/` — https://github.com/wanderc137/netEspNow-v1.3 |
| **Dashboard** | http://metagentes:5050 — `PSTATUS='python3 $HOME/Documents/Proyects/OpenCode/AG.CreadorContenido/dashboard/pipeline_status.py'` |

---

## 2. ¿Qué hace cada versión?

| Funcionalidad | v1.1 | v1.2 | v1.3 |
|---------------|------|------|------|
| Bridge ↔ Nodos (ESP-NOW plano) | ✅ | — | — |
| Bridge ↔ Puerta ECC (cifrado asimétrico) | — | ✅ | ✅ |
| Puerta ↔ Nodos (ESP-NOW plano) | — | ✅ | ✅ |
| Cabecera 21 B extendida | — | — | ✅ |
| Nodos con entrada digital + ADC + salida | — | — | ✅ |
| Comando remoto `set <id> on\|off` | — | — | ✅ |
| Watchdog de red (toda la cadena) | — | — | ✅ |
| FUNC/FW dinámicos en tabla Bridge | — | — | ✅ |
| ECDSA (firma) | — | ✅ | ✅ |
| EEPROM magic "NS12" → "NS13" | — | — | ✅ |

---

## 3. Hardware desplegado (v1.3)

| ID | Dispositivo | MAC | Puerto USB | Estado |
|----|------------|-----|-----------|--------|
| Bridge | NodeMCU v3 | `48:3F:DA:05:86:36` | `/dev/ttyUSB0` | ✅ Flasheado v1.3 |
| Puerta | ESP8266 cerámica | `C4:5B:BE:74:DB:4A` | `/dev/ttyUSB1` | ✅ Flasheado v1.3 |
| Nodo 0 | NodeMCU v3 | `84:F3:EB:7C:99:1E` | `/dev/ttyUSB1` (al flashear) | ✅ Flasheado v1.3 |
| Nodo 1 | NodeMCU v3 | `80:7D:3A:2B:E3:59` | `/dev/ttyUSB2` (al flashear) | ✅ Flasheado v1.3 |
| Nodo 2 | NodeMCU v3 | `8C:AA:B5:0F:39:7F` | `/dev/ttyUSB3` (al flashear) | ✅ Flasheado v1.3 |
| Nodo 3 | NodeMCU v3 | `50:02:91:DE:0D:7B` | `/dev/ttyUSB4` (al flashear) | ✅ Flasheado v1.3 |
| Nodo 4 | NodeMCU v3 | `48:3F:DA:AA:8E:6D` | `/dev/ttyUSB5` (al flashear) | ✅ Flasheado v1.3 |

---

## 4. Pines — Plantilla base I/O (todos los nodos v1.3)

| Pin silkscreen | GPIO | Modo | Propósito | Campo en respuesta |
|----------------|------|------|-----------|-------------------|
| **D5** | GPIO14 | `INPUT_PULLUP` | Entrada digital (botón, contacto) | `i=` (0/1) |
| **A0** | ADC0 | `INPUT` | Entrada analógica 0–1023 | `a=` (0-1023) |
| **D1** | GPIO5 | `OUTPUT` | Salida actuador on/off | `o=` (0/1) |
| **D4** | GPIO2 | `OUTPUT` | LED heartbeat (activo LOW) | — |

Voltaje ADC: `V = a × (3.3 / 1023)`. Ej: `a=5` → 16 mV (flotando).

---

## 5. Comandos del Bridge (serial 115200 baud, `/dev/ttyUSB0`)

| Comando | Descripción | v1.2 | v1.3 |
|---------|-------------|------|------|
| `ping <id>` | Interrogar nodo (0-4) | ✅ | ✅ |
| `ping all` | Interrogar todos secuencialmente | ✅ | ✅ |
| `set <id> on\|off` | Actuar GPIO D1 del nodo | — | ✅ |
| `list` | Tabla de nodos (FUNC + FW en v1.3) | ✅ | ✅ |
| `stats` | Estadísticas + estado I/O (v1.3) | parcial | ✅ |
| `auto on\|off` | Interrogación automática | ✅ | ✅ |
| `keygen` | Restaurar claves ECC de fábrica | ✅ | ✅ |
| `showkey` | Mostrar clave pública del Bridge | ✅ | ✅ |

### Formato de respuesta I/O

```
RESPUESTA DESCIFRADA - Nodo #0
N0 c=5 u=269729ms i=1 a=5 o=0
[FUNC IO] in=1 adc=5 out=0
RTT (via Puerta): 1377 ms
```

| Campo | Significado | Rango |
|-------|-------------|-------|
| `c=` | Contador de respuestas del nodo | 0–4294967295 |
| `u=` | Uptime del nodo (ms) | 0–~49 días |
| `i=` | Entrada digital D5 | 0/1 |
| `a=` | ADC A0 | 0–1023 |
| `o=` | Salida digital D1 | 0/1 |

---

## 6. Protocolo v1.3 (resumen rápido)

### Cabecera común nodo ↔ Puerta: 21 bytes

```c
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;         // 0=REQUEST, 1=RESPONSE, 2=COMMAND
  uint8_t  node_id;          // 0-4
  uint8_t  node_function;    // NODE_FUNC_IO=11
  uint8_t  fw_version;       // 13
  int8_t   rssi_db;          // RESERVADO=0
  uint32_t counter;          // correlativo
  uint32_t uptime_ms;
  uint8_t  payload[8];       // I/O: [0]=gpio_in, [1:2]=adc(LE), [3]=gpio_out, [4:7]=reservado
} EspNowNodeMessageV13;
```

### Comandos de actuación (msg_type=2)

| CMD | payload[0] | payload[1] | Efecto |
|-----|----------|------------|--------|
| `CMD_SET_STATE` (0x01) | 0x01 | 1 (ON) / 0 (OFF) | `digitalWrite(D1, valor)` |
| `CMD_SET_PWM` (0x02) | — | — | Futuro |
| `CMD_GET_STATE` (0x03) | — | — | Fuerza respuesta inmediata |

### Mensaje cifrado Bridge ↔ Puerta: 103 bytes
`EspNowCryptedMessage` = msg_type + node_id + counter + payload[32] + payload_len + signature[64]

---

## 7. Cómo retomar el proyecto

### Para compilar
```bash
# Bridge y Puerta (incluyen bearssl/bearssl.h directamente)
arduino-cli compile --fqbn esp8266:esp8266:generic firmware_bridge/
arduino-cli compile --fqbn esp8266:esp8266:generic firmware_puerta_enlace/

# Nodos
arduino-cli compile --fqbn esp8266:esp8266:generic firmware_nodo_0/
```

### Para flashear
```bash
# Bridge (USB0) y Puerta (USB1)
arduino-cli upload --fqbn esp8266:esp8266:generic --port /dev/ttyUSB0 firmware_bridge/
arduino-cli upload --fqbn esp8266:esp8266:generic --port /dev/ttyUSB1 firmware_puerta_enlace/

# Nodos
arduino-cli upload --fqbn esp8266:esp8266:generic --port /dev/ttyUSB<N> firmware_nodo_<id>/
```

### Para probar
```bash
# Monitor serial
screen /dev/ttyUSB0 115200
# o CuteCom

# Comandos
ping 0
set 0 on
list
stats
```

### Para añadir una nueva función de nodo (ej: v1.4)
1. Editar `firmware_comun/netesponw_comun.h` (fuente de verdad)
2. Copiar a las 7 ubicaciones: `cp firmware_comun/netesponw_comun.h firmware_bridge/` etc.
3. Añadir `#define NODE_FUNC_XXX` y layout de payload
4. Modificar `firmware_nodo_0.ino` (plantilla base): leer sensor, rellenar payload
5. Extender `parseIOState()` en Bridge para parsear nuevos campos
6. Si hay comandos nuevos, extender Puerta y Bridge
7. Compilar todo, flashear, probar

---

## 8. Resultados de validación v1.3 (31 Julio 2026)

| Prueba | Nodo 0 | Nodo 1 | Nodo 2 | Nodo 3 | Nodo 4 |
|--------|--------|--------|--------|--------|--------|
| Flasheo | ✅ | ✅ | ✅ | ✅ | ✅ |
| `ping` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `set on` | ✅ `o=1` | ✅ `o=1` | ✅ `o=1` | ✅ `o=1` | ✅ `o=1` |
| `set off` | ✅ `o=0` | ✅ `o=0` | ✅ `o=0` | ✅ `o=0` | ✅ `o=0` |
| `ping all` (5 simultáneos) | ✅ | ✅ | ✅ | ✅ | ✅ |
| ECDSA verificada | ✅ | ✅ | ✅ | ✅ | ✅ |
| FUNC=IO FW=13 aprendido | ✅ | ✅ | ✅ | ✅ | ✅ |

**20/20 transacciones exitosas. RTT promedio: ~1385 ms.**
**Observación**: ping manual muestra timeout de 5s en primer intento; auto-ping (`ping all`) no tiene timeouts (mantiene caliente la conexión ESP-NOW).

---

## 9. Historial de cambios

| Fecha | Versión | Cambio |
|-------|---------|--------|
| 30 Jul 2026 | v1.1 | Red ESP-NOW plana Bridge ↔ 5 nodos |
| 30 Jul 2026 | v1.2 | Puerta de Enlace con cifrado ECC + ECDSA. Claves en EEPROM. |
| 31 Jul 2026 | v1.3 | Cabecera común 21 B. Plantilla base I/O. Comando set remoto. Watchdog de red. 5 nodos validados. |
| 10 Ago 2026 | v1.3.1 | Fix: `<bearssl.h>` → `<bearssl/bearssl.h>` (compat. core 3.1.2). MAC Puerta actualizada a `C4:5B:BE:74:DB:4A`. FQBN `generic` en vez de `nodemcuv2`. EEPROM magic "NS17". |

---

*Bitácora actualizada: 31 Julio 2026 — Fase v1.3 completada*
