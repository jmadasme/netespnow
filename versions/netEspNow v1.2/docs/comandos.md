# Comandos del Bridge — netEspNow v1.2

Todos los comandos se envían por el monitor serial a 115200 baud.

## ping `<id>`

Interroga a un nodo específico (0-4) a través de la Puerta de Enlace.

```
> ping 0
[CMD] Preguntando a Nodo 0 via Puerta de Enlace...
[ENVIO CIFRADO] Comando -> Puerta de Enlace (nodo=0, cmd=ping 0)
...
[SEGURIDAD] Firma ECDSA VALIDA - Puerta autenticada
----------------------------------------
RESPUESTA DESCIFRADA - Nodo #0
  Nodo 0 | contador=42 | uptime=3600000 ms
  RTT (via Puerta): 847 ms
----------------------------------------
```

## ping all

Interroga a todos los nodos secuencialmente.

```
> ping all
[CMD] Preguntando a TODOS los nodos...
```

## list

Muestra la tabla de nodos conocidos y estado del cifrado.

```
> list
=== TABLA DE NODOS ===
  Nodo 0: CONOCIDO [84:F3:EB:7C:99:1E]
  Nodo 1: CONOCIDO [80:7D:3A:2B:E3:59]
  Nodo 2: CONOCIDO [8C:AA:B5:0F:39:7F]
  Nodo 3: CONOCIDO [50:02:91:DE:0D:7B]
  Nodo 4: CONOCIDO [48:3F:DA:AA:8E:6D]
  Puerta de Enlace: CONOCIDA [48:55:19:14:A8:39]
  Cifrado ECC: ACTIVO | Clave Puerta: CONOCIDA
========================
```

## stats

Muestra estadísticas de comunicación.

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
====================
```

## auto on / auto off

Activa o desactiva la interrogación automática de nodos.

```
> auto on
[OK] Interrogacion automatica ACTIVADA

> auto off
[OK] Interrogacion automatica DESACTIVADA
```

## keygen

Genera un nuevo par de claves ECC y las guarda en EEPROM.

```
> keygen
[INFO] Generando nuevo par de claves ECC...
[OK] Claves ECC generadas
[EEPROM] Claves propias guardadas
```

> **Importante**: Al regenerar claves, la Puerta de Enlace debe tener la nueva clave pública. Use `showkey` para obtenerla y actualícela en la Puerta.

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

=== COMANDOS DISPONIBLES (netEspNow v1.2) ===
  ping <id>    : Preguntar a un nodo (0-4) via Puerta de Enlace
  ping all     : Preguntar a todos los nodos
  list         : Mostrar tabla de nodos
  stats        : Mostrar estadisticas
  auto on      : Activar interrogacion automatica
  auto off     : Desactivar interrogacion automatica
  keygen       : Generar nuevo par de claves ECC
  learnkey     : Aprender clave publica de Puerta desde EEPROM
  showkey      : Mostrar clave publica de este Bridge
  help / ?     : Mostrar esta ayuda
=============================================
```
