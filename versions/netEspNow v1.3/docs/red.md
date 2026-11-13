# Red — netEspNow v1.3

## Topología

```
                    ┌──────────────────┐
                    │   PC (usuario)    │
                    │  Monitor Serial   │
                    └────────┬─────────┘
                             │ USB (115200 baud)
                    ┌────────▼─────────┐
                    │    BRIDGE        │
                    │  ESP8266 NodeMCU │
                    │  48:3F:DA:05:86:36│
                    └────────┬─────────┘
                             │ ESP-NOW CIFRADO (ECC)
                             │ msg_type=2 / msg_type=3
                    ┌────────▼─────────┐
                    │ PUERTA DE ENLACE │
                    │  ESP8266 cerámica │
                    │  48:55:19:14:A8:39│
                    └──┬──┬──┬──┬──┬───┘
                       │  │  │  │  │  ESP-NOW PLANO (21 B)
                       │  │  │  │  │  msg_type=0 / 1 / 2
          ┌────────────┘  │  │  │  └──────────┐
          ▼               ▼  ▼  ▼              ▼
     ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐
     │ NODO 0  │  │ NODO 1  │  │  ...    │  │ NODO 4  │
     │ I/O     │  │ I/O     │  │         │  │ I/O     │
     │84:F3:...│  │80:7D:...│  │         │  │48:3F:...│
     └─────────┘  └─────────┘  └─────────┘  └─────────┘
```

## Tabla de Dispositivos

| ID | Dispositivo | MAC | Rol | Canal | Enlaces |
|----|------------|-----|-----|-------|---------|
| Bridge | NodeMCU v3 | `48:3F:DA:05:86:36` | Puente USB ↔ ESP-NOW (cifrado) | 1 | Puerta de Enlace |
| Puerta | ESP8266 cerámica | `48:55:19:14:A8:39` | Intermediario cifrado | 1 | Bridge + Nodos 0-4 |
| 0 | NodeMCU v3 | `84:F3:EB:7C:99:1E` | Plantilla I/O | 1 | Puerta de Enlace |
| 1 | NodeMCU v3 | `80:7D:3A:2B:E3:59` | Plantilla I/O | 1 | Puerta de Enlace |
| 2 | NodeMCU v3 | `8C:AA:B5:0F:39:7F` | Plantilla I/O | 1 | Puerta de Enlace |
| 3 | NodeMCU v3 | `50:02:91:DE:0D:7B` | Plantilla I/O | 1 | Puerta de Enlace |
| 4 | NodeMCU v3 | `48:3F:DA:AA:8E:6D` | Plantilla I/O | 1 | Puerta de Enlace |

## Características de la Red

| Parámetro | Valor |
|-----------|-------|
| Protocolo | ESP-NOW (sin WiFi) |
| Canal | 1 |
| Alcance típico | ~30-50m interiores, ~200m exteriores |
| Topología | Estrella (Puerta de Enlace como centro) |
| Máximo nodos | 5 sensores + 1 Puerta + 1 Bridge |
| Latencia típica | ~500-2500ms por interrogación |

## Flujo de Datos

1. Bridge recibe comando serial (`ping <id>`, `set <id> on|off`) → cifra con ECC → envía a Puerta
2. Puerta descifra y verifica firma → interroga/actúa sobre nodo por ESP-NOW plano (cabecera 21 B)
3. Nodo responde (estado I/O) → Puerta cifra respuesta → envía a Bridge
4. Bridge descifra y verifica firma → muestra `[FUNC IO] in=.. adc=.. out=..` por serial

## Seguridad en la Red

- **Bridge ↔ Puerta**: Cifrado ECC + Firma ECDSA + contador anti-replay
- **Puerta ↔ Nodos**: Sin cifrado (mismo que v1.1)
- **Claves**: Almacenadas en EEPROM de Bridge y Puerta

## Watchdog de Red (v1.3)

| Dispositivo | Condición | Acción |
|-------------|-----------|--------|
| Bridge | 10 timeouts consecutivos | `ESP.restart()` |
| Puerta de Enlace | 120 s sin mensaje del Bridge | `ESP.restart()` |
| Nodo | 120 s sin request/comando | `ESP.restart()` |

---

*Red netEspNow v1.3 — Julio 2026*
