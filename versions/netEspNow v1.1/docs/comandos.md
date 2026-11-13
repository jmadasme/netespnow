# Comandos del Bridge — Referencia Completa

## Introducción

El bridge expone una interfaz serie (USB, 115200 baud) que permite al usuario interactuar con la red ESP-NOW mediante comandos de texto. Los comandos se escriben en el monitor serial y se ejecutan inmediatamente.

---

## Comandos Disponibles

### ping \<id\>

**Propósito**: Interrogar a un nodo específico por su ID.

**Sintaxis**: `ping <id>` donde `<id>` es 0, 1, 2, 3 o 4.

**Ejemplo**:
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

**Comportamiento**:
- Si la MAC del nodo es conocida → envío directo
- Si la MAC es desconocida → envío broadcast (autodescubrimiento)
- Si el nodo no responde en 1000 ms → timeout

**IDs no válidos**:
- `ping 5`: El bridge responde `[INFO] Nodo 5 es el bridge, no puede interrogarse a si mismo`
- `ping 99`: El bridge responde `[ERROR] ID invalido`

---

### ping all

**Propósito**: Interrogar secuencialmente a todos los nodos conocidos (IDs 0-4).

**Sintaxis**: `ping all`

**Ejemplo**:
```
> ping all
[CMD] Preguntando a TODOS los nodos...
[ENVIO] Request -> Nodo 0 [84:F3:EB:7C:99:1E]
... (respuesta nodo 0)
[ENVIO] Request -> Nodo 1 [80:7D:3A:2B:E3:59]
... (respuesta nodo 1)
[ENVIO] Request -> Nodo 2 [8C:AA:B5:0F:39:7F]
... (respuesta nodo 2)
[ENVIO] Request -> Nodo 3 [50:02:91:DE:0D:7B]
... (respuesta nodo 3)
[ENVIO] Request -> Nodo 4 [48:3F:DA:AA:8E:6D]
... (respuesta nodo 4)
```

**Nota**: El bridge se salta automáticamente el ID 5 (su propio ID). Los requests se espacian 3 segundos entre sí para no saturar la red.

---

### list

**Propósito**: Mostrar la tabla de nodos con direcciones MAC y estado.

**Sintaxis**: `list`

**Ejemplo**:
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

**Estados posibles**:
| Estado | Significado |
|--------|-------------|
| `CONOCIDO` | El bridge ha aprendido la MAC del nodo |
| `DESCONOCIDO` | El bridge no conoce la MAC (nunca ha respondido) |
| `BRIDGE` | Es el propio bridge (no se interroga) |

---

### stats

**Propósito**: Mostrar estadísticas de comunicación de la sesión actual.

**Sintaxis**: `stats`

**Ejemplo**:
```
> stats
=== ESTADISTICAS ===
  Requests enviados:  42
  Respuestas OK:      40
  Timeouts:           2
  Tasa de exito:      95%
  Nodos conocidos:
    Nodo 0: CONOCIDO [...]
    Nodo 1: CONOCIDO [...]
    Nodo 2: CONOCIDO [...]
    Nodo 3: CONOCIDO [...]
    Nodo 4: CONOCIDO [...]
    Nodo 5: BRIDGE [...]
========================
```

**Nota**: Las estadísticas **solo se muestran cuando se invoca `stats` manualmente**. No hay impresión automática de estadísticas (eso saturaba el monitor).

---

### auto on

**Propósito**: Activar la interrogación automática cíclica de todos los nodos.

**Sintaxis**: `auto on`

**Ejemplo**:
```
> auto on
[OK] Interrogacion automatica ACTIVADA
```

**Comportamiento**:
- El bridge interroga un nodo cada 3 segundos
- Ciclo: Nodo 0 → 1 → 2 → 3 → 4 → 0 → 1 → ...
- El bridge se salta automáticamente su propio ID (5)
- Las respuestas se muestran en tiempo real en el monitor

---

### auto off

**Propósito**: Desactivar la interrogación automática.

**Sintaxis**: `auto off`

**Ejemplo**:
```
> auto off
[OK] Interrogacion automatica DESACTIVADA
(Solo responde a comandos: ping, list, stats)
```

**Comportamiento**:
- El bridge deja de interrogar automáticamente
- Solo responde a comandos manuales del usuario
- **Recomendado** para mantener el monitor serial limpio y legible

---

### help

**Propósito**: Mostrar la ayuda con todos los comandos disponibles.

**Sintaxis**: `help`

**Ejemplo**:
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

## Comandos no reconocidos

Si se escribe un comando inválido, el bridge responde:

```
> comando_inventado
[ERROR] Comando desconocido: 'comando_inventado' (escriba 'help' para ayuda)
```

---

## Mensajes del Sistema

El bridge también muestra mensajes informativos que no son respuesta a comandos:

| Mensaje | Significado |
|---------|-------------|
| `[ENVIO] Request -> Nodo X [MAC]` | Request enviado directo a un nodo conocido |
| `[ENVIO] Request broadcast para nodo X` | Request enviado por broadcast (MAC desconocida) |
| `[TIMEOUT] Nodo X - No se recibio respuesta` | El nodo no respondió en 1000 ms |
| `[APRENDIZAJE] MAC del nodo X: ...` | Se aprendió una nueva MAC de nodo |
| `[ERROR] Fallo al enviar request a nodo X` | Error interno ESP-NOW al enviar |
| `[CMD] Preguntando a Nodo X...` | Procesando comando ping manual |
| `[CMD] Preguntando a TODOS los nodos...` | Procesando comando ping all |

---

## Flujo de Trabajo Recomendado

### Para pruebas rápidas:
```
auto off
ping 0
ping 1
ping 2
ping 3
ping 4
```

### Para monitoreo continuo:
```
auto on
```
(El monitor mostrará respuestas cada 3 segundos)

### Para diagnóstico de red:
```
list
stats
ping 0
ping 1
ping 2
ping 3
ping 4
stats
```
