# Red — netEspNow v1.2

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
                       │  │  │  │  │  ESP-NOW PLANO
                       │  │  │  │  │  msg_type=0 / msg_type=1
          ┌────────────┘  │  │  │  └──────────┐
          ▼               ▼  ▼  ▼              ▼
     ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐
     │ NODO 0  │  │ NODO 1  │  │  ...    │  │ NODO 4  │
     │84:F3:...│  │80:7D:...│  │         │  │48:3F:...│
     └─────────┘  └─────────┘  └─────────┘  └─────────┘
```

## Tabla de Dispositivos

| ID | Dispositivo | MAC | Rol | Canal | Enlaces |
|----|------------|-----|-----|-------|---------|
| Bridge | NodeMCU v3 | `48:3F:DA:05:86:36` | Puente USB ↔ ESP-NOW | 1 | Puerta de Enlace |
| Puerta | ESP8266 cerámica | `48:55:19:14:A8:39` | Intermediario cifrado | 1 | Bridge + Nodos 0-4 |
| 0 | NodeMCU v3 | `84:F3:EB:7C:99:1E` | Sensor | 1 | Puerta de Enlace |
| 1 | NodeMCU v3 | `80:7D:3A:2B:E3:59` | Sensor | 1 | Puerta de Enlace |
| 2 | NodeMCU v3 | `8C:AA:B5:0F:39:7F` | Sensor | 1 | Puerta de Enlace |
| 3 | NodeMCU v3 | `50:02:91:DE:0D:7B` | Sensor | 1 | Puerta de Enlace |
| 4 | NodeMCU v3 | `48:3F:DA:AA:8E:6D` | Sensor | 1 | Puerta de Enlace |

## Características de la Red

| Parámetro | Valor |
|-----------|-------|
| Protocolo | ESP-NOW (sin WiFi) |
| Canal | 1 |
| Alcance típico | ~30-50m interiores, ~200m exteriores |
| Topología | Estrella (Puerta de Enlace como centro) |
| Máximo nodos | 5 sensores + 1 Puerta + 1 Bridge |
| Latencia típica | ~500-1500ms por interrogación |

## Flujo de Datos

1. Bridge recibe comando serial → cifra con ECC → envía a Puerta
2. Puerta descifra → interroga nodo por ESP-NOW plano
3. Nodo responde → Puerta cifra respuesta → envía a Bridge
4. Bridge descifra → muestra resultado por serial

## Seguridad en la Red

- **Bridge ↔ Puerta**: Cifrado ECC (ECDH + XOR) + Firma ECDSA
- **Puerta ↔ Nodos**: Sin cifrado (mismo que v1.1)
- **Claves**: Almacenadas en EEPROM de Bridge y Puerta
