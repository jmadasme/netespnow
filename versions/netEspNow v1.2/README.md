# netEspNow v1.2 — Red ESP-NOW Cifrada con Puerta de Enlace ECC

![Estado](https://img.shields.io/badge/estado-operativo-brightgreen)
![Versión](https://img.shields.io/badge/versión-1.2-blue)
![Placa](https://img.shields.io/badge/placa-ESP8266%20NodeMCU%20v3-orange)

> **Red de sensores inalámbrica ESP-NOW con cifrado asimétrico ECC.**  
> El Bridge ya no habla directamente con los nodos: todo el tráfico pasa por una **Puerta de Enlace** que cifra/descifra con curvas elípticas (secp256r1).

---

## 📋 Tabla de Contenidos

- [Arquitectura](#arquitectura)
- [Componentes de Hardware](#componentes-de-hardware)
- [Diagrama de Flujo](#diagrama-de-flujo)
- [Protocolo ESP-NOW](#protocolo-esp-now)
- [Cifrado ECC](#cifrado-ecc)
- [Comandos del Bridge](#comandos-del-bridge)
- [Instalación Rápida](#instalación-rápida)
- [Advertencias de Seguridad](#advertencias-de-seguridad)

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
               │   │       │           ESP-NOW PLANO          │      │   │
               │   │       └──────────────────────────────────┘      │   │
               │   └──────────────────────────────────────────────────┘   │
               │                                                         │
               │            ┌─── Nodo 0 ───┐   ┌─── Nodo 3 ───┐          │
               │            │ ESP8266      │   │ ESP8266      │          │
               └─── serial ─┼─── Nodo 1 ───┤   ├─── Nodo 4 ───┤          │
                   (resultados) │ ESP8266      │   │ ESP8266      │          │
                               ├─── Nodo 2 ───┤   └──────────────┘          │
                               │ ESP8266      │                             │
                               └──────────────┘                             │
```

**Novedad v1.2 vs v1.1:**
- ✅ Comunicación Bridge ↔ Puerta de Enlace: **CIFRADA** (ECC asimétrico)
- ✅ Comunicación Puerta de Enlace ↔ Nodos: **PLANA** (mismo protocolo v1.1)
- ✅ Claves ECC almacenadas en EEPROM de ambos dispositivos
- ✅ Firmas ECDSA para autenticidad
- ✅ Contador anti-replay en cada mensaje

---

## 🔧 Componentes de Hardware

| ID | Dispositivo | MAC | Rol | Estado |
|----|------------|-----|-----|--------|
| Bridge | NodeMCU v3 | `48:3F:DA:05:86:36` | Recibe comandos serial, cifra y envía a Puerta | 📍 Conectado a PC |
| Puerta | ESP8266 cerámica | `48:55:19:14:A8:39` | Intermediario cifrado ECC | ✅ Firmware v1.2 cargado |
| Nodo 0 | NodeMCU v3 | `84:F3:EB:7C:99:1E` | Sensor ESP-NOW | Pendiente de actualizar |
| Nodo 1 | NodeMCU v3 | `80:7D:3A:2B:E3:59` | Sensor ESP-NOW | Pendiente de actualizar |
| Nodo 2 | NodeMCU v3 | `8C:AA:B5:0F:39:7F` | Sensor ESP-NOW | Pendiente de actualizar |
| Nodo 3 | NodeMCU v3 | `50:02:91:DE:0D:7B` | Sensor ESP-NOW | Pendiente de actualizar |
| Nodo 4 | NodeMCU v3 | `48:3F:DA:AA:8E:6D` | Sensor ESP-NOW | Pendiente de actualizar |

### Pinout Bridge (NodeMCU v3)

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

```
Bridge (PC)                  Puerta de Enlace               Nodo X
    │                              │                          │
    │ 1. "ping 1" por serial       │                          │
    │ 2. Cifra con ECDH+XOR       │                          │
    │ 3. Firma con ECDSA          │                          │
    │──[ESP-NOW msg_type=2]──────>│                          │
    │                              │ 4. Verifica firma       │
    │                              │ 5. Descifra comando     │
    │                              │ 6. Envia request        │
    │                              │──[ESP-NOW msg_type=0]──>│
    │                              │                          │ 7. Responde
    │                              │<──[ESP-NOW msg_type=1]──│
    │                              │ 8. Cifra respuesta       │
    │                              │ 9. Firma respuesta      │
    │<──[ESP-NOW msg_type=3]──────│                          │
    │ 10. Verifica firma           │                          │
    │ 11. Descifra y muestra       │                          │
    │                              │                          │
```

---

## 📡 Protocolo ESP-NOW

### Mensajes Planos (Puerta ↔ Nodos)

Compatibles con protocolo v1.1:

```c
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;    // 0=request, 1=response
  uint8_t  node_id;     // ID del nodo (0-4)
  uint32_t counter;     // Contador correlativo
  uint32_t uptime_ms;   // Tiempo activo del nodo
} EspNowMessage;
```

### Mensajes Cifrados (Bridge ↔ Puerta)

```c
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;       // 2=bridge->puerta, 3=puerta->bridge
  uint8_t  node_id;        // ID del nodo destino (0-4)
  uint32_t counter;        // Contador correlativo
  uint8_t  payload[32];    // Datos cifrados (XOR con keystream ECDH)
  uint8_t  payload_len;    // Longitud real del payload
  uint8_t  signature[64];  // Firma ECDSA
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
| 0 | 4 | Magic "NS12" |
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
list                 Mostrar tabla de nodos y estado ECC
stats                Mostrar estadísticas de comunicación
auto on              Activar interrogación automática
auto off             Desactivar interrogación automática
keygen               Generar nuevo par de claves ECC
learnkey             Cargar clave pública de Puerta desde EEPROM
showkey              Mostrar clave pública de este Bridge
help / ?             Mostrar ayuda
```

**Puerto serie**: `/dev/ttyUSB0` — **Baudrate**: 115200

---

## ⚡ Instalación Rápida

### Requisitos
```bash
arduino-cli core install esp8266:esp8266
arduino-cli lib install "micro-ecc"
```

### Compilar y subir Puerta de Enlace
```bash
# Primera vez: conectar Puerta de Enlace por USB
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
  firmware_puerta_enlace/
arduino-cli upload --fqbn esp8266:esp8266:nodemcuv2 \
  --port /dev/ttyUSB0 firmware_puerta_enlace/
```

### Compilar Bridge
```bash
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
  firmware_bridge/
```

### Compilar nodos
```bash
for id in 0 1 2 3 4; do
  arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
    firmware_nodo_${id}/
done
```

### Puesta en Marcha
1. Conectar **Puerta de Enlace** a USB (alimentación)
2. Conectar **Bridge** a PC via USB
3. Abrir monitor serial (115200 baud)
4. Ejecutar `keygen` para generar claves ECC
5. Conectar nodos 0-4 a alimentación
6. Probar con `ping 0`

---

## ⚠️ Advertencias de Seguridad

1. **Este es un sistema para IoT con recursos limitados.** El cifrado XOR con keystream derivado de ECDH es adecuado para sensores de bajo riesgo, pero no para aplicaciones que requieran seguridad criptográfica fuerte.
2. **Las claves privadas se almacenan en EEPROM** (flash interna). Un atacante con acceso físico al dispositivo podría extraerlas.
3. **La Puerta de Enlace no tiene serial de debug** — no se puede monitorear su estado directamente.
4. **Red de prueba** — No usar en entornos de producción sin una auditoría de seguridad completa.

---

## 📁 Estructura del Proyecto

```
netEspNow v1.2/
├── README.md                    ← Este archivo
├── BITACORA.md                  ← Historial del proyecto
├── PLAN.md                      ← Plan de implementación
├── AUDIT_REPORT.md              ← Informe de auditoría
├── .gitignore                   ← Archivos ignorados
├── docs/                        ← Documentación
│   ├── comandos.md
│   ├── hardware.md
│   ├── instalacion.md
│   ├── protocolo.md
│   └── red.md
├── firmware_bridge/             ← Bridge con cifrado ECC
│   └── firmware_bridge.ino
├── firmware_puerta_enlace/      ← Puerta de Enlace (NUEVO)
│   └── firmware_puerta_enlace.ino
├── firmware_nodo_0/
│   └── firmware_nodo_0.ino
├── firmware_nodo_1/
│   └── firmware_nodo_1.ino
├── firmware_nodo_2/
│   └── firmware_nodo_2.ino
├── firmware_nodo_3/
│   └── firmware_nodo_3.ino
└── firmware_nodo_4/
    └── firmware_nodo_4.ino
```

---

## 📜 Licencia

Proyecto privado — Cuenta wanderc137

---

*netEspNow v1.2 — Julio 2026*
