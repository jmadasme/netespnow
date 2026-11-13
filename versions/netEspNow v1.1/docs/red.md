# Topología de Red y Direcciones MAC

## Arquitectura de la Red

La red netEspNow v1.1 utiliza una topología **estrella** donde el bridge actúa como nodo central que interroga a los nodos sensores:

```
                    ┌─────────────────────┐
                    │  PC (Monitor Serial) │
                    │  Interfaz de usuario │
                    └──────────┬──────────┘
                               │ USB
                    ┌──────────▼──────────┐
                    │   BRIDGE (ID=5)     │
                    │   48:3F:DA:05:86:36  │
                    │   Interrogador      │
                    └──┬──┬──┬──┬──┬──────┘
                       │  │  │  │  │
          ┌────────────┘  │  │  │  └────────────┐
          │   ┌────────────┘  │  └────────────┐  │
          ▼   ▼   ▼   ▼   ▼                  ▼
       ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐
       │ N0 │ │ N1 │ │ N2 │ │ N3 │ │ N4 │
       │ID=0│ │ID=1│ │ID=2│ │ID=3│ │ID=4│
       └────┘ └────┘ └────┘ └────┘ └────┘

    5 Nodos sensores (ESP8266 NodeMCU v3)
```

## Tabla Completa de Nodos

| ID | Dirección MAC | Estado en Bridge | Rol | Último RTT |
|----|---------------|-------------------|-----|------------|
| 0 | `84:F3:EB:7C:99:1E` | ✅ CONOCIDO | Nodo sensor | 1–6 ms |
| 1 | `80:7D:3A:2B:E3:59` | ✅ CONOCIDO | Nodo sensor | 1–2 ms |
| 2 | `8C:AA:B5:0F:39:7F` | ✅ CONOCIDO | Nodo sensor | 1–6 ms |
| 3 | `50:02:91:DE:0D:7B` | ✅ CONOCIDO | Nodo sensor | 1–2 ms |
| 4 | `48:3F:DA:AA:8E:6D` | ✅ CONOCIDO | Nodo sensor | 1–4 ms |
| 5 | `48:3F:DA:05:86:36` | 🟦 BRIDGE | Bridge | N/A |

## Identificación Física de las Placas

Para saber qué placa física corresponde a cada ID, sigue estos pasos:

### Método 1: Conectar una placa a la vez

1. Desconecta todas las placas de la red (quita USB a todas menos al bridge)
2. Conecta una sola placa nodo
3. En el bridge, ejecuta `ping 0`, `ping 1`, ..., hasta que una responda
4. Cuando responda, sabrás su ID
5. **Etiqueta físicamente la placa** (con un marcador o cinta adhesiva)
6. Desconéctala y conecta la siguiente

### Método 2: Desconexión selectiva

1. Con todas las placas conectadas, ejecuta `ping all`
2. Desconecta una placa física
3. Ejecuta `ping all` de nuevo
4. El nodo que falle (timeout) es el que desconectaste
5. Etiquétalo y vuelve a conectarlo

### Método 3: Leer la MAC del firmware

1. Conecta una placa al PC (sin el bridge)
2. Abre monitor serial a 115200 baud
3. Presiona RESET en la placa
4. El mensaje de inicio muestra la MAC:
```
======================================
ESP8266 Nodo ESP-NOW - ID: 0
======================================
MAC: 84:F3:EB:7C:99:1E
```

## Mapa Físico de las Placas (Recomendado)

| Nodo | Ubicación Física | MAC | Notas |
|------|-------------------|-----|-------|
| 0 | [Espacio para etiqueta] | 84:F3:EB:7C:99:1E | |
| 1 | [Espacio para etiqueta] | 80:7D:3A:2B:E3:59 | |
| 2 | [Espacio para etiqueta] | 8C:AA:B5:0F:39:7F | |
| 3 | [Espacio para etiqueta] | 50:02:91:DE:0D:7B | |
| 4 | [Espacio para etiqueta] | 48:3F:DA:AA:8E:6D | |
| Bridge | Junto al PC | 48:3F:DA:05:86:36 | Conectado por USB al PC |

## Distancias y Alcance

- ESP-NOW funciona típicamente hasta **50 metros en campo abierto**
- En interiores, el alcance se reduce a **15–30 metros** (dependiendo de paredes)
- Todos los nodos deben estar dentro del alcance del bridge
- Si un nodo está fuera de alcance, se mostrará como timeout en el bridge

## Canales WiFi

ESP-NOW usa el mismo canal que el WiFi. El canal se configura automáticamente al iniciar:
```c
WiFi.mode(WIFI_STA);
WiFi.disconnect();
```

- **Canal por defecto**: El que seleccione el driver WiFi (típicamente canal 1)
- Todos los nodos y el bridge deben estar en el **mismo canal** para comunicarse
- No es necesario conectarse a un AP — ESP-NOW funciona en modo Station desconectado
