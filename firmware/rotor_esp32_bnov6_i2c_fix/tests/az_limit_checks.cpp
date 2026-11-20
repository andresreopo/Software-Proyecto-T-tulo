#include <cassert>

#include "RotorAzLimit.h"
#include "RotorConfig.h"

int main() {
  constexpr uint32_t delayMs = RotorConfig::AZ_LIMIT_RELEASE_MS;
  RotorAzLimit limit;
  const auto sample = [&](bool pressed, uint32_t nowMs, int approachDir) {
    limit.sample(pressed, nowMs, approachDir, delayMs);
  };
  assert(!limit.tripped());

  // Sin un último paso conocido, el contacto presionado impide mover.
  sample(true, 0, 0);
  assert(limit.tripped());
  assert(limit.backoffDirection() == 0);
  sample(false, 100, 0);
  sample(false, 100 + delayMs, 0);
  assert(!limit.tripped());

  // Al soltar se detienen los pasos. Un rebote continúa en el mismo sentido
  // con el mismo presupuesto de pasos, aunque cambie el sentido de la referencia.
  sample(true, 200, 1);
  assert(limit.backoffDirection() == -1);
  limit.recordBackoffStep(2);
  sample(false, 210, 0);
  assert(limit.backoffDirection() == 0);
  sample(true, 211, -1);
  assert(limit.backoffDirection() == -1);
  limit.recordBackoffStep(2);
  assert(limit.tripped());
  assert(!limit.canBackOff());
  assert(limit.backoffDirection() == 0);
  sample(false, 220, 0);
  sample(false, 220 + delayMs - 1, 0);
  assert(limit.tripped());
  sample(false, 220 + delayMs, 0);
  assert(!limit.tripped());
  assert(limit.trips() == 2);

  // Aproximación en sentido negativo. Un STOP cancela el retroceso incluso tras un rebote.
  sample(true, UINT32_MAX - 3, -1);
  assert(limit.backoffDirection() == 1);
  limit.cancelBackoff();
  sample(true, UINT32_MAX - 2, -1);
  assert(limit.backoffDirection() == 0);
  sample(false, UINT32_MAX - 1, 0);
  sample(false, delayMs - 3, 0);
  assert(limit.tripped());
  sample(false, delayMs - 2, 0);
  assert(!limit.tripped());

  // Se descarta la secuencia TRK anterior hasta que haya una pausa completa.
  constexpr uint32_t quietMs = RotorConfig::TRK_TIMEOUT_MS;
  RotorTrackAfterLimit track;
  assert(track.accept(0, 0, false, quietMs));
  assert(!track.accept(200, 1, true, quietMs));
  assert(!track.accept(400, 1, false, quietMs));
  assert(!track.accept(400 + quietMs - 1, 1, false, quietMs));
  assert(track.accept(400 + 2 * quietMs - 1, 1, false, quietMs));
  assert(!track.accept(500 + 2 * quietMs, 2, false, quietMs));  // aviso no observado durante el toque
  RotorTrackAfterLimit firstTrack;
  assert(firstTrack.accept(200, 1, false, quietMs));  // sin una secuencia anterior
}
