# netEspNow v1.1 — Red de Sensores Inalámbrica con ESP-NOW

![Status](https://img.shields.io/badge/status-operational-brightgreen)
![Protocol](https://img.shields.io/badge/protocol-ESP--NOW-blue)
![Board](https://img.shields.io/badge/board-NodeMCU%20v3-orange)

## Descripción General

**netEspNow v1.1** es una red de sensores inalámbrica basada en el protocolo **ESP-NOW** de Espressif. La red está compuesta por **6 placas NodeMCU v3 (ESP8266)**:

| Cantidad | Rol | Descripción |
|----------|-----|-------------|
| 5 | **Nodos sensores** (IDs 0–4) | Escuchan peticiones y responden con datos |
| 1 | **Bridge** (ID 5, virtual) | Interroga los nodos y muestra resultados por USB |

El bridge se conecta a un PC mediante USB y actúa como **puente de comunicaciones**: el usuario envía comandos por el monitor serial (CuteCom, Arduino IDE, etc.) y el bridge se encarga de interrogar a los nodos vía ESP-NOW, mostrando las respuestas en tiempo real.

---

## Hardware Requerido

| Componente | Cantidad | Especificación |
|---|---|---|
| NodeMCU v3 (ESP-12E, CP2102) | 6 | Microcontrolador ESP8266 con WiFi |
| Cable USB-A a Micro-USB | 6 | Para alimentación y programación |
| PC con Linux | 1 | Con `arduino-cli` instalado |
| LED (opcional) | 5 | Para indicación visual en nodos |

### Pinout NodeMCU v3

```
                    ┌─────────────┐
     ┌──RST         │             │         D0──┐
     │  ADC  ───────┤  NodeMCU v3 ├─────── D1──┤
     │              │   ESP-12E   │         D2──┤
     │  D5/A0 ──────┤             ├──────── D3──┤
     │  D6/A1 ──────┤             ├──────── D4──┘  ← LED_BUILTIN (GPIO2)
     │  D7/A2 ──────┤             │        3V3──┐
     │  D8/A3 ──────┤             │        GND──┘
     │              └─────────────┘
     └──TX ── GPIO1  (Serial a PC)
     └──RX ── GPIO3  (Serial a PC)
```

**LED_BUILTIN**: GPIO2 (D4) — activo en LOW, parpadea 1 vez por segundo como heartbeat.

---

## Mapa de Red

```
                         ┌─────────────────────┐
                         │       PC Linux       │
                         │   (CuteCom / Serial) │
                         └──────────┬──────────┘
                                    │ USB (115200 baud)
                         ┌──────────▼──────────┐
                         │   BRIDGE ESP8266    │
                         │  (48:3F:DA:05:86:36) │
                         │  Interroga nodos    │
                         └──┬──┬──┬──┬──┬──┬──┘
                            │  │  │  │  │  │
         ┌──────────────────┘  │  │  │  │  └──────────────────┐
         │      ┌──────────────┘  │  └──────────────┐         │
         ▼      ▼      ▼      ▼      ▼      ▼
     ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐
     │ N0   │ │ N1   │ │ N2   │ │ N3   │ │ N4   │ │ (B)  │
     │ ID=0 │ │ ID=1 │ │ ID=2 │ │ ID=3 │ │ ID=4 │ │Bridge│
     │84:F3:│ │80:7D:│ │8C:AA:│ │50:02:│ │48:3F:│ │(auto)│
     │EB... │ │3A... │ │B5... │ │91... │ │DA... │ │      │
     └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘

     5 Nodos sensores (IDs 0-4)             Bridge (no interroga)
     Responden a peticiones ESP-NOW         Interroga y muestra datos
```

---

## Protocolo ESP-NOW

### Estructura del Mensaje (8 bytes)

```c
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;    // 0=request, 1=response
  uint8_t  node_id;     // ID del nodo destino/origen (0-4)
  uint32_t counter;     // Contador correlativo (nodo) / 0 (request)
  uint32_t uptime_ms;   // Tiempo activo del nodo en ms
} EspNowMessage;
```

### Secuencia de Comunicación

```
BRIDGE                        NODO X
  │                              │
  │  ─── msg_type=0 ──────────►  │  Request (petición)
  │      node_id=X               │
  │      counter=0               │
  │      uptime_ms=0             │
  │                              │
  │  ◄── msg_type=1 ──────────  │  Response (respuesta)
  │      node_id=X               │
  │      counter=N (incremental) │
  │      uptime_ms=MMMM          │
  │                              │
```

### Timing

| Parámetro | Valor |
|---|---|
| Intervalo entre polls (auto) | 3000 ms (3 segundos) |
| Timeout de respuesta | 1000 ms (1 segundo) |
| RTT típico | 1–6 ms |
| Baudrate serial | 115200 |

---

## Instalación y Programación

### Requisitos

```bash
# Verificar que arduino-cli está instalado
arduino-cli version

# Verificar que el core ESP8266 está instalado
arduino-cli core list | grep esp8266
```

### Programar un Nodo Sensor

Cada nodo tiene su propia carpeta con el `NODE_ID` ya hardcodeado:

```bash
# Ejemplo: programar Nodo 2
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
  "/home/usuario/netEspNow v1.1/firmware_nodo_2/"

# Conectar la placa y subir
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp8266:esp8266:nodemcuv2 \
  "/home/usuario/netEspNow v1.1/firmware_nodo_2/"
```

**Nota**: El puerto puede ser `/dev/ttyUSB0`, `/dev/ttyUSB1`, etc. Verificar con:
```bash
arduino-cli board list
```

### Programar el Bridge

```bash
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
  "/home/usuario/netEspNow v1.1/firmware_bridge/"

arduino-cli upload -p /dev/ttyUSB0 --fqbn esp8266:esp8266:nodemcuv2 \
  "/home/usuario/netEspNow v1.1/firmware_bridge/"
```

### Compilación Rápida (con los alias del sistema)

Si tienes los alias configurados:
```bash
ardu-compile --fqbn esp8266:esp8266:nodemcuv2 "ruta/al/firmware/"
ardu-upload -p /dev/ttyUSB0 --fqbn esp8266:esp8266:nodemcuv2 "ruta/al/firmware/"
```

### Compilar un Nodo con ID Personalizado (avanzado)

Si usas el firmware genérico (con `#ifndef NODE_ID`), puedes pasar el ID como flag:

```bash
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
  --build-property build.extra_flags=-DNODE_ID=3 \
  "ruta/al/firmware_genérico/"
```

---

## Comandos del Bridge

Una vez que el bridge está conectado por USB, abre un monitor serial (CuteCom, Arduino IDE, screen, etc.) a **115200 baud** y escribe los siguientes comandos:

### ping \<id\>

Envía una petición a un nodo específico y espera su respuesta.

```
> ping 2
[CMD] Preguntando a Nodo 2...
[ENVIO] Request -> Nodo 2 [8C:AA:B5:0F:39:7F]
----------------------------------------
RESPUESTA RECIBIDA - Nodo #2
  MAC                 : 8C:AA:B5:0F:39:7F
  Contador correlativo: 42
  Tiempo activo nodo  : 1234567 ms (1234 s)
  RTT                 : 2 ms
----------------------------------------
```

### ping all

Interroga secuencialmente a TODOS los nodos conocidos (0-4), uno cada 3 segundos.

```
> ping all
[CMD] Preguntando a TODOS los nodos...
[ENVIO] Request -> Nodo 0 [...]
... (respuestas de cada nodo)
```

**Importante**: El bridge se salta automáticamente el ID 5 porque es él mismo.

### list

Muestra la tabla completa de nodos con sus MAC y estado.

```
> list
=== TABLA DE NODOS ===
  Nodo 0: CONOCIDO [84:F3:EB:7C:99:1E]
  Nodo 1: CONOCIDO [80:7D:3A:2B:E3:59]
  Nodo 2: CONOCIDO [8C:AA:B5:0F:39:7F]
  Nodo 3: CONOCIDO [50:02:91:DE:0D:7B]
  Nodo 4: CONOCIDO [48:3F:DA:AA:8E:6D]
  Nodo 5: BRIDGE [48:3F:DA:05:86:36]
========================
```

### stats

Muestra estadísticas de comunicación (solo cuando se invoca manualmente, no automático).

```
> stats
=== ESTADISTICAS ===
  Requests enviados:  42
  Respuestas OK:      40
  Timeouts:           2
  Tasa de exito:      95%
  Nodos conocidos:
    Nodo 0: CONOCIDO [...]
    ...
========================
```

### auto on / auto off

Activa o desactiva el modo de interrogación automática.

- **auto on**: El bridge interroga cíclicamente a todos los nodos (1 cada 3 segundos)
- **auto off**: El bridge NO interroga automáticamente. Solo responde a comandos manuales (`ping`, `list`, `stats`)

```
> auto off
[OK] Interrogacion automatica DESACTIVADA
(Solo responde a comandos: ping, list, stats)
```

### help

Muestra la ayuda con todos los comandos disponibles.

```
> help
COMANDOS DISPONIBLES:
  ping <id>    - Interrogar nodo especifico (0-4)
  ping all     - Interrogar todos los nodos
  list         - Mostrar tabla de nodos con MAC
  stats        - Mostrar estadisticas
  auto on      - Activar interrogacion automatica
  auto off     - Desactivar interrogacion automatica
  help         - Mostrar esta ayuda
```

---

## Tabla de MACs (Red Actual)

| ID | MAC | Estado | Rol |
|----|-----|--------|-----|
| 0 | `84:F3:EB:7C:99:1E` | ✅ CONOCIDO | Nodo sensor 0 |
| 1 | `80:7D:3A:2B:E3:59` | ✅ CONOCIDO | Nodo sensor 1 |
| 2 | `8C:AA:B5:0F:39:7F` | ✅ CONOCIDO | Nodo sensor 2 |
| 3 | `50:02:91:DE:0D:7B` | ✅ CONOCIDO | Nodo sensor 3 |
| 4 | `48:3F:DA:AA:8E:6D` | ✅ CONOCIDO | Nodo sensor 4 |
| 5 | `48:3F:DA:05:86:36` | 🟦 BRIDGE | Bridge (no se interroga) |

---

## Identificación Física de las Placas

Cada NodeMCU v3 tiene una **dirección MAC única** grabada en el chip ESP8266. Para identificar físicamente qué placa es cuál:

1. Conecta una placa a la vez al PC
2. Abre el monitor serial o usa `arduino-cli board list`
3. Lee la MAC impresa en el mensaje de inicio del firmware
4. Compara con la tabla de MACs
5. Etiqueta la placa físicamente: "Nodo 0", "Nodo 1", etc.

**Método alternativo**: Desconecta una placa de la red, ejecuta `ping all` en el bridge, y el nodo que falle (timeout) es el que desconectaste.

---

## Solución de Problemas

### No se detecta la placa
```bash
# Verificar puertos disponibles
arduino-cli board list
ls -la /dev/ttyUSB* /dev/ttyACM*
```

### Error de permisos al grabar
```bash
# Agregar usuario al grupo dialout
sudo usermod -a -G dialout $USER
newgrp dialout
```

### El nodo no responde al ping
1. Verificar que el nodo está encendido (LED parpadeando)
2. Verificar que el nodo tiene el ID correcto
3. Verificar que el bridge tiene la MAC correcta del nodo
4. Proximidad: los nodos deben estar dentro del alcance WiFi (~50m en campo abierto)

### Timeout en todos los nodos
```bash
# Verificar que el bridge tiene el peer broadcast agregado
# En el monitor serial, ver el mensaje de inicio del bridge
```

### El monitor serial muestra basura
- Verificar que el baudrate sea **115200** en el monitor serial
- Verificar que el firmware del bridge se grabó correctamente

---

## Estructura del Proyecto

```
netEspNow v1.1/
├── firmware_nodo_0/
│   └── firmware_nodo_0.ino      # Nodo sensor ID=0
├── firmware_nodo_1/
│   └── firmware_nodo_1.ino      # Nodo sensor ID=1
├── firmware_nodo_2/
│   └── firmware_nodo_2.ino      # Nodo sensor ID=2
├── firmware_nodo_3/
│   └── firmware_nodo_3.ino      # Nodo sensor ID=3
├── firmware_nodo_4/
│   └── firmware_nodo_4.ino      # Nodo sensor ID=4
├── firmware_bridge/
│   └── firmware_bridge.ino       # Bridge (interrogador USB)
├── docs/
│   ├── hardware.md               # Detalles de hardware y pinout
│   ├── protocolo.md              # Especificación del protocolo ESP-NOW
│   ├── instalacion.md            # Guía de instalación y compilación
│   ├── comandos.md               # Referencia de comandos del bridge
│   ├── red.md                    # Topología de red y MACs
│   └── solucion_problemas.md     # Guía de troubleshooting
├── .gitignore
└── README.md
```

---

## Licencia

Proyecto privado — Todos los derechos reservados.
