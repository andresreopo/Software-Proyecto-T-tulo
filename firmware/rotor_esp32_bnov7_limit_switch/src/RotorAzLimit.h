#pragma once

#include <stdint.h>

class RotorAzLimit {
 public:
  bool tripped() const { return latched; }
  bool canBackOff() const { return direction != 0; }
  uint32_t trips() const { return tripCount; }
  int backoffDirection() const { return releaseSeen ? 0 : direction; }

  void sample(bool pressed, uint32_t nowMs, int approachDir, uint32_t releaseMs) {
    if (pressed) {
      if (!latched) {
        latched = true;
        ++tripCount;
        direction = -approachDir;
        steps = 0;
      }
      releaseSeen = false;
    } else if (!latched) {
      return;
    } else if (!releaseSeen) {
      releaseSeen = true;
      releasedSinceMs = nowMs;
    } else if (nowMs - releasedSinceMs >= releaseMs) {
      latched = false;
      releaseSeen = false;
      direction = 0;
    }
  }

  void recordBackoffStep(unsigned maxSteps) {
    if (++steps >= maxSteps) cancelBackoff();
  }

  // Un STOP manual o el tope de recorrido impiden volver a intentar el retroceso.
  void cancelBackoff() { direction = 0; }

 private:
  bool latched = false;
  bool releaseSeen = false;
  int direction = 0;
  unsigned steps = 0;
  uint32_t releasedSinceMs = 0;
  uint32_t tripCount = 0;
};

// Una secuencia TRK que ya estaba llegando no puede reanudar el pase tras el STOP.
class RotorTrackAfterLimit {
 public:
  bool accept(uint32_t nowMs, uint32_t trips, bool limitActive, uint32_t quietMs) {
    if (trips != lastTrip || limitActive) blocked = true;
    lastTrip = trips;
    if (!limitActive && (!seenPacket || nowMs - lastPacketMs >= quietMs)) blocked = false;
    lastPacketMs = nowMs;
    seenPacket = true;
    return !blocked;
  }

 private:
  uint32_t lastTrip = 0;
  uint32_t lastPacketMs = 0;
  bool seenPacket = false;
  bool blocked = false;
};
