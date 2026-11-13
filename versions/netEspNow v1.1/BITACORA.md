# Bitácora del Proyecto — netEspNow v1.1

> **Propósito**: Este documento registra todo el historial del proyecto, decisiones técnicas, estado actual y próximos pasos. Léelo al inicio de cada sesión para retomar sin necesidad de re-explicar.

---

## 1. Resumen del Proyecto

**Nombre**: netEspNow v1.1  
**Propósito**: Red de sensores inalámbrica basada en ESP-NOW con 5 nodos sensores ESP8266 + 1 bridge USB  
**Protocolo**: ESP-NOW (8 bytes por mensaje, request/response)  
**Estado actual**: ✅ OPERATIVO — Red funcionando, documentada y publicada en GitHub  

---

## 2. Historia Completa

### 2.1 — Intento fallido con ESP32
- Se intentó usar un **ESP32 DevKit v1** (ESP32‑D0WD‑V3 rev v3.1) como maestro
- El ESP32 entró en **boot-loop** con el error:
  ```
  E (57) flash_parts: partition 0 invalid magic number 0x1389
  Failed to verify partition table
  ```
- Causa: El bootloader del core ESP32 3.3.x es incompatible con chip revision v3.1
- **Decisión**: Abandonar ESP32, migrar toda la red a **ESP8266 NodeMCU v3**

### 2.2 — Migración a ESP8266
- Se reemplazó el ESP32 por un **ESP8266 NodeMCU v3** como bridge/maestro
- Se reprogramaron todos los nodos como esclavos ESP8266
- El bridge originalmente era la **5ª ESP8266** (MAC: `48:3F:DA:05:86:36`)
- IDs 0-3: Nodos sensores físicos
- ID 4: Se programó posteriormente (MAC: `48:3F:DA:AA:8E:6D`)
- ID 5: Reservado para el bridge (no se interroga a sí mismo)

### 2.3 — Problemas resueltos durante el desarrollo

| # | Problema | Síntoma | Solución |
|---|---|---|---|
| 1 | ESP32 boot-loop | No arranca, error de partición | Migrar a ESP8266 |
| 2 | ISR unsafe en callbacks | Crashes aleatorios | Flags volátiles, sin Serial.print() en ISR |
| 3 | Timeout siempre 4.2s | `4294967295 ms` | Inicializar `request_sent_time` correctamente |
| 4 | Stats mostraban ID incorrecto en timeout | Mostraba 0 en vez del nodo real | Guardar `last_polled_node_id` antes de enviar |
| 5 | Auto-poll no se desactivaba con `auto off` | Seguía preguntando | Cambiar a flag booleano (`auto_poll_enabled`) |
| 6 | Estadísticas spam cada 30s | Saturaban el monitor | Condicionar a `auto_poll_enabled` |
| 7 | Bridge se interrogaba a sí mismo (ID 5) | Timeout en nodo 5 | `isSelfNode()` con detección dinámica de MAC |
| 8 | Auto-identificación asignaba bridge a nodo 4 | Tabla incorrecta | Hardcodear MAC de nodo 4 en la tabla inicial |
| 9 | Peer duplicado para self-MAC | Error al agregar peer | `!isSelfNode(i)` en el loop de peers |

### 2.4 — Autodescubrimiento de nodos
- Nodos 0-3: MACs pre-cargadas en la tabla del bridge
- Nodo 4: Se aprendió por broadcast cuando se programó físicamente
- El bridge detecta automáticamente la MAC de cualquier nodo que responda por broadcast

---

## 3. Estado Actual

### 3.1 — Hardware

| ID | MAC | Rol | Estado físico |
|---|---|---|---|
| 0 | `84:F3:EB:7C:99:1E` | Nodo sensor | ✅ Operativo |
| 1 | `80:7D:3A:2B:E3:59` | Nodo sensor | ✅ Operativo |
| 2 | `8C:AA:B5:0F:39:7F` | Nodo sensor | ✅ Operativo |
| 3 | `50:02:91:DE:0D:7B` | Nodo sensor | ✅ Operativo |
| 4 | `48:3F:DA:AA:8E:6D` | Nodo sensor | ✅ Operativo (programado después) |
| 5 | `48:3F:DA:05:86:36` | **Bridge** (PC via USB) | ✅ Conectado a CuteCom |
| — | `48:3F:DA:AA:8E:6D`? | **Placa cerámica** (nuevo nodo) | 🕒 Pendiente de definir rol |

**Nota**: Hay una ESP8266 adicional encapsulada en cerámica, sin acceso a pines, con cable USB externalizado. Pendiente de asignarle un rol (candidata a Nodo de Seguridad).

### 3.2 — Firmware

| Componente | Archivo | Líneas | Última modificación |
|---|---|---|---|
| Nodo sensor (genérico) | `firmware_esp8266_nodo.ino` | 170 | Base inicial |
| Nodo 0 | `firmware_nodo_0.ino` | 168 | Hardcode NODE_ID=0 |
| Nodo 1 | `firmware_nodo_1.ino` | 168 | Hardcode NODE_ID=1 |
| Nodo 2 | `firmware_nodo_2.ino` | 168 | Hardcode NODE_ID=2 |
| Nodo 3 | `firmware_nodo_3.ino` | 168 | Hardcode NODE_ID=3 |
| Nodo 4 | `firmware_nodo_4.ino` | 168 | Hardcode NODE_ID=4 |
| Bridge | `firmware_bridge.ino` | 571 | Versión final con isSelfNode |
| ESP32 | `firmware_esp32_maestro.ino` | — | 🗑️ Deprecado (no usar) |

### 3.3 — Documentación

| Archivo | Contenido |
|---|---|
| `README.md` | Descripción general, pinout, mapa de red, protocolo, comandos, MACs, troubleshooting |
| `docs/hardware.md` | Especificaciones técnicas NodeMCU v3, pinout, GPIOs usados |
| `docs/protocolo.md` | Estructura ESP-NOW, flujo request/response, ISR, timing, peer management |
| `docs/instalacion.md` | Instalación arduino-cli, compilación, subida, script rápido, errores comunes |
| `docs/comandos.md` | Referencia completa: ping, list, stats, auto on/off, help |
| `docs/red.md` | Topología, tabla MACs, identificación física, alcance |
| `docs/solucion_problemas.md` | 14 problemas con solución detallada |
| `BITACORA.md` | **Este documento** — historial y estado del proyecto |

### 3.4 — GitHub

- **Repositorio**: `github.com/wanderc137/netEspNow-v1.1` (privado)
- **Cuenta**: wanderc137
- **Contenido**: Todo el proyecto (firmwares, docs, README, .gitignore)
- **NO incluido**: PLAN.md, AUDIT_REPORT.md (excluidos por .gitignore)

---

## 4. Comandos del Bridge (Referencia Rápida)

```
ping <id>    - Interrogar nodo especifico (0-4)
ping all     - Interrogar todos los nodos
list         - Mostrar tabla de nodos con MAC
stats        - Mostrar estadisticas
auto on      - Activar interrogacion automatica
auto off     - Desactivar interrogacion automatica (silencio)
help         - Mostrar esta ayuda
```

**Puerto**: `/dev/ttyUSB0` — **Baudrate**: 115200  
**Monitor recomendado**: CuteCom (RAW mode, CR/LF al enviar)

---

## 5. Hallazgos Técnicos Importantes

### ESP-NOW en ESP8266
- **FQBN**: `esp8266:esp8266:nodemcuv2`
- **ISR**: Los callbacks son en contexto de interrupción — NO usar Serial, delay, o memcpy lentos
- **Roles**: `ESP_NOW_ROLE_COMBO` tanto en bridge como en nodos
- **Peers**: Agregar peer broadcast (`FF:FF:FF:FF:FF:FF`) para descubrimiento
- **Self-MAC**: No agregar peer con la propia MAC — causa errores silenciosos

### Compilación con NODE_ID variable
```bash
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
  --build-property build.extra_flags=-DNODE_ID=X \
  ruta/al/firmware/
```

### Placa cerámica ESP8266
- Es un ESP8266EX encapsulado en cerámica (similar a ESP-01 pero sellado)
- **No tiene Bluetooth** (solo ESP32 tiene Bluetooth)
- **No tiene sensor Hall** (solo ESP32 tiene sensor Hall interno)
- Solo accesible por cable USB externalizado — sin pines GPIO disponibles
- Ideal para roles que no requieran sensores físicos (seguridad, presencia, testigo)

---

## 6. Próximos Pasos (Ideas para Futuras Sesiones)

### Pendientes inmediatos:
- [ ] Agregar la placa cerámica a la red como Nodo 5 (firmware básico)
- [ ] Verificar estabilidad de la red tras horas de funcionamiento continuo
- [ ] Identificar físicamente nodos 0-4 con etiquetas

### Mejoras planeadas:
- [ ] **Auto-reboot por pérdida de red** en nodos (watchdog)
- [ ] **Nodo de Seguridad (Key Node)** para validación de integridad
- [ ] **Algoritmo dinámico de tokens** entre nodo crítico y Key Node
- [ ] Token de un solo uso por petición (anti-replay)
- [ ] **Conexión de sensores reales** (DHT22, HC-SR04, etc.) a los nodos

### Ideas exploradas pero no priorizadas:
- [ ] Repetidor/extensor de alcance ESP-NOW
- [ ] Puente Bluetooth ↔ ESP-NOW (requiere ESP32)
- [ ] Dashboard web en el bridge
- [ ] Deep sleep para nodos a batería

---

## 7. Notas para la Próxima Sesión

Al retomar el proyecto:

1. **Conectar bridge** a `/dev/ttyUSB0`
2. **Abrir CuteCom** a 115200 baud
3. Verificar con `list` que los 5 nodos están presentes
4. Probar `ping 0` .. `ping 4` para verificar respuestas
5. Si todo funciona → red estable ✅
6. Si algún nodo falló → diagnosticar (alcance, alimentación, reinicio)

---

*Última actualización: 30 Julio 2026*
