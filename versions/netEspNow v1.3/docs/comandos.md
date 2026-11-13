# Comandos del Bridge — netEspNow v1.3

Todos los comandos se envían por el monitor serial a 115200 baud.

## ping `<id>`

Interroga a un nodo específico (0-4) a través de la Puerta de Enlace.

```
> ping 1
[CMD] Preguntando a Nodo 1 via Puerta de Enlace...
[SEGURIDAD] Firma ECDSA VALIDA - Puerta autenticada
----------------------------------------
RESPUESTA DESCIFRADA - Nodo #1 [FUNC IO] in=0 adc=512 out=0
  RTT (via Puerta): 2354 ms
----------------------------------------
```

## ping all

Interroga a todos los nodos secuencialmente (activa interrogación automática).

```
> ping all
[CMD] Preguntando a TODOS los nodos...
```

## set `<id> on|off` — NUEVO en v1.3

Acciona la salida digital (D1/GPIO5) del nodo. Envía `MSG_TYPE_COMMAND` con `CMD_SET_STATE` a través de la Puerta.

```
> set 1 on
[CMD] Actuando GPIO salida del Nodo 1 -> ON
[SEGURIDAD] Firma ECDSA VALIDA - Puerta autenticada
RESPUESTA DESCIFRADA - Nodo #1 [FUNC IO] in=0 adc=512 out=1

> set 1 off
[CMD] Actuando GPIO salida del Nodo 1 -> OFF
RESPUESTA DESCIFRADA - Nodo #1 [FUNC IO] in=0 adc=512 out=0
```

También acepta `set <id> 1|0`.

## list

Muestra la tabla de nodos conocidos, su **función (FUNC)** y **versión de firmware (FW)** aprendidas dinámicamente (v1.3).

```
> list
=== TABLA DE NODOS ===
  Nodo 0: CONOCIDO [84:F3:EB:7C:99:1E] | FUNC=IO fw=13
  Nodo 1: CONOCIDO [80:7D:3A:2B:E3:59] | FUNC=IO fw=13
  Nodo 2: CONOCIDO [8C:AA:B5:0F:39:7F] | FUNC=IO fw=13
  Nodo 3: CONOCIDO [50:02:91:DE:0D:7B] | FUNC=IO fw=13
  Nodo 4: CONOCIDO [48:3F:DA:AA:8E:6D] | FUNC=IO fw=13
  Puerta de Enlace: CONOCIDA [48:55:19:14:A8:39]
  Cifrado ECC: ACTIVO | Clave Puerta: CONOCIDA
========================
```

## stats

Muestra estadísticas de comunicación y **estado I/O por nodo** (v1.3).

```
> stats
=== ESTADISTICAS ===
  Peticiones enviadas:  47
  Respuestas OK:        45
  Timeouts:             2
  Tasa de exito:        95%
  Claves ECC:           GENERADAS
  Clave Puerta:         CONOCIDA
  MAC Puerta:           CONOCIDA
  --- Estado I/O por nodo ---
  Nodo 0: sin datos I/O
  Nodo 1: in=0 adc=512 out=0
  Nodo 2: sin datos I/O
  ...
========================
```

> Los nodos sin respuesta muestran "sin datos I/O" (`has_io=false`) — un timeout no marca estado I/O.

## auto on / auto off

Activa o desactiva la interrogación automática de nodos (cada ~3 s).

```
> auto on
[OK] Interrogacion automatica ACTIVADA

> auto off
[OK] Interrogacion automatica DESACTIVADA
```

## keygen

Restaura las claves ECC de fábrica y las guarda en EEPROM (v1.3: claves hardcodeadas de fábrica, no aleatorias).

```
> keygen
[INFO] Restaurando claves ECC hardcodeadas de fabrica...
[OK] Claves ECC de fabrica restauradas en EEPROM
```

## learnkey

Carga la clave pública de la Puerta de Enlace desde EEPROM.

```
> learnkey
[OK] Clave publica de Puerta de Enlace cargada de EEPROM
  MAC Puerta: 48:55:19:14:A8:39
```

## showkey

Muestra la clave pública de este Bridge (para copiarla a la Puerta).

```
> showkey
=== CLAVE PUBLICA DEL BRIDGE ===
  Hex: 04 A1 B2 C3 D4 E5 F6 ... (64 bytes)
=================================
```

## help / ?

Muestra la ayuda completa.

```
> help

=== COMANDOS DISPONIBLES (netEspNow v1.3) ===
  ping <id>    : Preguntar a un nodo (0-4) via Puerta de Enlace
  ping all     : Preguntar a todos los nodos
  set <id> on|off : Actuar GPIO salida del nodo (0-4)
  list         : Mostrar tabla de nodos (funcion + FW)
  stats        : Mostrar estadisticas y estado I/O
  auto on      : Activar interrogacion automatica
  auto off     : Desactivar interrogacion automatica
  keygen       : Restaurar claves ECC de fabrica
  learnkey     : Aprender clave publica de Puerta desde EEPROM
  showkey      : Mostrar clave publica de este Bridge
  help / ?     : Mostrar esta ayuda
=============================================
```

---

*Comandos netEspNow v1.3 — Julio 2026*
