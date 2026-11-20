#include <cassert>

#include "RotorAzLimit.h"
#include "RotorConfig.h"

int main() {
  constexpr uint32_t delayMs = RotorConfig::AZ_LIMIT_RELEASE_MS;
  RotorAzLimit limit;
  assert(!limit.tripped());
  assert(!limit.rearm(0, delayMs));

  // Interruptor ya presionado al arrancar.
  limit.sample(true, 0);
  assert(limit.tripped());
  assert(!limit.rearm(100, delayMs));

  // Un rebote al soltar vuelve a iniciar el plazo.
  limit.sample(false, 110);
  assert(!limit.rearm(110, delayMs));
  limit.sample(true, 111);
  assert(!limit.rearm(111 + delayMs, delayMs));
  constexpr uint32_t releasedAt = 200;
  limit.sample(false, releasedAt);
  limit.sample(false, releasedAt + delayMs - 1);
  assert(!limit.rearm(releasedAt + delayMs - 1, delayMs));
  limit.sample(false, releasedAt + delayMs);
  assert(limit.tripped());  // Soltar no basta sin STOP.
  assert(limit.rearm(releasedAt + delayMs, delayMs));
  assert(!limit.tripped());

  // El plazo de liberación también funciona al desbordarse millis().
  limit.sample(true, UINT32_MAX - 2);
  limit.sample(false, UINT32_MAX - 1);
  assert(!limit.rearm(delayMs - 3, delayMs));
  assert(limit.rearm(delayMs - 2, delayMs));
  assert(!limit.tripped());
}
