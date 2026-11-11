// RotorMath.h — funciones puras (sin hardware) para el rotor:
// conversión grados↔pasos, camino más corto en azimut y parseo de texto.
// Se mantienen separadas de main.cpp para poder probarlas en un PC.
#pragma once

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

namespace RotorMath {
// Lleva cualquier ángulo al rango [0, 360).
inline double normalizeAzimuth(double degrees) {
  double result = fmod(degrees, 360.0);
  if (result < 0) result += 360.0;
  // Un valor negativo minúsculo (-1e-17) sumado a 360 se redondea a 360.0 exacto.
  return (result >= 360.0 || result == 0) ? 0.0 : result;
}

// Diferencia angular más corta, en [-180, 180).
inline double wrap180(double degrees) { return normalizeAzimuth(degrees + 180.0) - 180.0; }

// Un objetivo es válido si ambos valores son finitos y la elevación está en límites.
inline bool validTarget(double az, double el, double elMin, double elMax) {
  return isfinite(az) && isfinite(el) && el >= elMin && el <= elMax;
}

// grados -> pasos (con redondeo). Falla si el valor no cabe en el contador de pasos.
inline bool angleToSteps(double degrees, double stepsPerDegree, long &steps) {
  const double value = degrees * stepsPerDegree;
  // Se deja la mitad del rango del contador libre para la aritmética de AccelStepper.
  if (!isfinite(value) || value < LONG_MIN / 2.0 || value > LONG_MAX / 2.0)
    return false;
  steps = lround(value);
  return true;
}

// Azimut objetivo -> posición en pasos.
//  shortestPath=true : elige la vuelta que minimiza el giro (cruza el norte 359°->1°);
//                      requiere que el mecanismo gire libre (sin cables que se enreden).
//  shortestPath=false: usa siempre 0..360° absolutos (nunca cruza el norte).
inline bool azimuthToSteps(double az, long currentSteps, double stepsPerDegree,
                           bool shortestPath, long &targetSteps) {
  if (!isfinite(az) || !isfinite(stepsPerDegree) || stepsPerDegree <= 0)
    return false;
  const double target = normalizeAzimuth(az);
  if (!shortestPath) return angleToSteps(target, stepsPerDegree, targetSteps);
  const double current = currentSteps / stepsPerDegree;
  const double delta = normalizeAzimuth(target - normalizeAzimuth(current) +
                                        180.0) - 180.0;
  return angleToSteps(current + delta, stepsPerDegree, targetSteps);
}

// Avanza el puntero sobre espacios en blanco.
inline const char *skipSpaces(const char *p) {
  while (isspace(static_cast<unsigned char>(*p))) ++p;
  return p;
}

// Convierte texto a double; rechaza vacío, basura al final, NaN/inf.
inline bool parseNumber(const char *text, double &value) {
  text = skipSpaces(text);
  if (!*text) return false;
  errno = 0;
  char *end = nullptr;
  const double parsed = strtod(text, &end);
  if (end == text || errno == ERANGE || !isfinite(parsed) || *skipSpaces(end))
    return false;
  value = parsed;
  return true;
}

// Parsea exactamente `n` números separados por coma ("12.5,-3,0.1"), p. ej. los
// argumentos de SET (n=2) o TRK (n=4). Falla si faltan, sobran o no son finitos.
inline bool parseList(const char *text, double *out, int n) {
  for (int i = 0; i < n; ++i) {
    text = skipSpaces(text);
    errno = 0;
    char *end = nullptr;
    const double v = strtod(text, &end);
    if (end == text || errno == ERANGE || !isfinite(v)) return false;
    out[i] = v;
    text = skipSpaces(end);
    if (i < n - 1) {
      if (*text != ',') return false;
      ++text;
    } else if (*text) {
      return false;  // basura o un número de más al final
    }
  }
  return true;
}

// Rota el vector (vx,vy,vz) con el quaternion unitario (w,x,y,z): v' = q v q^-1
//   v' = v + 2w (q×v) + 2 q×(q×v)
inline void rotateVector(double w, double x, double y, double z,
                         double vx, double vy, double vz, double out[3]) {
  const double tx = 2 * (y * vz - z * vy);
  const double ty = 2 * (z * vx - x * vz);
  const double tz = 2 * (x * vy - y * vx);
  out[0] = vx + w * tx + (y * tz - z * ty);
  out[1] = vy + w * ty + (z * tx - x * tz);
  out[2] = vz + w * tz + (x * ty - y * tx);
}

// Azimut (0° = norte, creciente hacia el este) y elevación (+ arriba) de un vector en el
// marco del mundo ENU (X = este, Y = norte, Z = arriba).
inline void azElFromWorldVector(const double v[3], double &az, double &el) {
  const double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  const double z = n > 0 ? v[2] / n : 0.0;
  const double kDeg = 180.0 / M_PI;
  el = asin(z > 1 ? 1 : (z < -1 ? -1 : z)) * kDeg;
  az = normalizeAzimuth(atan2(v[0], v[1]) * kDeg);
}

// Hacia dónde apunta la antena, a partir del quaternion (w,x,y,z) del BNO085.
//  * El quaternion rota del marco del sensor al marco del mundo ENU del BNO08x.
//  * (bx,by,bz) es el eje de la antena (boresight) expresado en el marco del sensor.
// Se rota ese eje al mundo y se calcula azimut y elevación. Así no importa cómo esté
// montado el sensor ni aparece el bloqueo de cardán de usar ángulos de Euler.
// La POSICIÓN del sensor (distancia al eje de giro) no influye: la orientación de un
// cuerpo rígido es la misma en todos sus puntos.
// En el cenit (el = ±90°) el azimut queda indefinido.
inline void pointingFromQuaternion(double w, double x, double y, double z,
                                   double bx, double by, double bz,
                                   double &az, double &el) {
  double v[3];
  rotateVector(w, x, y, z, bx, by, bz, v);
  azElFromWorldVector(v, az, el);
}

// Rotación que llevó al sensor de la orientación q0 a la q1, expresada en el MARCO DEL
// SENSOR: devuelve el ángulo (0..180°) y el eje unitario (en ejes del sensor) tal que
// girar +ángulo alrededor de ese eje lleva q0 a q1. Si el cuerpo giró solo alrededor
// de un eje fijo a él (la bisagra de elevación o el eje de azimut), ese eje es el de giro
// del mecanismo expresado en el sensor, sin suponer nada sobre cómo está pegado.
// Falla (false) si casi no hubo rotación y el eje no está definido.
inline bool relativeRotation(const double q0[4], const double q1[4], double &angleDeg,
                             double axis[3]) {
  // q_rel = conj(q0) * q1
  const double pw = q0[0], px = -q0[1], py = -q0[2], pz = -q0[3];
  double w = pw * q1[0] - px * q1[1] - py * q1[2] - pz * q1[3];
  double x = pw * q1[1] + px * q1[0] + py * q1[3] - pz * q1[2];
  double y = pw * q1[2] - px * q1[3] + py * q1[0] + pz * q1[1];
  double z = pw * q1[3] + px * q1[2] - py * q1[1] + pz * q1[0];
  if (w < 0) { w = -w; x = -x; y = -y; z = -z; }  // q y -q son la misma rotación
  const double s = sqrt(x * x + y * y + z * z);
  if (s < 1e-6) return false;
  angleDeg = 2.0 * atan2(s, w) * 180.0 / M_PI;
  axis[0] = x / s; axis[1] = y / s; axis[2] = z / s;
  return true;
}
}  // namespace RotorMath
