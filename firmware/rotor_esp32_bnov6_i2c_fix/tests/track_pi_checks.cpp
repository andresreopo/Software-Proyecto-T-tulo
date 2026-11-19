#include <cassert>

#include "RotorConfig.h"
#include "RotorTrackPI.h"

namespace {
namespace cfg = RotorConfig;
using RotorTrack::Integrator;
using RotorTrack::RateRange;

constexpr double absValue(double value) { return value < 0 ? -value : value; }
constexpr bool near(double a, double b) { return absValue(a - b) < 1e-9; }

// Modelo determinista: lazo de 1 ms, TRK a 5 Hz y BNO a 100 Hz con 20 ms de atraso.
// El eje real se mueve un 25 % menos por paso de lo que estima el firmware.
double trackingError(double targetRate, double ki, double stepsPerDeg, double maxStepsS) {
  constexpr double dt = 0.001;
  const double maxRate = maxStepsS / stepsPerDeg;
  const RateRange allowed{-maxRate, maxRate};
  const double rampDelta = cfg::RAMP_ACCELERATION * dt / stepsPerDeg;
  const RateRange rampStep{-rampDelta, rampDelta};
  Integrator integral;
  double position = 0, measured = 0, applied = 0;
  double sampleDelay[20] = {};
  double packetPosition = 0, errorSum = 0;
  for (int tick = 0; tick < 15000; ++tick) {
    const double reference = targetRate * dt * tick;
    if (tick % 200 == 0) packetPosition = reference;
    if (tick % 10 == 0) {
      const double noise = tick % 20 == 0 ? 0.08 : -0.08;
      measured = sampleDelay[tick % 20] + noise;
    }
    sampleDelay[tick % 20] = position;
    const double extrapolated = packetPosition + targetRate * dt * (tick % 200);
    const double error = extrapolated - measured;
    const double requested = integral.update(targetRate + cfg::CTRL_KP * error, ki * error * dt,
                                             cfg::TRK_MAX_INTEGRAL_DEG_S,
                                             allowed, applied, rampDelta);
    applied += rampStep.clamp(requested - applied);
    if (tick >= 10000) errorSum += absValue(reference - position);
    position += 0.75 * applied * dt;
  }
  return errorSum / 5000;
}

bool saturationDoesNotWindUp() {
  Integrator integral;
  constexpr RateRange allowed{-4.5, 4.5};
  for (int i = 0; i < 100; ++i) {
    if (!near(integral.update(12, .2, 1.5, allowed, 4.5, .003), 4.5)) return false;
    integral.update(-2, -.2, 1.5, allowed, 0, .003);
  }
  return near(integral.update(0, 0, 1.5, allowed, 0, .003), 0);
}

bool integralUnwindsAndResets() {
  Integrator integral;
  constexpr RateRange allowed{-4.5, 4.5};
  // De 1 °/s de margen en la rampa, solo se acumula hasta 1.5 °/s en total.
  if (!near(integral.update(0, 2, 1.5, allowed, 0, 1), 1)) return false;
  if (!near(integral.update(0, 2, 1.5, allowed, 1, 1), 1.5)) return false;
  integral.update(12, -.5, 1.5, allowed, 4.5, .003);
  if (!near(integral.update(0, 0, 1.5, allowed, 1, .003), 1)) return false;
  integral.reset();
  return near(integral.update(0, 0, 1.5, allowed, 0, .003), 0);
}

bool mechanicalStopClearsOutwardCorrection() {
  Integrator integral;
  constexpr RateRange allowed{-4.5, 4.5};
  constexpr RateRange upperStop{-4.5, 0};
  integral.update(0, .5, 1.5, allowed, 0, 1);
  if (!near(integral.update(1, .5, 1.5, upperStop, .5, .003), 0)) return false;
  if (!near(integral.update(0, 0, 1.5, allowed, 0, .003), 0)) return false;
  return integral.update(-1, -.5, 1.5, upperStop, 0, .003) < 0;
}

}  // namespace

int main() {
  assert(trackingError(2.5, 0, cfg::AZ_STEPS_PER_DEG, cfg::AZ_MAX_SPEED) > .25);
  assert(trackingError(2.5, cfg::TRK_KI_AZ, cfg::AZ_STEPS_PER_DEG, cfg::AZ_MAX_SPEED) < .02);
  assert(trackingError(-2.5, cfg::TRK_KI_EL, cfg::EL_STEPS_PER_DEG, cfg::EL_MAX_SPEED) < .02);
  assert(saturationDoesNotWindUp());
  assert(integralUnwindsAndResets());
  assert(mechanicalStopClearsOutwardCorrection());
}
