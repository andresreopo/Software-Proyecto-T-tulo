// esp32_rotor.ino — lado ESP32 del enlace serial con SATTRACK.
//
//   PC → ESP32 :  SET,<az>,<el>\n                 ir a una referencia fija (grados)
//   PC → ESP32 :  TRK,<az>,<el>,<vaz>,<vel>\n     seguimiento: posición del satélite (grados) y su
//                                                 velocidad angular (°/s; vaz > 0 = azimut creciente), ~5 Hz
//   ESP32 → PC :  POS,<az>,<el>\n                 posición actual, ~5 Hz
//   ESP32 → PC :  ACK / ERR,formato               respuesta a SET (TRK no responde: llega a 5 Hz)
// Los mensajes de depuración deben empezar con '#' (aparecen en el registro de la interfaz).
//
// SEGUIMIENTO: entre dos TRK el objetivo sigue moviéndose. reference() extrapola la posición recibida con
// su velocidad, así el objetivo es continuo y el rotor puede moverse al mismo ritmo que el satélite
// (feedforward de velocidad + corrección proporcional del error). Si dejan de llegar TRK durante
// TRK_TIMEOUT_MS, la ESP32 se detiene y mantiene su posición.
//
// SIMULATE_MOTION 1 → la ESP32 simula un rotor (12 °/s en azimut, 6 °/s en elevación).
//                     Sirve para comprobar el cable, los baudios y el seguimiento SIN motores.
// SIMULATE_MOTION 0 → usa tu hardware: completa readAzimuth(), readElevation() y driveTowards().
//
// Revisado solo en un PC con un reemplazo mínimo de Arduino; NO compilado con el core de ESP32.

#define SIMULATE_MOTION 1

const unsigned long BAUD           = 115200;   // debe coincidir con los baudios de la interfaz
const unsigned long POS_PERIOD_MS  = 200;
const unsigned long TRK_TIMEOUT_MS = 1500;
const float EL_MIN = 0.0f, EL_MAX = 90.0f;     // iguales a ROTOR_EL_MIN/MAX de rotor_config.py

float curAz = 0, curEl = 0;                    // posición actual
float tgtAz = 0, tgtEl = 0;                    // última referencia recibida
bool  trk = false;                             // true mientras llegan TRK
float ffAz = 0, ffEl = 0;                      // velocidad angular del objetivo (°/s)
unsigned long trkMs = 0;                       // instante del último TRK
String line;

float wrap180(float x) { return fmodf(x + 540.0f, 360.0f) - 180.0f; }   // diferencia angular en (-180, 180]

// Referencia vigente y velocidad del objetivo (0 si no hay seguimiento).
void reference(unsigned long now, float &az, float &el, float &vaz, float &vel) {
  if (trk && now - trkMs > TRK_TIMEOUT_MS) {   // dejaron de llegar TRK: mantener la posición
    trk = false; ffAz = ffEl = 0; tgtAz = curAz; tgtEl = curEl;
  }
  if (trk) {
    float age = (now - trkMs) / 1000.0f;
    az  = fmodf(tgtAz + ffAz * age + 720.0f, 360.0f);
    el  = constrain(tgtEl + ffEl * age, EL_MIN, EL_MAX);
    vaz = ffAz; vel = ffEl;
  } else {
    az = tgtAz; el = tgtEl; vaz = 0; vel = 0;
  }
}

#if SIMULATE_MOTION
const float SIM_AZ_RATE = 12.0f, SIM_EL_RATE = 6.0f;    // °/s máximos del rotor simulado
const float KP = 2.0f;                                  // 1/s, ganancia sobre el error de posición

void updatePosition(float dt, unsigned long now) {
  float az, el, vaz, vel;
  reference(now, az, el, vaz, vel);
  if (trk) {                                            // feedforward de velocidad + corrección
    float rAz = constrain(vaz + KP * wrap180(az - curAz), -SIM_AZ_RATE, SIM_AZ_RATE);
    float rEl = constrain(vel + KP * (el - curEl),        -SIM_EL_RATE, SIM_EL_RATE);
    curAz = fmodf(curAz + rAz * dt + 360.0f, 360.0f);
    curEl = constrain(curEl + rEl * dt, EL_MIN, EL_MAX);
  } else {                                              // referencia fija: velocidad máxima
    float stepAz = SIM_AZ_RATE * dt, stepEl = SIM_EL_RATE * dt;
    curAz = fmodf(curAz + constrain(wrap180(az - curAz), -stepAz, stepAz) + 360.0f, 360.0f);
    curEl += constrain(el - curEl, -stepEl, stepEl);
  }
}
#else
float readAzimuth()   { return curAz; }        // TODO: leer encoder / potenciómetro / IMU
float readElevation() { return curEl; }        // TODO
// az/el: referencia ya extrapolada; vaz/vel: velocidad del objetivo en °/s (0 si no hay seguimiento).
// Un control típico: velocidad_comando = vaz + Kp * wrap180(az - curAz)  (PID con feedforward).
void driveTowards(float az, float el, float vaz, float vel) {
  // TODO: control de motores
}

void updatePosition(float dt, unsigned long now) {
  curAz = readAzimuth();
  curEl = readElevation();
  float az, el, vaz, vel;
  reference(now, az, el, vaz, vel);
  driveTowards(az, el, vaz, vel);
}
#endif

// Lee n números separados por coma desde la posición 'from'. false si faltan o sobran.
bool parseFloats(const String &s, int from, float *out, int n) {
  int start = from;
  for (int i = 0; i < n; i++) {
    int c = (i < n - 1) ? s.indexOf(',', start) : (int)s.length();
    if (c < 0) return false;
    String f = s.substring(start, c);
    if (f.indexOf(',') >= 0) return false;
    out[i] = f.toFloat();
    start = c + 1;
  }
  return true;
}

void handleLine(String s) {
  s.trim();
  float v[4];
  if (s.startsWith("SET,")) {
    if (!parseFloats(s, 4, v, 2)) { Serial.println("ERR,formato"); return; }
    tgtAz = fmodf(fmodf(v[0], 360.0f) + 360.0f, 360.0f);
    tgtEl = constrain(v[1], EL_MIN, EL_MAX);
    trk = false; ffAz = ffEl = 0;
    Serial.println("ACK");
  } else if (s.startsWith("TRK,")) {
    if (!parseFloats(s, 4, v, 4)) { Serial.println("ERR,formato"); return; }
    tgtAz = fmodf(fmodf(v[0], 360.0f) + 360.0f, 360.0f);
    tgtEl = constrain(v[1], EL_MIN, EL_MAX);
    ffAz = v[2]; ffEl = v[3];
    trk = true; trkMs = millis();
  }
}

void setup() {
  Serial.begin(BAUD);
  Serial.println("# esp32_rotor listo");
}

void loop() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n') { handleLine(line); line = ""; }
    else if (ch != '\r' && line.length() < 64) line += ch;
  }

  static unsigned long last = millis();
  unsigned long now = millis();
  updatePosition((now - last) / 1000.0f, now);
  last = now;

  static unsigned long t = 0;
  if (now - t >= POS_PERIOD_MS) {
    t = now;
    Serial.printf("POS,%.2f,%.2f\n", curAz, curEl);
  }
}
