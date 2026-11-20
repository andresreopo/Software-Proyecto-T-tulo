#pragma once

#include <stdint.h>

class RotorAzLimit {
 public:
  bool tripped() const { return latched; }

  void sample(bool pressed, uint32_t nowMs) {
    if (pressed) {
      // Disparar con el primer LOW. Filtrar el rebote solo al liberar.
      latched = true;
      releaseSeen = false;
    } else if (latched && !releaseSeen) {
      releaseSeen = true;
      releasedSinceMs = nowMs;
    }
  }

  // Solo se llama con STOP, después de volver a leer el contacto.
  bool rearm(uint32_t nowMs, uint32_t releaseMs) {
    if (!latched || !releaseSeen || nowMs - releasedSinceMs < releaseMs) return false;
    latched = false;
    releaseSeen = false;
    return true;
  }

 private:
  bool latched = false;
  bool releaseSeen = false;
  uint32_t releasedSinceMs = 0;
};
