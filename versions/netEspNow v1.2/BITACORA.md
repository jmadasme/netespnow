# Bitácora del Proyecto — netEspNow v1.2

> **Propósito**: Este documento registra todo el historial del proyecto v1.2,
> que introduce una **Puerta de Enlace** con cifrado asimétrico entre el
> Bridge y la red ESP-NOW. Léelo al inicio de cada sesión para retomar sin
> re-explicar.

---

## 1. Resumen del Proyecto

**Nombre**: netEspNow v1.2  
**Propósito**: Red de sensores inalámbrica basada en ESP-NOW con **cifrado asimétrico**  
**Novedad v1.2**: 
- Se añade una **Puerta de Enlace** (ESP8266 cerámica) como intermediaria cifrada
- Bridge ya no habla directamente con los nodos — todo pasa por la Puerta de Enlace
- Cifrado asimétrico (claves pública/privada almacenadas en EEPROM)
- Nodos 0-4 modificados para apuntar a la Puerta de Enlace

**Estado actual**: 🚧 EN DESARROLLO — Pipeline en ejecución

---

## 2. Arquitectura v1.2

### Flujo de comunicación

```
PC --(serial)--> Bridge
  → Bridge cifra asimétricamente el comando
  → Bridge --(ESP-NOW cifrado)--> Puerta de Enlace
  → Puerta de Enlace descifra el comando
  → Puerta de Enlace --(ESP-NOW plano)--> Nodo X
  → Nodo X --(ESP-NOW plano)--> Puerta de Enlace
  → Puerta de Enlace cifra la respuesta
  → Puerta de Enlace --(ESP-NOW cifrado)--> Bridge
  → Bridge descifra y saca por serial --> PC
```

### Hardware

| ID | Dispositivo | Rol |
|---|---|---|
| Bridge | ESP8266 NodeMCU v3 | Recibe comandos serial, cifra/envía a Puerta de Enlace |
| Puerta de Enlace | ESP8266 cerámica | Intermediario cifrado, interroga nodos 0-4 |
| 0-4 | ESP8266 NodeMCU v3 | Nodos sensores (apuntan a Puerta de Enlace) |

---

## 3. Entregables v1.2

- [ ] `firmware_bridge/firmware_bridge.ino` — Bridge con cifrado asimétrico
- [ ] `firmware_puerta_enlace/firmware_puerta_enlace.ino` — Nuevo firmware
- [ ] `firmware_nodo_0/` a `firmware_nodo_4/` — Modificados (MAC destino = Puerta de Enlace)
- [ ] `docs/` — Documentación actualizada
- [ ] `README.md` — Nuevo README

---

*Iniciado: 30 Julio 2026*
