// =============================================================================
//  Rotor az/el con ESP32 — LAZO CERRADO con BNO085 — control por serial para la
//  interfaz unificada (SATTRACK / MaiaView).
//
//  La interfaz (Raspberry Pi / PC) envía referencias de azimut y elevación por el
//  puerto serie y el ESP32 devuelve periódicamente su posición actual.
//
//  Un BNO085 pegado a la antena mide hacia dónde apunta realmente (azimut y
//  elevación). Esa medición es la posición que se reporta (POS) y la que cierra el
//  lazo de control: los motores paso a paso se mueven hasta que el sensor indica que
//  se llegó, y corrigen si algo los desvía. Los pasos enviados ya no se usan como
//  posición, solo como velocidad y para detectar fallas.
//
//  PROTOCOLO (texto ASCII, una orden por línea terminada en '\n', 115200 baud)
//  Es el mismo que usa serial_link.py de la interfaz unificada.
//  ----------------------------------------------------------------------------
//   Interfaz -> ESP32                              Respuesta del ESP32
//   SET,<az>,<el>  ir a una posición y mantenerla   ACK   | ERR,<motivo>
//   TRK,<az>,<el>,<vaz>,<vel>                       (sin respuesta si es válido;
//                  seguimiento: posición del         ERR,<motivo> si no)
//                  satélite (°) y su velocidad
//                  angular (°/s, vaz>0 = az creciente), ~5 Hz
//   STOP           detener con rampa y borrar fallas ACK
//   CAL,<az>,<el>  declarar que la antena apunta     ACK   | ERR,busy | ERR,no_imu
//                  ahora a esa posición (se guarda). Con AZ_ABSOLUTE_MAGNETIC solo
//                  calibra la elevación: el azimut es absoluto y el <az> se ignora.
//   ZERO           equivale a CAL,0,0                idem
//   CALAX          calibración de ejes con el sensor ACK   | ERR,busy | ERR,no_imu
//                  (mueve ±CAL_SWEEP_DEG cada eje, ~30 s; STOP la aborta).
//                  Resultado en líneas '# calax ...'.
//   GET            pedir posición ahora              POS,<az>,<el>
//   IMU            diagnóstico del sensor            línea con '#'
//   ACC            precisión: fusión, error angular  línea con '#'
//                  estimado (°) y magnetómetro (MagAcc 0..3)
//   MAGCAL         calibrar el magnetómetro (figuras  ACK | ERR,busy | ERR,no_imu
//                  de 8, rotor detenido); informa '# magcal ...' cada 0.5 s
//   SAVECAL        guardar la calibración en la      ACK | ERR,busy | ERR,no_imu
//                  flash del BNO085 (termina MAGCAL)
//   ENDCAL         abortar MAGCAL sin guardar        ACK
//   HELP           lista de comandos                 línea con '#'
//
//   ESP32 -> Interfaz (sin que la interfaz lo pida)
//   POS,<az>,<el>  cada POS_PERIOD_MS: posición MEDIDA por el sensor (az 0..360, norte
//                  verdadero = rumbo magnético del sensor + MAG_DECLINATION_DEG).
//                  Si el sensor no responde, se deja de enviar (la interfaz marca la
//                  posición como obsoleta).
//   BAT,<volts>    cada BAT_PERIOD_MS: voltaje de la batería en V (p. ej. BAT,12.63).
//   # texto        diagnóstico; la interfaz lo muestra en su registro.
//   ERR,fault_az / ERR,fault_el   un eje no se movió como se esperaba: motores
//                  detenidos hasta recibir STOP (ver "Detector de fallas").
//
//   Motivos de ERR: formato, invalid_target, queue_full, busy, no_imu, fault,
//                   unknown_command, line_too_long, line_timeout, invalid_character.
//
//  CONTROL
//  ----------------------------------------------------------------------------
//  Todo el movimiento es por velocidad. Cada 1 ms, por eje:
//    SET : v = KP*error, limitada por la rampa de frenado sqrt(2*a*error) para llegar
//          sin pasarse; dentro de GOTO_DEADBAND_DEG se detiene y vigila.
//    TRK : v = vsat + KP*error + integral. La referencia se extrapola entre mensajes;
//          la integral corrige el error sostenido y respeta los límites y la rampa.
//          Si no llegan TRK por TRK_TIMEOUT_MS frena y queda en reposo.
//  La velocidad cambia con una rampa (RAMP_ACCELERATION) y se convierte en pulsos STEP.
//
//  SEGURIDAD
//  ----------------------------------------------------------------------------
//  * El final de carrera de azimut (GPIO 32 a LOW) cancela el movimiento. Azimut
//    puede retroceder en sentido contrario al último paso para liberar el contacto.
//  * Sin datos del sensor por IMU_TIMEOUT_MS: los motores frenan y no se acepta SET/TRK.
//  * Detector de fallas: cada FAULT_WINDOW_MS se compara el movimiento esperado según
//    los pasos enviados con el medido. Si no coincide (sentido invertido, motor sin
//    energía, sensor mal montado) se detienen ambos ejes y se responde ERR,fault_*
//    hasta recibir STOP. Es lo que evita que un sentido invertido haga girar el
//    rotor en la dirección equivocada sin parar.
//
//  ARQUITECTURA
//  ----------------------------------------------------------------------------
//   loop()        (core 1) lee el serial, interpreta comandos, envía POS y BAT.
//   motionTask()  (core 1, prioridad 2) único código que genera pulsos STEP.
//   imuTask()     (core 0) lee el BNO085 por I2C y publica la posición medida.
//   Se comunican con una cola de órdenes (loop -> motionTask) y con "fotos" del
//   estado protegidas por secciones críticas (imuTask/motionTask -> loop/motionTask).
// =============================================================================
#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>

#include "SparkFun_BNO080_Arduino_Library.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

#include <cstring>

#include "RotorAzLimit.h"
#include "RotorConfig.h"
#include "RotorMath.h"
#include "RotorTrackPI.h"

namespace cfg = RotorConfig;

static_assert(configTICK_RATE_HZ >= 1000,
              "La tarea de motores requiere un tick de FreeRTOS de 1 ms o menos");

namespace {

// Puerto por el que habla la interfaz (USB por defecto, o Serial2).
HardwareSerial &controlUart = cfg::UART_USE_SERIAL2 ? Serial2 : Serial;

// ── Ejes ─────────────────────────────────────────────────────────────────────
enum Axis { AZ = 0, EL = 1 };
struct AxisConfig {
  uint8_t stepPin, dirPin;
  bool invertDir;
  double stepsPerDeg;
  float maxSpeed;  // pasos/s
  double trackKi;
};
constexpr AxisConfig AXES[2] = {
    {cfg::AZ_STEP_PIN, cfg::AZ_DIR_PIN, cfg::AZ_INVERT_DIRECTION,
     cfg::AZ_STEPS_PER_DEG, cfg::AZ_MAX_SPEED, cfg::TRK_KI_AZ},
    {cfg::EL_STEP_PIN, cfg::EL_DIR_PIN, cfg::EL_INVERT_DIRECTION,
     cfg::EL_STEPS_PER_DEG, cfg::EL_MAX_SPEED, cfg::TRK_KI_EL},
};

// ── Comunicación entre tareas ────────────────────────────────────────────────
enum class CommandType : uint8_t { Target, Track, Stop, CalAxes, HomeEl };

// Orden que loop() entrega a motionTask().
struct MotionCommand {
  CommandType type;
  double az, el;    // grados (Target, Track)
  double vaz, vel;  // °/s (solo Track)
  uint32_t id;      // número de orden, para saber si ya fue procesada
  uint32_t timeMs;  // millis() al aceptar la orden (para extrapolar el TRK)
};

// Posición medida por el sensor (la publica imuTask).
struct Pose {
  double az = 0, el = 0;        // ya con offsets aplicados
  double rawAz = 0, rawEl = 0;  // sin offsets (para calibrar)
  double q[4] = {1, 0, 0, 0};   // quaternion del sensor (w,x,y,z), para CALAX
  float accuracyDeg = NAN;      // precisión estimada que informa el sensor
  uint8_t fusionAcc = 0;        // calidad de la fusión: 0=Unreliable .. 3=High
  uint8_t magAcc = 0;           // calibración del magnetómetro: 0=Unreliable .. 3=High
  uint32_t ms = 0;              // millis() de la última medición
  bool everValid = false;
};

enum Fault : uint8_t { FaultNone, FaultAz, FaultEl, FaultAzLimit, FaultAzLimitBlocked };
constexpr const char *AZ_LIMIT_ERROR = "fault_az_limit";

bool azLimitPressed() { return digitalRead(cfg::AZ_LIMIT_PIN) == LOW; }
bool elMinPressed() { return digitalRead(cfg::EL_LIMIT_MIN_PIN) == (cfg::EL_LIMIT_ACTIVE_LOW ? LOW : HIGH); }
bool elMaxPressed() { return digitalRead(cfg::EL_LIMIT_MAX_PIN) == (cfg::EL_LIMIT_ACTIVE_LOW ? LOW : HIGH); }
bool isAzLimitFault(Fault fault) { return fault == FaultAzLimit || fault == FaultAzLimitBlocked; }

// Estado que motionTask() publica para loop().
struct MotionState {
  bool moving = false;
  bool calibrating = false;
  bool elHomed = false;
  double elPosition = 0;
  uint32_t activeCommandId = 0;  // última orden ya procesada por motionTask
  Fault fault = FaultNone;
  uint32_t azLimitTrips = 0;
  // Detalle de la última evaluación de falla (válido si fault != FaultNone).
  float faultExpectedDeg = 0;  // movimiento esperado según pasos enviados (con signo)
  float faultMeasuredDeg = 0;  // movimiento medido por el sensor en esa ventana
  float faultWindowS = 0;      // duración de la ventana
  float faultElDeg = 0;        // elevación medida al detectar
};

// Datos crudos de una CALAX terminada; los entrega motionTask y los procesa loop().
struct CalData {
  bool ready = false;
  bool aborted = false;
  const char *reason = "";
  double q[4][4] = {};        // quaternions: az inicio, az fin, el inicio, el fin
  double azCmdDeg = 0;        // giro pedido (° según pasos enviados, con signo)
  double elCmdDeg = 0;
};
CalData sharedCal;

QueueHandle_t commandQueue = nullptr;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;  // protege sharedState
portMUX_TYPE poseMux = portMUX_INITIALIZER_UNLOCKED;   // protege pose y offsets
MotionState sharedState;
Pose sharedPose;
double azOffset = cfg::MAG_DECLINATION_DEG;  // rumbo magnético -> norte verdadero (° + CAL si no es absoluto)
double elOffset = cfg::EL_OFFSET;                         // corrección de elevación (°)
// Eje de la antena en el marco del sensor. Parte con el valor de RotorConfig.h y CALAX lo
// afina (se guarda en memoria). Protegido por poseMux.
double boresight[3] = {cfg::BORESIGHT_X, cfg::BORESIGHT_Y, cfg::BORESIGHT_Z};
bool boresightCalibrated = false;  // true si hay un eje guardado por CALAX
uint32_t lastAcceptedCommandId = 0;          // solo lo usa loop()
Preferences prefs;                           // memoria no volátil (offsets)

MotionState motionSnapshot() {
  portENTER_CRITICAL(&stateMux);
  const MotionState copy = sharedState;
  portEXIT_CRITICAL(&stateMux);
  return copy;
}
void publishMotion(const MotionState &state) {
  portENTER_CRITICAL(&stateMux);
  sharedState = state;
  portEXIT_CRITICAL(&stateMux);
}
void publishCal(const CalData &data) {
  portENTER_CRITICAL(&stateMux);
  sharedCal = data;
  portEXIT_CRITICAL(&stateMux);
}
bool takeCal(CalData &out) {
  bool ready;
  portENTER_CRITICAL(&stateMux);
  ready = sharedCal.ready;
  if (ready) { out = sharedCal; sharedCal.ready = false; }
  portEXIT_CRITICAL(&stateMux);
  return ready;
}
Pose poseSnapshot() {
  portENTER_CRITICAL(&poseMux);
  const Pose copy = sharedPose;
  portEXIT_CRITICAL(&poseMux);
  return copy;
}
// ¿La medición es reciente? Con un sensor caído no se debe mover nada.
bool poseFresh(const Pose &pose, uint32_t nowMs) {
  return pose.everValid && nowMs - pose.ms <= cfg::IMU_TIMEOUT_MS;
}
bool poseFresh(const Pose &pose) { return poseFresh(pose, millis()); }

// Mensaje de diagnóstico (empieza con '#', la interfaz lo ignora o lo registra).
void note(const char *text) { controlUart.print("# "); controlUart.println(text); }

// ═════════════════════════════════════════════════════════════════════════════
//  SENSOR BNO085
// ═════════════════════════════════════════════════════════════════════════════
BNO080 bno08x;  // librería SparkFun (sin pines INT/RST)
bool imuReady = false;
uint32_t imuLastTryMs = 0;
bool wireStarted = false;
uint32_t imuLastReportMs = 0;  // último reporte de ORIENTACIÓN recibido (solo imuTask)
uint32_t imuRestartCount = 0;  // reinicios seguidos sin datos (solo imuTask)

// Rutina de calibración del magnetómetro. loop() solo deja una petición; la ejecuta
// imuTask, que es la única tarea que toca el sensor por I2C.
enum ImuRequest : uint8_t { ImuReqNone = 0, ImuReqMagCal, ImuReqSaveCal, ImuReqEndCal };
volatile uint8_t imuRequest = ImuReqNone;  // protegida por poseMux
volatile bool magCalActive = false;        // MAGCAL en curso: el rotor no se mueve
uint32_t magCalLastMs = 0, magHighSinceMs = 0, saveStartMs = 0;  // solo imuTask
bool magCalReadyShown = false;

void requestImu(ImuRequest r) {
  portENTER_CRITICAL(&poseMux);
  imuRequest = r;
  portEXIT_CRITICAL(&poseMux);
}

const char *accLabel(uint8_t a) {
  static const char *const names[] = {"Unreliable", "Low", "Medium", "High"};
  return a < 4 ? names[a] : "?";
}

// Pide al sensor la orientación cada IMU_REPORT_US y el magnetómetro (para MagAcc).
// (La librería usa ms.)
void imuEnableReport() {
  const uint16_t ms = static_cast<uint16_t>(cfg::IMU_REPORT_US / 1000 > 0 ? cfg::IMU_REPORT_US / 1000 : 1);
  if (cfg::IMU_USE_GAME_ROTATION) bno08x.enableGameRotationVector(ms);
  else bno08x.enableRotationVector(ms);
  if (cfg::IMU_MAG_REPORT_US > 0 || magCalActive) {
    const uint32_t magUs = cfg::IMU_MAG_REPORT_US > 0 ? cfg::IMU_MAG_REPORT_US : 50000;
    bno08x.enableMagnetometer(static_cast<uint16_t>(magUs / 1000 > 0 ? magUs / 1000 : 1));
  }
}

// Guarda en la pose compartida la precisión que informa el sensor.
void updateSensorAccuracy() {
  const uint8_t f = bno08x.getQuatAccuracy();
  const uint8_t m = bno08x.getMagAccuracy();
  portENTER_CRITICAL(&poseMux);
  sharedPose.fusionAcc = f;
  sharedPose.magAcc = m;
  portEXIT_CRITICAL(&poseMux);
}

// Ejecuta las peticiones de MAGCAL / SAVECAL / ENDCAL que dejó loop().
void serviceImuRequests(uint32_t now) {
  uint8_t req;
  portENTER_CRITICAL(&poseMux);
  req = imuRequest;
  imuRequest = ImuReqNone;
  portEXIT_CRITICAL(&poseMux);
  if (req == ImuReqMagCal) {
    bno08x.calibrateMagnetometer();  // solo calibra el magnetómetro mientras dura MAGCAL
    magCalActive = true;
    magCalLastMs = 0;
    magHighSinceMs = 0;
    magCalReadyShown = false;
    imuEnableReport();
    note("magcal iniciada: mueve el sensor en figuras de 8, lejos de motores e imanes; espera Mag=High y envia SAVECAL");
  } else if (req == ImuReqSaveCal) {
    bno08x.saveCalibration();         // guarda la calibración (DCD) en la flash del sensor
    bno08x.requestCalibrationStatus();
    saveStartMs = now ? now : 1;
    note("savecal: guardando en la flash del sensor, no muevas el sensor...");
  } else if (req == ImuReqEndCal) {
    bno08x.calibrateAll();            // vuelve la calibración dinámica de los 3 sensores
    magCalActive = false;
    saveStartMs = 0;
    imuEnableReport();
    note("magcal terminada sin guardar: calibracion dinamica restaurada");
  }
}

// Avance de SAVECAL (espera la confirmación) y reporte periódico de MAGCAL.
void serviceImuCalibration(uint32_t now) {
  if (saveStartMs) {
    if (now - saveStartMs >= 300 && bno08x.calibrationComplete()) {
      saveStartMs = 0;
      note("savecal: guardado confirmado por el sensor");
      if (magCalActive) {
        bno08x.calibrateAll();  // terminar MAGCAL: vuelve la calibración dinámica de los 3 sensores
        magCalActive = false;
        imuEnableReport();
        note("magcal terminada: calibracion dinamica restaurada");
      }
    } else if (now - saveStartMs > cfg::MAGCAL_SAVE_TIMEOUT_MS) {
      saveStartMs = 0;
      note("savecal: SIN confirmacion del sensor; repite SAVECAL con el sensor quieto");
    }
  }
  if (magCalActive && now - magCalLastMs >= cfg::MAGCAL_REPORT_MS) {
    magCalLastMs = now;
    const Pose p = poseSnapshot();
    char msg[96];
    snprintf(msg, sizeof(msg), "magcal mag=%s fusion=%s err=%.1fdeg",
             accLabel(p.magAcc), accLabel(p.fusionAcc), p.accuracyDeg);
    note(msg);
    if (p.magAcc == 3) {
      if (!magHighSinceMs) magHighSinceMs = now;
      else if (!magCalReadyShown && now - magHighSinceMs >= cfg::MAGCAL_HIGH_HOLD_MS) {
        magCalReadyShown = true;
        note("magcal: magnetometro en High. Envia SAVECAL para guardar");
      }
    } else {
      magHighSinceMs = 0;
      magCalReadyShown = false;
    }
  }
}

// Convierte un quaternion en azimut/elevación de la antena y lo publica.
void publishPose(double w, double x, double y, double z, float accuracyRad) {
  double rawAz, rawEl;
  double b[3];
  portENTER_CRITICAL(&poseMux);
  b[0] = boresight[0]; b[1] = boresight[1]; b[2] = boresight[2];
  portEXIT_CRITICAL(&poseMux);
  RotorMath::pointingFromQuaternion(w, x, y, z, b[0], b[1], b[2], rawAz, rawEl);
  const uint32_t now = millis();
  portENTER_CRITICAL(&poseMux);
  sharedPose.q[0] = w; sharedPose.q[1] = x; sharedPose.q[2] = y; sharedPose.q[3] = z;
  sharedPose.rawAz = rawAz;
  sharedPose.rawEl = rawEl;
  sharedPose.az = RotorMath::normalizeAzimuth(rawAz + azOffset);
  sharedPose.el = rawEl + elOffset;
  sharedPose.accuracyDeg = accuracyRad * static_cast<float>(180.0 / M_PI);
  sharedPose.ms = now;
  sharedPose.everValid = true;
  portEXIT_CRITICAL(&poseMux);
}

// Libera un bus I2C trabado. Si el ESP32 se reinicia (o el cable falla) a mitad de una
// transacción, el BNO085 puede quedarse sujetando SDA en bajo y Wire.begin() "funciona"
// pero ya no llegan datos. Se emulan 9 pulsos de SCL en colector abierto + condición STOP.
// Llamar con Wire detenido (antes de Wire.begin).
void i2cBusRecover() {
  const int sda = cfg::IMU_SDA_PIN, scl = cfg::IMU_SCL_PIN;
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  delayMicroseconds(10);
  for (int i = 0; i < 9; ++i) {
    pinMode(scl, OUTPUT); digitalWrite(scl, LOW);  delayMicroseconds(10);
    pinMode(scl, INPUT_PULLUP);                    delayMicroseconds(10);
    if (digitalRead(sda)) break;                   // el esclavo soltó SDA
  }
  // STOP: SDA sube mientras SCL está en alto.
  pinMode(sda, OUTPUT); digitalWrite(sda, LOW);    delayMicroseconds(10);
  pinMode(scl, INPUT_PULLUP);                      delayMicroseconds(10);
  pinMode(sda, INPUT_PULLUP);                      delayMicroseconds(10);
}

// Reset por hardware del BNO085 (pin RST, activo en bajo). Sin cablear RST (-1) no hace nada.
void imuHardwareReset() {
  if (cfg::IMU_RST_PIN < 0) return;
  pinMode(cfg::IMU_RST_PIN, OUTPUT);
  digitalWrite(cfg::IMU_RST_PIN, LOW);
  delay(10);
  digitalWrite(cfg::IMU_RST_PIN, HIGH);
  delay(300);  // arranque del BNO085 tras el reset
}

// Una pasada de lectura del sensor (la llama imuTask cada ~1 ms).
// Si el sensor no está inicializado, reintenta cada segundo.
void imuPoll() {
  const uint32_t now = millis();
  if (!imuReady) {
    if (imuLastTryMs != 0 && now - imuLastTryMs < 1000) return;
    imuLastTryMs = now ? now : 1;
    if (wireStarted) Wire.end();  // reintento: liberar el bus antes de volver a iniciarlo
    wireStarted = true;
    i2cBusRecover();      // libera SDA/SCL si quedaron trabados
    imuHardwareReset();   // solo si IMU_RST_PIN está cableado
    Wire.begin(cfg::IMU_SDA_PIN, cfg::IMU_SCL_PIN);
    Wire.setClock(cfg::IMU_I2C_HZ);
    if (bno08x.begin(cfg::IMU_ADDRESS, Wire)) {
      imuEnableReport();
      imuReady = true;
      imuLastReportMs = millis();  // desde aquí corre el vigilante de datos
      note("imu responde por I2C; esperando datos de orientacion");
    } else {
      note("imu_init_failed: revisa cableado I2C (SDA/SCL/direccion)");
    }
    return;
  }
  if (bno08x.hasReset()) imuEnableReport();  // el sensor se reinició: volver a pedir datos

  serviceImuRequests(now);

  bool gotData = false;
  for (int n = 0; n < 8; ++n) {  // vaciar paquetes pendientes
    const uint16_t id = bno08x.getReadings();  // id del reporte recibido (0 = nada)
    if (id == 0) break;
    gotData = true;
    // Solo la orientación refresca la pose; el reporte del magnetómetro solo da MagAcc.
    if (id == SENSOR_REPORTID_ROTATION_VECTOR || id == SENSOR_REPORTID_GAME_ROTATION_VECTOR) {
      publishPose(bno08x.getQuatReal(), bno08x.getQuatI(), bno08x.getQuatJ(), bno08x.getQuatK(),
                  cfg::IMU_USE_GAME_ROTATION ? NAN : bno08x.getQuatRadianAccuracy());
      imuLastReportMs = millis();
      imuRestartCount = 0;
    }
  }
  if (gotData) updateSensorAccuracy();
  serviceImuCalibration(now);

  // Vigilante: begin() solo confirma que el sensor contesta por I2C, no que esté enviando
  // orientación (el comando de activación puede perderse). Sin reportes por
  // IMU_DATA_WATCHDOG_MS se rehace todo: bus, reset y begin. No se aplica mientras SAVECAL
  // escribe en la flash del sensor. (Se usa millis() fresco: 'now' es anterior a la lectura.)
  const uint32_t t = millis();
  if (!saveStartMs && t - imuLastReportMs > cfg::IMU_DATA_WATCHDOG_MS) {
    char msg[96];
    snprintf(msg, sizeof(msg), "imu_sin_datos: reiniciando I2C y sensor (intento %lu)",
             static_cast<unsigned long>(++imuRestartCount));
    note(msg);
    imuReady = false;
  }
}

// Tarea del sensor (core 0): el I2C bloquea unos cientos de µs por lectura y aquí no
// afecta a los pulsos STEP.
void imuTask(void *) {
  for (;;) {
    imuPoll();
    vTaskDelay(1);
  }
}

// Calibración manual: la antena apunta ahora a (az, el) -> se ajustan los offsets y se
// guardan en memoria no volátil. Devuelve false si no hay medición reciente.
// Con AZ_ABSOLUTE_MAGNETIC el azimut no se toca (es absoluto: rumbo magnético + declinación)
// y solo se calibra la elevación.
bool calibrate(double az, double el) {
  const Pose pose = poseSnapshot();
  if (!poseFresh(pose, millis())) return false;
  portENTER_CRITICAL(&poseMux);
  if (!cfg::AZ_ABSOLUTE_MAGNETIC) azOffset = RotorMath::normalizeAzimuth(az - pose.rawAz);
  elOffset = el - pose.rawEl;
  portEXIT_CRITICAL(&poseMux);
  if (prefs.begin("rotor", false)) {
    // Con GAME_ROTATION el azimut 0° es la orientación al encender: no se guarda el offset.
    if (!cfg::AZ_ABSOLUTE_MAGNETIC && !cfg::IMU_USE_GAME_ROTATION)
      prefs.putDouble("azOff", azOffset);
    prefs.putDouble("elOff", elOffset);
    prefs.end();
  }
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  MOTORES Y CONTROL
// ═════════════════════════════════════════════════════════════════════════════
enum class Mode : uint8_t { Idle, Goto, Track };

// Estado del controlador; solo lo toca motionTask().
// Estado de CALAX (calibración de ejes): 4 tramos — azimut ida/vuelta, elevación ida/vuelta.
struct CalRun {
  bool active = false;
  int leg = 0;               // 0 az ida, 1 az vuelta, 2 el ida, 3 el vuelta
  int phase = 0;             // 0 iniciar tramo, 1 moviendo, 2 asentando el sensor
  long startEmitted = 0;     // pasos enviados al iniciar el tramo
  long goal = 0;             // pasos del tramo (con signo)
  long forward[2] = {0, 0};  // pasos de la ida de cada eje (con signo)
  uint32_t settleUntil = 0;
  CalData data;
};

struct Controller {
  CalRun cal;
  RotorAzLimit azLimit;
  bool elHoming = false;
  bool elHomed = false;
  double elPosition = 0.0;
  uint32_t elMinChangedMs = 0, elMaxChangedMs = 0;
  Mode mode = Mode::Idle;
  double tgt[2] = {0, 0};   // objetivo (°)
  double ff[2] = {0, 0};    // velocidad del objetivo (°/s), solo en Track
  uint32_t trkMs = 0;       // millis() del último TRK
  uint32_t lastUs = 0;      // para medir dt
  Fault fault = FaultNone;

  // Por eje
  float cmd[2] = {0, 0};          // velocidad ordenada (pasos/s, con signo)
  double acc[2] = {0, 0};         // fracción de paso acumulada (ver emitSteps)
  long emitted[2] = {0, 0};       // pasos enviados en total (con signo)
  int lastAzDir = 0;              // sentido del último pulso de azimut en movimiento normal
  uint32_t lastAzStepMs = 0;
  RotorTrack::Integrator trackIntegral[2];
  // Ventana del detector de fallas
  uint32_t winMs[2] = {0, 0};
  long winSteps[2] = {0, 0};
  double winDeg[2] = {0, 0};
  bool winValid[2] = {false, false};
  double evalExpected[2] = {0, 0};  // última ventana evaluada: esperado / medido (°)
  double evalActual[2] = {0, 0};
  double evalWindowS[2] = {0, 0};
  int faultAxis = 0;
  double lastMeasEl = 0;  // última elevación medida (para el informe de falla)
  uint8_t strikes[2] = {0, 0};  // ventanas malas consecutivas por eje

  void setMode(Mode next) {
    if (mode == Mode::Track && next != Mode::Track) {
      trackIntegral[AZ].reset();
      trackIntegral[EL].reset();
    }
    mode = next;
  }
};

// Acerca `current` a `desired` cambiando a lo sumo `maxDelta` (rampa de velocidad).
float ramp(float current, float desired, float maxDelta) {
  const float d = desired - current;
  return d > maxDelta ? current + maxDelta : (d < -maxDelta ? current - maxDelta : desired);
}

double clampAbs(double x, double limit) {
  return x > limit ? limit : (x < -limit ? -limit : x);
}

// Genera los pulsos STEP de un eje a la velocidad `speed` (pasos/s, con signo).
// Se acumula la fracción de paso (speed*dt) y se emite un pulso cada vez que se
// completa uno: la velocidad media resulta exacta aunque el lazo corra a 1 ms
// (con un intervalo entero de ms, 400 pasos/s saldrían a 333).
enum class StepResult { NotDue, Emitted, SwitchChanged };

StepResult emitSteps(Controller &c, int axis, double dt, bool backingOff = false) {
  c.acc[axis] += c.cmd[axis] * dt;
  if (c.acc[axis] > 2.0) c.acc[axis] = 2.0;
  if (c.acc[axis] < -2.0) c.acc[axis] = -2.0;
  int dir = 0;
  if (c.acc[axis] >= 1.0) { dir = 1; c.acc[axis] -= 1.0; }
  else if (c.acc[axis] <= -1.0) { dir = -1; c.acc[axis] += 1.0; }
  if (dir == 0) return StepResult::NotDue;
  const AxisConfig &a = AXES[axis];
  digitalWrite(a.dirPin, ((dir > 0) != a.invertDir) ? HIGH : LOW);
  delayMicroseconds(2);  // tiempo de establecimiento de DIR antes del pulso
  // Interbloqueos independientes: nunca avanzar hacia un final de carrera activo.
  if (axis == AZ && (azLimitPressed() != backingOff)) return StepResult::SwitchChanged;
  if (axis == EL && ((dir < 0 && elMinPressed()) || (dir > 0 && elMaxPressed())))
    return StepResult::SwitchChanged;
  digitalWrite(a.stepPin, HIGH);
  delayMicroseconds(cfg::STEP_PULSE_US);
  digitalWrite(a.stepPin, LOW);
  c.emitted[axis] += dir;
  if (axis == EL && c.elHomed && !c.elHoming) c.elPosition += dir / a.stepsPerDeg;
  if (axis == AZ && !backingOff) {
    c.lastAzDir = dir;
    c.lastAzStepMs = millis();
  }
  return StepResult::Emitted;
}

// Velocidad deseada (°/s, con signo) para SET.
double gotoRate(int axis, double err) {
  const double maxDeg = AXES[axis].maxSpeed / AXES[axis].stepsPerDeg;
  // Goto: dentro de la banda muerta quieto; fuera, proporcional pero limitado por la
  // rampa de frenado v <= sqrt(2*a*distancia) para no pasarse del objetivo.
  const double absErr = err < 0 ? -err : err;
  if (absErr <= cfg::GOTO_DEADBAND_DEG) return 0;
  const double aDeg = cfg::RAMP_ACCELERATION / AXES[axis].stepsPerDeg;  // °/s²
  const double brake = sqrt(2.0 * aDeg * (absErr - cfg::GOTO_DEADBAND_DEG));
  const double lim = brake < maxDeg ? brake : maxDeg;
  return clampAbs(cfg::CTRL_KP * err, lim);
}

double trackRate(Controller &c, int axis, double err, double ff, double dt,
                 RotorTrack::RateRange allowed) {
  const AxisConfig &a = AXES[axis];
  const double currentRate = c.cmd[axis] / a.stepsPerDeg;
  const double rampDelta = cfg::RAMP_ACCELERATION * dt / a.stepsPerDeg;
  return c.trackIntegral[axis].update(ff + cfg::CTRL_KP * err, a.trackKi * err * dt,
                                     cfg::TRK_MAX_INTEGRAL_DEG_S, allowed, currentRate, rampDelta);
}

// Detector de fallas de un eje (ver cabecera). Devuelve true si detecta una falla.
bool checkAxisFault(Controller &c, int axis, double meas, bool measOk, uint32_t nowMs) {
  if (!cfg::FAULT_DETECTION) return false;
  if (!measOk) { c.winValid[axis] = false; c.strikes[axis] = 0; return false; }  // sin medición fiable: reiniciar
  if (!c.winValid[axis]) {  // abrir ventana
    c.winValid[axis] = true;
    c.winMs[axis] = nowMs;
    c.winSteps[axis] = c.emitted[axis];
    c.winDeg[axis] = meas;
    return false;
  }
  if (nowMs - c.winMs[axis] < cfg::FAULT_WINDOW_MS) return false;
  const double expected = (c.emitted[axis] - c.winSteps[axis]) / AXES[axis].stepsPerDeg;
  const double actual = axis == AZ ? RotorMath::wrap180(meas - c.winDeg[axis])
                                   : meas - c.winDeg[axis];
  c.evalWindowS[axis] = (nowMs - c.winMs[axis]) / 1000.0;
  c.evalExpected[axis] = expected;
  c.evalActual[axis] = actual;
  c.winMs[axis] = nowMs;  // siguiente ventana
  c.winSteps[axis] = c.emitted[axis];
  c.winDeg[axis] = meas;
  const double absExpected = expected < 0 ? -expected : expected;
  if (absExpected < cfg::FAULT_MIN_DEG) return false;  // casi no se movió: no cambia las rachas
  // Movimiento medido en el sentido esperado; negativo = se movió al revés.
  const double along = actual * (expected < 0 ? -1.0 : 1.0);
  if (along >= cfg::FAULT_MIN_RATIO * absExpected) { c.strikes[axis] = 0; return false; }
  return ++c.strikes[axis] >= cfg::FAULT_STRIKES;
}

// Termina CALAX con error y deja constancia para loop().
void abortCal(Controller &c, const char *reason) {
  c.cal.active = false;
  c.cal.data.ready = true;
  c.cal.data.aborted = true;
  c.cal.data.reason = reason;
  publishCal(c.cal.data);
}

void sampleAzLimit(Controller &c, bool pressed) {
  const uint32_t nowMs = millis();
  const bool wasTripped = c.azLimit.tripped();
  const bool recentStep = nowMs - c.lastAzStepMs <= cfg::AZ_LIMIT_APPROACH_MS;
  const bool motionActive = c.mode != Mode::Idle || c.cal.active;
  const int approachDir = motionActive && recentStep && c.fault == FaultNone ? c.lastAzDir : 0;
  c.azLimit.sample(pressed, nowMs, approachDir, cfg::AZ_LIMIT_RELEASE_MS);
  if (wasTripped == c.azLimit.tripped()) return;

  if (c.cal.active) abortCal(c, "final de carrera de azimut activo");
  c.setMode(Mode::Idle);
  c.cmd[AZ] = c.cmd[EL] = 0;
  c.acc[AZ] = c.acc[EL] = 0;
  c.lastAzDir = 0;
  c.winValid[AZ] = c.winValid[EL] = false;
  c.strikes[AZ] = c.strikes[EL] = 0;
  xQueueReset(commandQueue);
}

void backOffAzLimit(Controller &c, double dt) {
  const int dir = c.azLimit.backoffDirection();
  if (dir != 0) {
    const float speed = fminf(cfg::AZ_LIMIT_BACKOFF_SPEED, AXES[AZ].maxSpeed);
    c.cmd[AZ] = ramp(c.cmd[AZ], dir * speed, static_cast<float>(cfg::RAMP_ACCELERATION * dt));
    switch (emitSteps(c, AZ, dt, true)) {
      case StepResult::Emitted:
        c.azLimit.recordBackoffStep(cfg::AZ_LIMIT_BACKOFF_MAX_STEPS);
        break;
      case StepResult::SwitchChanged:
        sampleAzLimit(c, false);
        break;
      case StepResult::NotDue:
        break;
    }
  }
  if (c.azLimit.backoffDirection() == 0) {
    c.cmd[AZ] = 0;
    c.acc[AZ] = 0;
  }
}

// CALAX, un paso: mueve el eje del tramo actual en lazo abierto, espera a que el sensor se
// asiente y fotografía su quaternion al inicio y al fin de cada ida. Rellena `desired`.
void calStep(Controller &c, const Pose &pose, uint32_t nowMs, double desired[2]) {
  CalRun &k = c.cal;
  const int axis = k.leg < 2 ? AZ : EL;
  const bool outbound = (k.leg % 2) == 0;

  if (k.phase == 0) {                         // iniciar el tramo
    if (c.cmd[AZ] != 0 || c.cmd[EL] != 0) return;  // esperar a que el rotor esté quieto
    if (outbound) {
      std::memcpy(k.data.q[axis == AZ ? 0 : 2], pose.q, sizeof(pose.q));
      int sign = 1;
      if (axis == AZ && !cfg::AZ_SHORTEST_PATH && pose.az + cfg::CAL_SWEEP_DEG > 360.0) sign = -1;
      if (axis == EL) {
        if (pose.el + cfg::CAL_SWEEP_DEG <= cfg::EL_MAX_DEG) sign = 1;
        else if (pose.el - cfg::CAL_SWEEP_DEG >= cfg::EL_MIN_DEG) sign = -1;
        else { abortCal(c, "sin recorrido de elevacion suficiente (mueve la antena a una elevacion intermedia)"); return; }
      }
      k.goal = sign * lround(cfg::CAL_SWEEP_DEG * AXES[axis].stepsPerDeg);
    } else {
      k.goal = -k.forward[axis];              // volver al punto de partida
    }
    k.startEmitted = c.emitted[axis];
    k.phase = 1;
    return;
  }

  if (k.phase == 1) {                         // moviendo a velocidad constante con rampa
    const long done = c.emitted[axis] - k.startEmitted;
    const long remaining = k.goal - done;
    const double v = cfg::CAL_SPEED_STEPS;
    const double decelDist = v * v / (2.0 * cfg::RAMP_ACCELERATION) + 2.0;
    const double dist = remaining < 0 ? -static_cast<double>(remaining) : remaining;
    if (dist > decelDist) desired[axis] = remaining > 0 ? v : -v;
    if (desired[axis] == 0 && c.cmd[axis] == 0) {   // frenó: esperar asentamiento
      k.phase = 2;
      k.settleUntil = nowMs + cfg::CAL_SETTLE_MS;
    }
    return;
  }

  if (nowMs < k.settleUntil) return;          // fase 2: asentando
  if (outbound) {
    k.forward[axis] = c.emitted[axis] - k.startEmitted;  // pasos realmente enviados
    std::memcpy(k.data.q[axis == AZ ? 1 : 3], pose.q, sizeof(pose.q));
    (axis == AZ ? k.data.azCmdDeg : k.data.elCmdDeg) = k.forward[axis] / AXES[axis].stepsPerDeg;
  }
  k.leg++;
  k.phase = 0;
  if (k.leg >= 4) {                           // terminado
    k.active = false;
    k.data.ready = true;
    k.data.aborted = false;
    publishCal(k.data);
  }
}

// Un paso del lazo de control (cada ~1 ms).
void controlStep(Controller &c, uint32_t nowUs) {
  double dt = (nowUs - c.lastUs) / 1e6;
  c.lastUs = nowUs;
  if (dt <= 0) return;
  if (dt > 0.05) dt = 0.05;  // tras una pausa larga no dar un salto de velocidad
  if (c.azLimit.tripped()) {
    backOffAzLimit(c, dt);
    return;
  }

  const Pose pose = poseSnapshot();
  // La otra tarea puede publicar una muestra entre dos lecturas de reloj.
  // Tomar la hora después de copiar la muestra evita una edad negativa sin signo.
  const uint32_t nowMs = millis();
  const bool fresh = poseFresh(pose, nowMs);
  const double meas[2] = {pose.az, c.elPosition};

  // Homing de elevación: búsqueda lenta del switch inferior, sin depender del BNO085.
  if (c.elHoming) {
    c.cmd[AZ] = 0; c.acc[AZ] = 0;
    if (elMinPressed()) {
      c.cmd[EL] = 0; c.acc[EL] = 0;
      c.elHoming = false; c.elHomed = true;
      c.elPosition = cfg::EL_LIMIT_MIN_DEG;
      c.emitted[EL] = 0;
      note("HOMEEL: limite inferior detectado; elevacion referenciada");
    } else if (labs(c.emitted[EL]) > static_cast<long>(120.0 * AXES[EL].stepsPerDeg)) {
      c.cmd[EL] = 0; c.acc[EL] = 0; c.elHoming = false;
      note("HOMEEL: no se encontro el limite inferior tras 120 deg; revisa cableado del switch (GPIO 32) y sentido (EL_INVERT_DIRECTION)");
    } else {
      c.cmd[EL] = cfg::EL_HOME_DIRECTION * cfg::EL_HOME_SPEED;
      emitSteps(c, EL, dt);
    }
    return;
  }
  // El límite superior fija el extremo y detiene cualquier intento de seguir subiendo.
  if (c.elHomed && elMaxPressed() && c.cmd[EL] > 0) {
    c.cmd[EL] = 0; c.acc[EL] = 0; c.elPosition = cfg::EL_LIMIT_MAX_DEG;
  }
  if (c.elHomed && elMinPressed() && c.cmd[EL] < 0) {
    c.cmd[EL] = 0; c.acc[EL] = 0; c.elPosition = cfg::EL_LIMIT_MIN_DEG;
  }

  if (c.mode == Mode::Track && nowMs - c.trkMs > cfg::TRK_TIMEOUT_MS)
    c.setMode(Mode::Idle);  // dejaron de llegar TRK: frenar y quedarse
  // Si el BNO085 pierde datos, se inhibe solo el azimut. La elevación es independiente
  // y puede seguir usando su posición por pasos siempre que esté referenciada.

  if (c.cal.active && !fresh) abortCal(c, "el sensor dejo de responder");

  double desired[2] = {0, 0};  // pasos/s
  if (c.cal.active) {
    calStep(c, pose, nowMs, desired);   // CALAX: movimiento en lazo abierto
  } else if ((fresh || c.elHomed) && c.fault == FaultNone && c.mode != Mode::Idle) {
    const bool tracking = c.mode == Mode::Track;
    const double age = tracking ? (nowMs - c.trkMs) / 1000.0 : 0.0;
    for (int i = 0; i < 2; ++i) {
      const AxisConfig &a = AXES[i];
      if ((i == AZ && !fresh) || (i == EL && !c.elHomed)) { desired[i] = 0; continue; }
      const double maxRate = a.maxSpeed / a.stepsPerDeg;
      double ff = c.ff[i];
      const double ref = c.tgt[i] + ff * age;  // objetivo extrapolado (Track)
      RotorTrack::RateRange allowed{-maxRate, maxRate};
      double err;
      if (i == AZ) {
        err = cfg::AZ_SHORTEST_PATH ? RotorMath::wrap180(ref - meas[AZ])
                                    : RotorMath::normalizeAzimuth(ref) - meas[AZ];
        if (!cfg::AZ_SHORTEST_PATH) {
          if (meas[AZ] <= 0) allowed.min = 0;
          if (meas[AZ] >= 360) allowed.max = 0;
        }
      } else {
        const double limitedRef = ref < cfg::EL_MIN_DEG ? cfg::EL_MIN_DEG
                                  : ref > cfg::EL_MAX_DEG ? cfg::EL_MAX_DEG : ref;
        err = limitedRef - meas[EL];
        if ((ref <= cfg::EL_MIN_DEG && ff < 0) || (ref >= cfg::EL_MAX_DEG && ff > 0)) ff = 0;
        if (meas[EL] <= cfg::EL_MIN_DEG) allowed.min = 0;
        if (meas[EL] >= cfg::EL_MAX_DEG) allowed.max = 0;
      }
      const double rate = tracking ? trackRate(c, i, err, ff, dt, allowed)
                                   : allowed.clamp(gotoRate(i, err));
      desired[i] = rate * a.stepsPerDeg;
    }
  }

  const float maxDelta = static_cast<float>(cfg::RAMP_ACCELERATION * dt);
  for (int i = 0; i < 2; ++i) {
    c.cmd[i] = ramp(c.cmd[i], static_cast<float>(desired[i]), maxDelta);
    if (emitSteps(c, i, dt) == StepResult::SwitchChanged) {
      if (i == AZ) sampleAzLimit(c, true);
      else {
        c.cmd[EL] = 0; c.acc[EL] = 0;
        if (elMinPressed()) c.elPosition = cfg::EL_LIMIT_MIN_DEG;
        if (elMaxPressed()) c.elPosition = cfg::EL_LIMIT_MAX_DEG;
      }
      return;
    }
  }

  // Detector de fallas. El azimut no se evalúa cerca del cenit (indefinido). Durante CALAX se
  // mueve en lazo abierto a propósito y se desactiva.
  c.lastMeasEl = meas[EL];
  const bool azOk = fresh && meas[EL] < cfg::FAULT_AZ_MAX_EL_DEG;
  if (c.cal.active) {
    c.winValid[AZ] = c.winValid[EL] = false; c.strikes[AZ] = c.strikes[EL] = 0;
  } else if (c.fault == FaultNone) {
    if (checkAxisFault(c, AZ, meas[AZ], azOk, nowMs)) c.fault = FaultAz;
    /* Elevación: posición por pasos referenciada con HOMEEL; no comparar con quaternion. */
    if (c.fault != FaultNone) {  // frenado inmediato; STOP la borra
      c.faultAxis = c.fault == FaultAz ? AZ : EL;
      c.setMode(Mode::Idle);
      c.cmd[AZ] = c.cmd[EL] = 0;
    }
  }
}

// Tarea de los motores (core 1). Recibe órdenes de la cola y ejecuta el lazo cada
// ~1 ms. Es la ÚNICA tarea que genera pulsos STEP.
void motionTask(void *) {
  MotionState state;
  Controller ctl;
  ctl.lastUs = micros();

  for (;;) {
    sampleAzLimit(ctl, azLimitPressed());
    MotionCommand cmd;
    if (xQueueReceive(commandQueue, &cmd, 0) == pdTRUE &&
        (!ctl.azLimit.tripped() || cmd.type == CommandType::Stop)) {
      state.activeCommandId = cmd.id;
      switch (cmd.type) {
        case CommandType::CalAxes:
          if (!ctl.cal.active && ctl.mode == Mode::Idle) {
            ctl.cal = CalRun();
            ctl.cal.active = true;
          }
          break;
        case CommandType::Target:
          if (ctl.cal.active) break;  // ocupado calibrando (loop ya lo rechaza)
          ctl.setMode(Mode::Goto);
          ctl.tgt[AZ] = cmd.az;  ctl.tgt[EL] = cmd.el;
          ctl.ff[AZ] = ctl.ff[EL] = 0;
          break;
        case CommandType::Track:
          if (ctl.cal.active) break;
          ctl.setMode(Mode::Track);
          ctl.tgt[AZ] = cmd.az;  ctl.tgt[EL] = cmd.el;
          ctl.ff[AZ] = cmd.vaz;  ctl.ff[EL] = cmd.vel;
          ctl.trkMs = cmd.timeMs;
          break;
        case CommandType::HomeEl:
          if (!ctl.cal.active) {
            ctl.setMode(Mode::Idle);
            ctl.elHomed = false;
            ctl.elHoming = true;
            ctl.emitted[EL] = 0;
            ctl.cmd[EL] = 0; ctl.acc[EL] = 0;
            ctl.winValid[EL] = false; ctl.strikes[EL] = 0;
            note("HOMEEL: buscando limite inferior; mantén despejada la trayectoria");
          }
          break;
        case CommandType::Stop:
          if (ctl.cal.active) abortCal(ctl, "interrumpida por STOP");
          ctl.elHoming = false;     // STOP también cancela la búsqueda de elevación
          ctl.cmd[EL] = 0; ctl.acc[EL] = 0;
          ctl.setMode(Mode::Idle);  // frena con la rampa
          ctl.fault = FaultNone;  // STOP borra la falla
          ctl.winValid[AZ] = ctl.winValid[EL] = false; ctl.strikes[AZ] = ctl.strikes[EL] = 0;
          ctl.azLimit.cancelBackoff();
          break;
      }
    }

    controlStep(ctl, micros());
    state.moving = ctl.cmd[AZ] != 0 || ctl.cmd[EL] != 0;
    state.calibrating = ctl.cal.active;
    state.elHomed = ctl.elHomed;
    state.elPosition = ctl.elPosition;
    state.moving = state.moving || ctl.elHoming;
    state.azLimitTrips = ctl.azLimit.trips();
    state.fault = ctl.fault;
    if (ctl.azLimit.tripped())
      state.fault = ctl.azLimit.canBackOff() ? FaultAzLimit : FaultAzLimitBlocked;
    if (ctl.fault != FaultNone) {
      state.faultExpectedDeg = static_cast<float>(ctl.evalExpected[ctl.faultAxis]);
      state.faultMeasuredDeg = static_cast<float>(ctl.evalActual[ctl.faultAxis]);
      state.faultWindowS = static_cast<float>(ctl.evalWindowS[ctl.faultAxis]);
      state.faultElDeg = static_cast<float>(ctl.lastMeasEl);
    }
    publishMotion(state);
    vTaskDelay(1);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  BATERÍA
// ═════════════════════════════════════════════════════════════════════════════
// Voltaje de la batería a través de un divisor resistivo (ver RotorConfig.h).
// La lectura NO bloquea: loop() toma 1 muestra cada BAT_SAMPLE_PERIOD_MS y, al
// juntar BAT_SAMPLES, guarda el promedio.
float batteryVolts = NAN;  // último promedio disponible (NAN hasta tener uno); solo loop()

void pollBattery() {
  static uint32_t sum = 0, lastSampleAt = 0;
  static unsigned int count = 0;
  const uint32_t now = millis();
  if (now - lastSampleAt < cfg::BAT_SAMPLE_PERIOD_MS) return;
  lastSampleAt = now;
  sum += analogReadMilliVolts(cfg::BAT_PIN);  // usa la calibración de fábrica del ESP32
  if (++count >= cfg::BAT_SAMPLES) {
    const double mv = static_cast<double>(sum) / count;       // mV en el pin
    batteryVolts = static_cast<float>(mv / 1000.0 * cfg::BAT_DIVIDER);  // V en la batería
    sum = 0;
    count = 0;
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  PROTOCOLO SERIAL
// ═════════════════════════════════════════════════════════════════════════════
// Encola una orden para motionTask(). Devuelve false si la cola está llena.
bool queueCommand(CommandType type, double az, double el, double vaz = 0, double vel = 0) {
  uint32_t nextId = lastAcceptedCommandId + 1;
  if (nextId == 0) nextId = 1;  // el id 0 significa "ninguna orden aún"
  const MotionCommand cmd{type, az, el, vaz, vel, nextId, millis()};
  if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE) return false;
  lastAcceptedCommandId = nextId;
  return true;
}

void uartError(const char *error) {
  controlUart.print("ERR,");
  controlUart.println(error);
}

// Escribe una línea solo si cabe en el buffer de salida: un lector serial lento no
// debe bloquear la lectura de comandos.
void sendLine(const char *message, int length) {
  if (length > 0 && controlUart.availableForWrite() >= length)
    controlUart.write(reinterpret_cast<const uint8_t *>(message), length);
}

// Envía "POS,<az>,<el>\n" con la posición MEDIDA. Sin medición reciente no se envía:
// así la interfaz ve la posición como obsoleta en vez de creer un dato viejo.
void reportPosition() {
  const Pose pose = poseSnapshot();
  const MotionState st = motionSnapshot();
  if (!poseFresh(pose, millis()) || !st.elHomed) {
    static uint32_t lastWhyMs = 0;
    if (millis() - lastWhyMs >= 5000) {
      lastWhyMs = millis();
      char why[96];
      snprintf(why, sizeof(why), "sin POS: imu=%s el_homed=%s az_limit=%s el_min=%s el_max=%s",
               poseFresh(pose, millis()) ? "ok" : "SIN_DATOS", st.elHomed ? "si" : "NO (envia HOMEEL)",
               azLimitPressed() ? "ACTIVO" : "libre", elMinPressed() ? "ACTIVO" : "libre",
               elMaxPressed() ? "ACTIVO" : "libre");
      note(why);
    }
    return;
  }
  char message[72];
  const int length = cfg::POS_INCLUDE_ACCURACY
      ? snprintf(message, sizeof(message), "POS,%.2f,%.2f,%u,%.1f,%u\n", pose.az, st.elPosition,
                 pose.fusionAcc, pose.accuracyDeg, pose.magAcc)
      : snprintf(message, sizeof(message), "POS,%.2f,%.2f\n", pose.az, st.elPosition);
  if (length > 0 && length < static_cast<int>(sizeof(message))) sendLine(message, length);
}

// Envía "BAT,<V>\n" (omite el envío si aún no hay medición).
void reportBattery() {
  if (!isfinite(batteryVolts)) return;
  char message[24];
  const int length = snprintf(message, sizeof(message), "BAT,%.2f\n", batteryVolts);
  if (length > 0 && length < static_cast<int>(sizeof(message))) sendLine(message, length);
}

// Si el movimiento se declaró en falla, avisa una vez por la interfaz.
void reportFaults() {
  static Fault reported = FaultNone;
  const MotionState st = motionSnapshot();
  const Fault now = st.fault;
  if (now == reported) return;
  if (isAzLimitFault(reported) && !isAzLimitFault(now))
    note("final de carrera de azimut liberado; STOP automatico");
  reported = now;
  if (isAzLimitFault(now)) {
    uartError(AZ_LIMIT_ERROR);
    note(now == FaultAzLimit ? "final de carrera de azimut activo; retroceso automatico"
                             : "final de carrera de azimut bloqueado; rotor detenido");
  } else if (now != FaultNone) {
    const bool az = now == FaultAz;
    uartError(az ? "fault_az" : "fault_el");
    // Motivo: comparar lo esperado (pasos enviados) con lo medido por el sensor.
    const double exp = st.faultExpectedDeg, got = st.faultMeasuredDeg;
    const double along = got * (exp < 0 ? -1.0 : 1.0);  // medido en el sentido esperado
    const double ratio = exp != 0 ? along / (exp < 0 ? -exp : exp) : 0;
    const char *why = along < 0 ? "se movio en sentido CONTRARIO (revisa INVERT del motor o la orientacion del sensor)"
                                : "casi no se movio (driver sin energia/ENABLE, corriente baja, correa suelta, "
                                  "motor trabado, o el sensor no sigue al eje)";
    char msg[220];
    snprintf(msg, sizeof(msg), "fault %s: esperado=%+.2f medido=%+.2f deg en %.1fs (avance=%.0f%%, minimo=%.0f%%, %u ventanas seguidas) el=%.1f",
             az ? "az" : "el", exp, got, st.faultWindowS, ratio * 100.0, cfg::FAULT_MIN_RATIO * 100.0,
             static_cast<unsigned>(cfg::FAULT_STRIKES), st.faultElDeg);
    note(msg);
    snprintf(msg, sizeof(msg), "motivo: %s", why);
    note(msg);
    note("motores detenidos; corrige y envia STOP");
  }
}

// Procesa el resultado de una CALAX: calcula, informa y guarda el eje de la antena corregido.
// Se hace aquí (core 1, fuera de la tarea de motores) porque escribe en memoria no volátil.
void processCalibration() {
  CalData d;
  if (!takeCal(d)) return;
  char msg[140];
  if (d.aborted) {
    snprintf(msg, sizeof(msg), "calax abortada: %s", d.reason);
    note(msg);
    return;
  }
  const double kDeg = 180.0 / M_PI;
  double angAz, axAz[3], angEl, axEl[3];
  if (!RotorMath::relativeRotation(d.q[0], d.q[1], angAz, axAz) ||
      !RotorMath::relativeRotation(d.q[2], d.q[3], angEl, axEl)) {
    note("calax: el sensor no detecto giro en un eje; revisa motores, drivers y montaje");
    return;
  }
  // Giro real vs pedido -> pasos por grado que corresponderian.
  const double azCmd = fabs(d.azCmdDeg), elCmd = fabs(d.elCmdDeg);
  snprintf(msg, sizeof(msg), "calax az: pedido %.2f medido %.2f grados (x%.3f) -> AZ_STEPS_PER_DEG %.2f (actual %.2f)",
           azCmd, angAz, angAz / azCmd, cfg::AZ_STEPS_PER_DEG * azCmd / angAz, cfg::AZ_STEPS_PER_DEG);
  note(msg);
  snprintf(msg, sizeof(msg), "calax el: pedido %.2f medido %.2f grados (x%.3f) -> EL_STEPS_PER_DEG %.2f (actual %.2f)",
           elCmd, angEl, angEl / elCmd, cfg::EL_STEPS_PER_DEG * elCmd / angEl, cfg::EL_STEPS_PER_DEG);
  note(msg);

  // Eje de la antena: proyectar el actual al plano perpendicular a la bisagra de elevación.
  double b[3];
  portENTER_CRITICAL(&poseMux);
  b[0] = boresight[0]; b[1] = boresight[1]; b[2] = boresight[2];
  portEXIT_CRITICAL(&poseMux);
  const double bn0 = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
  for (double &x : b) x /= bn0;
  const double dot = b[0] * axEl[0] + b[1] * axEl[1] + b[2] * axEl[2];
  double nb[3] = {b[0] - dot * axEl[0], b[1] - dot * axEl[1], b[2] - dot * axEl[2]};
  const double nn = sqrt(nb[0] * nb[0] + nb[1] * nb[1] + nb[2] * nb[2]);
  if (nn < 0.5) {
    note("calax: BORESIGHT casi paralelo a la bisagra de elevacion; ponlo hacia adelante en RotorConfig.h");
    return;
  }
  for (double &x : nb) x /= nn;
  snprintf(msg, sizeof(msg), "calax: el eje de la antena estaba %.1f grados fuera del plano de giro de la elevacion; corregido",
           asin(fabs(dot > 1 ? 1 : dot)) * kDeg);
  note(msg);

  // Geometría del mecanismo: ejes az/el perpendiculares (ideal 90) e inclinación de la base.
  const double hv = fabs(axEl[0] * axAz[0] + axEl[1] * axAz[1] + axEl[2] * axAz[2]);
  double vw[3];
  RotorMath::rotateVector(d.q[0][0], d.q[0][1], d.q[0][2], d.q[0][3], axAz[0], axAz[1], axAz[2], vw);
  snprintf(msg, sizeof(msg), "calax: ejes az/el a %.1f grados entre si (ideal 90); inclinacion de la base %.1f grados",
           acos(hv > 1 ? 1 : hv) * kDeg, acos(fabs(vw[2]) > 1 ? 1 : fabs(vw[2])) * kDeg);
  note(msg);

  // ¿El sentido de giro coincide? Con el eje corregido, los pasos positivos deben subir la
  // elevación y aumentar el azimut.
  double az0, el0, az1, el1, azA, elA, azB, elB;
  RotorMath::pointingFromQuaternion(d.q[2][0], d.q[2][1], d.q[2][2], d.q[2][3], nb[0], nb[1], nb[2], az0, el0);
  RotorMath::pointingFromQuaternion(d.q[3][0], d.q[3][1], d.q[3][2], d.q[3][3], nb[0], nb[1], nb[2], az1, el1);
  RotorMath::pointingFromQuaternion(d.q[0][0], d.q[0][1], d.q[0][2], d.q[0][3], nb[0], nb[1], nb[2], azA, elA);
  RotorMath::pointingFromQuaternion(d.q[1][0], d.q[1][1], d.q[1][2], d.q[1][3], nb[0], nb[1], nb[2], azB, elB);
  if ((el1 - el0) * d.elCmdDeg < 0) note("calax: la ELEVACION gira al reves de lo esperado -> invierte EL_INVERT_DIRECTION");
  if (elA < cfg::FAULT_AZ_MAX_EL_DEG && RotorMath::wrap180(azB - azA) * d.azCmdDeg < 0)
    note("calax: el AZIMUT gira al reves de lo esperado -> invierte AZ_INVERT_DIRECTION");

  // Aplicar y guardar el eje corregido. El offset de elevación anterior era relativo al eje
  // antiguo: se anula y hay que volver a fijar el cero con CAL.
  portENTER_CRITICAL(&poseMux);
  boresight[0] = nb[0]; boresight[1] = nb[1]; boresight[2] = nb[2];
  elOffset = 0;
  portEXIT_CRITICAL(&poseMux);
  if (prefs.begin("rotor", false)) {
    prefs.putDouble("bx", nb[0]); prefs.putDouble("by", nb[1]); prefs.putDouble("bz", nb[2]);
    prefs.putDouble("elOff", 0.0);
    prefs.end();
  }
  note("calax lista. Mide la elevacion real de la antena (inclinometro o un objeto conocido) y envia CAL,0,<elevacion real>");
}

// Comprueba que se pueden aceptar SET/TRK; si no, responde el error y devuelve false.
bool motionAllowed() {
  const MotionState st = motionSnapshot();
  if (isAzLimitFault(st.fault) || azLimitPressed()) { uartError(AZ_LIMIT_ERROR); return false; }
  if (st.calibrating || magCalActive) { uartError("busy"); return false; }
  if (st.fault != FaultNone) { uartError("fault"); return false; }
  if (!st.elHomed) { uartError("el_not_homed"); return false; }
  if (!poseFresh(poseSnapshot())) { uartError("no_imu"); return false; }
  return true;
}

// Interpreta una línea completa (sin '\n'). Responde ACK o ERR,<motivo>.
void handleCommand(char *line) {
  // Recorta espacios al inicio y al final.
  char *text = const_cast<char *>(RotorMath::skipSpaces(line));
  char *end = text + strlen(text);
  while (end > text && isspace(static_cast<unsigned char>(end[-1]))) --end;
  *end = '\0';
  if (!*text) return;

  if (strncmp(text, "SET,", 4) == 0) {
    double v[2];
    if (!RotorMath::parseList(text + 4, v, 2)) {
      uartError("formato");
    } else if (!RotorMath::validTarget(v[0], v[1], cfg::EL_MIN_DEG, cfg::EL_MAX_DEG)) {
      uartError("invalid_target");
    } else if (motionAllowed()) {
      if (queueCommand(CommandType::Target, RotorMath::normalizeAzimuth(v[0]), v[1]))
        controlUart.println("ACK");
      else
        uartError("queue_full");
    }
  } else if (strncmp(text, "TRK,", 4) == 0) {
    // Llega a ~5 Hz: no se responde ACK. Si la cola está llena se descarta
    // en silencio; el siguiente TRK llega en 200 ms.
    double v[4];
    if (!RotorMath::parseList(text + 4, v, 4)) {
      uartError("formato");
    } else if (!RotorMath::validTarget(v[0], v[1], cfg::EL_MIN_DEG, cfg::EL_MAX_DEG) ||
               fabs(v[2]) > 360.0 || fabs(v[3]) > 360.0) {
      uartError("invalid_target");
    } else {
      static RotorTrackAfterLimit trackStream;
      const MotionState st = motionSnapshot();
      const bool limitActive = isAzLimitFault(st.fault) || azLimitPressed();
      if (!trackStream.accept(millis(), st.azLimitTrips, limitActive, cfg::TRK_TIMEOUT_MS))
        uartError(limitActive ? AZ_LIMIT_ERROR : "track_wait");
      else if (motionAllowed())
        queueCommand(CommandType::Track, RotorMath::normalizeAzimuth(v[0]), v[1], v[2], v[3]);
    }
  } else if (strcmp(text, "HOMEEL") == 0) {
    const MotionState st = motionSnapshot();
    if (isAzLimitFault(st.fault) || azLimitPressed()) uartError(AZ_LIMIT_ERROR);  // motionTask lo descartaría
    else if (st.moving || st.calibrating) uartError("busy");
    else if (queueCommand(CommandType::HomeEl, 0, 0)) controlUart.println("ACK");
    else uartError("queue_full");
  } else if (strcmp(text, "STOP") == 0) {
    if (queueCommand(CommandType::Stop, 0, 0)) controlUart.println("ACK");
    else uartError("queue_full");
  } else if (strncmp(text, "CAL,", 4) == 0 || strcmp(text, "ZERO") == 0) {
    uartError("cal_disabled_step_elevation");
    return;
    double v[2] = {0, 0};  // ZERO = CAL,0,0
    const bool isCal = text[0] == 'C';
    if (isCal && !RotorMath::parseList(text + 4, v, 2)) {
      uartError("formato");
    } else if (!RotorMath::validTarget(v[0], v[1], -90.0, 90.0)) {
      uartError("invalid_target");
    } else if (motionSnapshot().moving) {
      uartError("busy");  // calibrar en movimiento haría saltar el control
    } else if (calibrate(v[0], v[1])) {
      controlUart.println("ACK");
      if (cfg::AZ_ABSOLUTE_MAGNETIC) note("azimut absoluto (norte magnetico + declinacion): solo se calibro la elevacion");
    } else {
      uartError("no_imu");
    }
  } else if (strcmp(text, "CALAX") == 0) {
    uartError("calax_disabled_step_elevation");
    return;
    const MotionState st = motionSnapshot();
    if (isAzLimitFault(st.fault) || azLimitPressed()) uartError(AZ_LIMIT_ERROR);
    else if (!poseFresh(poseSnapshot())) uartError("no_imu");
    else if (st.moving || st.calibrating || st.fault != FaultNone || magCalActive) uartError("busy");
    else if (queueCommand(CommandType::CalAxes, 0, 0)) {
      controlUart.println("ACK");
      note("calax iniciada (~30 s): el rotor se movera +-grados en azimut y elevacion; STOP la aborta");
    } else uartError("queue_full");
  } else if (strncmp(text, "BORE,", 5) == 0) {
    // BORE,x,y,z: fija la direccion "adelante" de la antena en los ejes del sensor y la guarda
    // (reemplaza lo que dejo CALAX o RotorConfig.h). Anula el offset de elevacion: repetir CAL.
    double v[3];
    if (!RotorMath::parseList(text + 5, v, 3)) {
      uartError("formato");
    } else {
      const double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      if (!(n > 1e-6)) { uartError("invalid_target"); }
      else if (motionSnapshot().moving || motionSnapshot().calibrating) { uartError("busy"); }
      else {
        portENTER_CRITICAL(&poseMux);
        for (int i = 0; i < 3; ++i) boresight[i] = v[i] / n;
        elOffset = 0;
        portEXIT_CRITICAL(&poseMux);
        if (prefs.begin("rotor", false)) {
          prefs.putDouble("bx", v[0] / n); prefs.putDouble("by", v[1] / n); prefs.putDouble("bz", v[2] / n);
          prefs.putDouble("elOff", 0.0);
          prefs.end();
        }
        boresightCalibrated = true;
        controlUart.println("ACK");
        note("boresight guardado; offset de elevacion a 0: ejecuta CALAX y luego CAL,0,<elevacion real>");
      }
    }
  } else if (strcmp(text, "MAGCAL") == 0) {
    const MotionState st = motionSnapshot();
    if (!poseFresh(poseSnapshot())) uartError("no_imu");
    else if (st.moving || st.calibrating || st.fault != FaultNone || magCalActive) uartError("busy");
    else {
      magCalActive = true;  // desde ya no se aceptan SET/TRK/CALAX
      requestImu(ImuReqMagCal);
      controlUart.println("ACK");
    }
  } else if (strcmp(text, "SAVECAL") == 0) {
    if (!poseFresh(poseSnapshot())) uartError("no_imu");
    else if (motionSnapshot().moving) uartError("busy");
    else { requestImu(ImuReqSaveCal); controlUart.println("ACK"); }
  } else if (strcmp(text, "ENDCAL") == 0) {
    requestImu(ImuReqEndCal);
    controlUart.println("ACK");
  } else if (strcmp(text, "ACC") == 0) {
    const Pose p = poseSnapshot();
    char msg[112];
    snprintf(msg, sizeof(msg), "acc fusion=%s(%u) err=%.1fdeg mag=%s(%u)",
             accLabel(p.fusionAcc), p.fusionAcc, p.accuracyDeg, accLabel(p.magAcc), p.magAcc);
    note(msg);
  } else if (strcmp(text, "GET") == 0) {
    reportPosition();
  } else if (strcmp(text, "IMU") == 0) {
    // Diagnóstico de montaje/calibración: az/el medidos, valores crudos, precisión y offsets.
    const Pose p = poseSnapshot();
    char msg[160];
    snprintf(msg, sizeof(msg),
             "imu az=%.2f el=%.2f raw_az=%.2f raw_el=%.2f acc=%.1fdeg age=%lums off_az=%.2f off_el=%.2f",
             p.az, p.el, p.rawAz, p.rawEl, p.accuracyDeg,
             static_cast<unsigned long>(p.everValid ? millis() - p.ms : 0), azOffset, elOffset);
    note(msg);
    double bs[3];
    portENTER_CRITICAL(&poseMux);
    bs[0] = boresight[0]; bs[1] = boresight[1]; bs[2] = boresight[2];
    portEXIT_CRITICAL(&poseMux);
    snprintf(msg, sizeof(msg), "imu boresight=(%.3f,%.3f,%.3f) azimut=%s declinacion=%.2f", bs[0], bs[1], bs[2],
             cfg::AZ_ABSOLUTE_MAGNETIC ? "absoluto(magnetico+decl)" : "relativo(CAL)", cfg::MAG_DECLINATION_DEG);
    note(msg);
    snprintf(msg, sizeof(msg), "imu precision: fusion=%s mag=%s err=%.1fdeg", accLabel(p.fusionAcc),
             accLabel(p.magAcc), p.accuracyDeg);
    note(msg);
  } else if (strcmp(text, "HELP") == 0) {
    note("SET,<az>,<el> | TRK,<az>,<el>,<vaz>,<vel> | CAL,<az>,<el> | ZERO | CALAX | BORE,<x>,<y>,<z> | MAGCAL | SAVECAL | ENDCAL | ACC | GET | HOMEEL | STOP | IMU | HELP");
  } else {
    uartError("unknown_command");
  }
}

// Lee los bytes disponibles y arma líneas. Una línea defectuosa (muy larga,
// con caracteres inválidos o incompleta por timeout) se descarta y se avisa con ERR.
// No bloquea: procesa como máximo UART_BYTES_PER_LOOP bytes por llamada.
void pollUart() {
  static char line[cfg::UART_LINE_CAPACITY];
  static size_t used = 0;
  static const char *discardReason = nullptr;  // != nullptr: ignorando hasta '\n'
  static uint32_t lastByteAt = 0;

  const uint32_t now = millis();
  if (used > 0 && now - lastByteAt >= cfg::UART_LINE_TIMEOUT_MS) {
    used = 0;
    discardReason = "line_timeout";
  }

  for (unsigned int n = 0;
       n < cfg::UART_BYTES_PER_LOOP && controlUart.available() > 0; ++n) {
    const int received = controlUart.read();
    if (received < 0) break;
    const char ch = static_cast<char>(received);
    lastByteAt = millis();

    if (ch == '\n') {  // fin de línea: ejecutar (o reportar el descarte)
      if (discardReason) uartError(discardReason);
      else { line[used] = '\0'; handleCommand(line); }
      used = 0;
      discardReason = nullptr;
    } else if (ch != '\r' && !discardReason) {
      const unsigned char u = static_cast<unsigned char>(ch);
      if ((u < 32 && ch != '\t') || u > 126) {
        used = 0;
        discardReason = "invalid_character";
      } else if (used + 1 >= sizeof(line)) {
        used = 0;
        discardReason = "line_too_long";
      } else {
        line[used++] = ch;
      }
    }
  }
}
}  // namespace

// ── Arduino ──────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(cfg::UART_BAUD);
  if (cfg::UART_USE_SERIAL2)
    Serial2.begin(cfg::UART_BAUD, SERIAL_8N1, cfg::UART2_RX_PIN, cfg::UART2_TX_PIN);

  // Pines STEP/DIR de ambos ejes (sin AccelStepper hay que configurarlos aquí).
  for (const AxisConfig &a : AXES) {
    pinMode(a.stepPin, OUTPUT);
    pinMode(a.dirPin, OUTPUT);
    digitalWrite(a.stepPin, LOW);
  }
  pinMode(cfg::AZ_LIMIT_PIN, INPUT_PULLUP);
  pinMode(cfg::EL_LIMIT_MIN_PIN, cfg::EL_LIMIT_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  pinMode(cfg::EL_LIMIT_MAX_PIN, cfg::EL_LIMIT_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  analogReadResolution(12);
  analogSetPinAttenuation(cfg::BAT_PIN, ADC_11db);  // rango del pin ~0-3.3 V

  // Offsets del sensor guardados por CAL (con GAME_ROTATION no se usan: el azimut 0°
  // es la orientación al encender).
  if (prefs.begin("rotor", true)) {
    // Azimut absoluto: siempre rumbo magnético + declinación (no se usa ningún offset guardado).
    if (!cfg::AZ_ABSOLUTE_MAGNETIC && !cfg::IMU_USE_GAME_ROTATION)
      azOffset = prefs.getDouble("azOff", cfg::MAG_DECLINATION_DEG);
    elOffset = prefs.getDouble("elOff", 0.0);
    // Eje de la antena afinado por CALAX (si existe).
    const double bx = prefs.getDouble("bx", boresight[0]);
    const double by = prefs.getDouble("by", boresight[1]);
    const double bz = prefs.getDouble("bz", boresight[2]);
    const double n = sqrt(bx * bx + by * by + bz * bz);
    if (n > 0.5) { boresight[0] = bx / n; boresight[1] = by / n; boresight[2] = bz / n; }
    boresightCalibrated = prefs.isKey("bx");
    prefs.end();
  }

  commandQueue = xQueueCreate(cfg::COMMAND_QUEUE_LENGTH, sizeof(MotionCommand));
  if (!commandQueue ||
      xTaskCreatePinnedToCore(motionTask, "rotor-motion", cfg::MOTION_TASK_STACK_BYTES,
                              nullptr, 2, nullptr, cfg::MOTION_TASK_CORE) != pdPASS ||
      xTaskCreatePinnedToCore(imuTask, "rotor-imu", cfg::IMU_TASK_STACK_BYTES,
                              nullptr, 1, nullptr, cfg::IMU_TASK_CORE) != pdPASS) {
    note("FATAL: no se pudieron iniciar las tareas; no se aceptan comandos");
    for (;;) delay(1000);
  }
  note("Rotor listo: azimut con BNO085; elevacion por pasos y finales de carrera. Ejecuta HOMEEL antes de mover.");
  note("CALAX deshabilitado: la elevacion ya no se calibra con quaternion; ajusta EL_STEPS_PER_DEG y angulos de switches.");
  if (!boresightCalibrated)
    note("AVISO: eje de la antena sin calibrar. Un sensor mal alineado acopla azimut y elevacion y puede dar ERR,fault_*; ejecuta CALAX antes de mover en lazo cerrado");
}

void loop() {
  pollUart();
  pollBattery();
  reportFaults();
  processCalibration();

  // Envío periódico de la posición y del voltaje de batería a la interfaz.
  static uint32_t lastPositionAt = 0, lastBatteryAt = 0;
  const uint32_t now = millis();
  if (now - lastPositionAt >= cfg::POS_PERIOD_MS) {
    lastPositionAt = now;
    reportPosition();
  }
  if (now - lastBatteryAt >= cfg::BAT_PERIOD_MS) {
    lastBatteryAt = now;
    reportBattery();
  }
  delay(1);
}