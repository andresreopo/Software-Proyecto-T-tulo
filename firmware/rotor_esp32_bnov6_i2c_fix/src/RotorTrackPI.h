#pragma once

namespace RotorTrack {

struct RateRange {
  double min;
  double max;

  constexpr double clamp(double rate) const {
    return rate < min ? min : (rate > max ? max : rate);
  }
};

class Integrator {
 public:
  constexpr void reset() { correction = 0; }

  // Todas las velocidades, incluida la corrección integral, están en °/s.
  constexpr double update(double baseRate, double increment, double maxCorrection,
                          RateRange allowed, double currentRate, double rampDelta) {
    // Al tocar un límite físico se descarta la corrección que empuja hacia el tope.
    correction = allowed.clamp(correction);
    const double requested = baseRate + correction;
    const double roomDown = allowed.clamp(currentRate - rampDelta) - requested;
    const double roomUp = allowed.clamp(currentRate + rampDelta) - requested;

    // No integrar más allá de lo que el próximo paso de la rampa puede aplicar.
    // Cuando ya se satura, el incremento en sentido contrario puede descargar la integral.
    const RateRange incrementRange{roomDown < 0 ? roomDown : 0, roomUp > 0 ? roomUp : 0};
    const RateRange correctionRange{-maxCorrection, maxCorrection};
    correction = allowed.clamp(correctionRange.clamp(correction + incrementRange.clamp(increment)));
    return allowed.clamp(baseRate + correction);
  }

 private:
  double correction = 0;
};

}  // namespace RotorTrack
