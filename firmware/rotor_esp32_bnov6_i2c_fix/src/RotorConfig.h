// RotorConfig.h — todos los parámetros ajustables del rotor en un solo lugar.
// Si cambias mecánica, drivers, pines o el montaje del sensor, solo debería ser
// necesario editar este archivo.
#pragma once

#include <limits.h>
#include <stdint.h>

namespace RotorConfig {

// ───────────────────────── Cableado STEP/DIR ─────────────────────────
// Los drivers deben estar alimentados y habilitados externamente (pin ENABLE).
constexpr uint8_t AZ_DIR_PIN  = 23;
constexpr uint8_t AZ_STEP_PIN = 22;
constexpr uint8_t EL_DIR_PIN  = 21;
constexpr uint8_t EL_STEP_PIN = 19;
constexpr uint8_t AZ_LIMIT_PIN = 32;          // INPUT_PULLUP, LOW = final de carrera activo
constexpr uint32_t AZ_LIMIT_RELEASE_MS = 30;  // HIGH estable antes del STOP automático
constexpr uint32_t AZ_LIMIT_APPROACH_MS = 250;  // antigüedad máxima del último paso al detectar LOW
constexpr double AZ_LIMIT_BACKOFF_MAX_DEG = 3.0;
constexpr float AZ_LIMIT_BACKOFF_SPEED = 100.0f;  // micropasos/s
// Sentido de giro. Con lazo cerrado un sentido invertido NO es solo "gira al revés":
// el control empujaría en la dirección equivocada. El detector de fallas (más abajo)
// lo detecta y detiene los motores con ERR,fault_az / ERR,fault_el; si pasa en la
// primera prueba, cambia el valor de INVERT del eje.
constexpr bool AZ_INVERT_DIRECTION = true;
constexpr bool EL_INVERT_DIRECTION = true;
constexpr unsigned int STEP_PULSE_US = 3;    // ancho mínimo del pulso STEP

// ───────────────────────── Mecánica ─────────────────────────
// Pasos por grado del eje de salida = pasos_motor * microstepping * reducción / 360.
// Con lazo cerrado el valor solo necesita ser aproximado (el sensor corrige el
// resto), pero determina la velocidad en °/s y el detector de fallas.
constexpr double AZ_FULL_STEPS_PER_REV = 200.0;
constexpr double EL_FULL_STEPS_PER_REV = 200.0;
constexpr unsigned int AZ_MICROSTEPS = 32;
constexpr unsigned int EL_MICROSTEPS = 32;
constexpr double AZ_GEAR_RATIO = 5.0;  // vueltas del motor por vuelta del eje
constexpr double EL_GEAR_RATIO = 5.0;
constexpr double AZ_STEPS_PER_DEG =
    AZ_FULL_STEPS_PER_REV * AZ_MICROSTEPS * AZ_GEAR_RATIO / 360.0;
constexpr double EL_STEPS_PER_DEG =
    EL_FULL_STEPS_PER_REV * EL_MICROSTEPS * EL_GEAR_RATIO / 360.0;
constexpr unsigned AZ_LIMIT_BACKOFF_MAX_STEPS =
    static_cast<unsigned>(AZ_LIMIT_BACKOFF_MAX_DEG * AZ_STEPS_PER_DEG);
static_assert(AZ_LIMIT_BACKOFF_MAX_STEPS > 0 && AZ_LIMIT_BACKOFF_SPEED > 0,
              "El retroceso debe permitir al menos un paso");

// ───────────────────────── Dinámica ─────────────────────────
// Velocidad máxima (micropasos/s). Con 88.9 pasos/° son 400 pasos/s ≈ 4.5 °/s.
// El generador de pasos emite como máximo 1 paso por ms, así que el tope es 1000.
constexpr float AZ_MAX_SPEED = 400.0f;
constexpr float EL_MAX_SPEED = 400.0f;
// Rampa de velocidad (micropasos/s²) para arrancar y frenar, en todos los modos.
// También define cuánto antes de llegar se empieza a frenar. 300 ≈ 3.4 °/s².
constexpr float RAMP_ACCELERATION = 300.0f;

// Límites de elevación (grados), iguales a rotor_config.py de la interfaz.
constexpr double EL_MIN_DEG = 0.0;
constexpr double EL_MAX_DEG = 90.0;

// true : el azimut toma el camino más corto cruzando el norte (mecanismo giro libre).
// false: recorrido acotado 0..360° absolutos, sin cruzar el norte.
constexpr bool AZ_SHORTEST_PATH = true;

// ───────────────────────── Sensor BNO085 (I2C) ─────────────────────────
// OJO: el I2C por defecto del ESP32 (GPIO 21/22) lo usan STEP/DIR, por eso se usan otros pines.
constexpr int IMU_SDA_PIN = 25;
constexpr int IMU_SCL_PIN = 26;
constexpr uint32_t IMU_I2C_HZ = 50000;
constexpr uint8_t IMU_ADDRESS = 0x4B;      // 0x4B si el pin SA0/DI está en alto
constexpr uint32_t IMU_REPORT_US = 10000;  // un dato cada 10 ms (100 Hz)
// Vigilante: si el sensor contesta por I2C pero no llega ningún reporte de orientación en este
// tiempo, se reinicia el bus y el sensor (begin() solo prueba que responde, no que transmita).
constexpr uint32_t IMU_DATA_WATCHDOG_MS = 1500;
// GPIO conectado al pin RST del BNO085 (reset por hardware en cada reintento). -1 = sin cablear.
// Recomendado cablearlo (p. ej. GPIO 27, libre): es la forma más fiable de recuperar el sensor.
constexpr int IMU_RST_PIN = -1;
// Sin datos del sensor por este tiempo -> los motores frenan y se deja de enviar POS.
constexpr uint32_t IMU_TIMEOUT_MS = 300;

// Precisión del sensor: estado de calibración del magnetómetro (MagAcc) y de la fusión.
// Niveles: 0=Unreliable 1=Low 2=Medium 3=High. El magnetómetro se pide a este período solo
// para conocer MagAcc (0 = no pedirlo; MagAcc quedaría en 0).
constexpr uint32_t IMU_MAG_REPORT_US = 50000;  // 20 Hz
// true : POS,<az>,<el>,<fusionAcc 0-3>,<error_deg>,<magAcc 0-3>  (campos extra al final).
// false: POS,<az>,<el> (protocolo original; la interfaz no necesita cambios).
// ACC e IMU siempre muestran la precisión, sea cual sea este valor.
constexpr bool POS_INCLUDE_ACCURACY = false;

// Rutina de calibración del magnetómetro (comandos MAGCAL / SAVECAL / ENDCAL).
constexpr uint32_t MAGCAL_REPORT_MS = 500;        // período del reporte "# magcal ..."
constexpr uint32_t MAGCAL_HIGH_HOLD_MS = 3000;    // tiempo en Mag=High para avisar que está listo
constexpr uint32_t MAGCAL_SAVE_TIMEOUT_MS = 5000; // espera máxima de la confirmación de SAVECAL

// Tipo de orientación:
//  false: ROTATION_VECTOR. Usa magnetómetro: el azimut es absoluto (norte magnético),
//         no deriva, pero los imanes/hierro de motores y estructura lo distorsionan.
//         Calibra el magnetómetro en el lugar y lejos de los motores.
//  true : GAME_ROTATION_VECTOR. Sin magnetómetro: inmune a campos magnéticos, pero el
//         azimut 0° es la dirección al encender (hay que enviar CAL tras cada encendido)
//         y puede derivar lentamente.
constexpr bool IMU_USE_GAME_ROTATION = false;

// Eje de la antena (boresight, hacia donde apunta) en el marco del SENSOR: valor inicial
// aproximado (ver el montaje real abajo).
// Debe apuntar aproximadamente hacia adelante. El comando CALAX lo afina para que sea
// exactamente perpendicular a la bisagra de elevación y guarda el resultado; ese valor
// guardado reemplaza a este.
// Montaje real: la parte superior del sensor (eje +Y) apunta en la direccion de la antena y el
// eje X apunta a la IZQUIERDA de la antena (entonces Z queda hacia abajo). "Adelante" = +Y.
// X coincide con la bisagra de elevacion, por eso no sirve como boresight.
// (Si cambias el montaje o ya corriste CALAX con otro valor, usa BORE,x,y,z por UART.)
constexpr double BORESIGHT_X = 0.0;
constexpr double BORESIGHT_Y = 1.0;
constexpr double BORESIGHT_Z = 0.0;

// Referencia del azimut.
//  true : azimut ABSOLUTO respecto al norte magnético, sin calibraciones manuales. El
//         firmware informa y controla el azimut en NORTE VERDADERO (el que calcula Skyfield),
//         que es el rumbo magnético del sensor + MAG_DECLINATION_DEG. CAL/ZERO no cambian el
//         azimut (solo la elevación) y no se guarda ningún offset de azimut.
//  false: el azimut 0° se define con CAL (offset guardado), útil con GAME_ROTATION.
// Requiere IMU_USE_GAME_ROTATION = false (el magnetómetro es lo que da el norte).
constexpr bool AZ_ABSOLUTE_MAGNETIC = true;

// Declinación magnética del lugar en grados, positiva hacia el este:
//   norte verdadero = norte magnético + declinación.
// Calcúlala para tu ubicación y fecha (p. ej. con el calculador del NOAA/NCEI) y escríbela aquí;
// con 0.0 el azimut queda en norte magnético y el modelo propagado (norte verdadero) no coincidirá.
// Valor del lugar de uso: 0.08 deg E (+-0.37 deg), variando 0.18 deg W por año (NOAA/NCEI).
// La deriva anual es menor que la incertidumbre del modelo: no hace falta actualizarlo.
constexpr double MAG_DECLINATION_DEG = 0.08;
constexpr double EL_OFFSET = -82;

// ───────────────────────── Calibración de ejes (comando CALAX) ─────────────────────────
// CALAX mueve cada eje ±CAL_SWEEP_DEG con los motores (lazo abierto, despejar el rotor) y mide
// con el sensor alrededor de qué eje giró y cuánto. Con eso corrige el eje de la antena
// (BORESIGHT) para que sea perpendicular a la bisagra de elevación y reporta si
// los pasos/° configurados coinciden con el giro real. Tarda ~30 s.
constexpr double CAL_SWEEP_DEG = 20.0;
constexpr float CAL_SPEED_STEPS = 300.0f;       // velocidad del barrido (pasos/s)
constexpr uint32_t CAL_SETTLE_MS = 1500;        // espera tras cada tramo para que el sensor se asiente

// ───────────────────────── Lazo de control ─────────────────────────
// SET : va a la posición y la mantiene (corrigiendo si el sensor ve que se desvió).
// TRK : velocidad = vsat + CTRL_KP * error + corrección integral.
constexpr double CTRL_KP = 2.0;            // 1/s, ganancia sobre el error de posición
constexpr double TRK_KI_AZ = 0.8;          // 1/s², 0 desactiva la integral de azimut
constexpr double TRK_KI_EL = 0.8;          // 1/s², 0 desactiva la integral de elevación
constexpr double TRK_MAX_INTEGRAL_DEG_S = 1.5;  // °/s, corrección máxima por eje
constexpr double GOTO_DEADBAND_DEG = 0.5;  // error aceptado en SET; no bajar de la
                                           // precisión del sensor (≈1-2° con magnetómetro)
constexpr uint32_t TRK_TIMEOUT_MS = 1500;  // sin TRK por este tiempo -> frenar y mantener

// ───────────────────────── Detector de fallas (seguridad) ─────────────────────────
// Cada FAULT_WINDOW_MS se compara cuánto se debió mover un eje según los pasos enviados
// con cuánto se movió según el sensor. Si se movió menos de FAULT_MIN_RATIO de lo esperado
// (o al revés) se declara falla y se detienen ambos ejes hasta recibir STOP. Cubre sentido
// invertido, motor sin energía, correa suelta o sensor mal montado.
// Ajustado para ser permisivo: el azimut del magnetómetro es ruidoso y con retardo, así que
// solo se declara falla si el eje NO avanza (o va al revés) durante varias ventanas seguidas.
constexpr bool FAULT_DETECTION = true;     // false = desactivar el detector por completo
constexpr uint32_t FAULT_WINDOW_MS = 4000;
constexpr double FAULT_MIN_DEG = 4.0;      // movimiento esperado mínimo para evaluar
constexpr double FAULT_MIN_RATIO = 0.10;   // avance medido mínimo (fracción de lo esperado)
constexpr uint8_t FAULT_STRIKES = 2;       // ventanas malas consecutivas para declarar falla
// Cerca del cenit el azimut es poco fiable: sobre esta elevación no se evalúa el azimut.
constexpr double FAULT_AZ_MAX_EL_DEG = 80.0;

// ───────────────────────── Lectura de batería ─────────────────────────
// Divisor resistivo: batería -> R1 -> BAT_PIN -> R2 -> GND.
// BAT_PIN debe ser un pin ADC1 (GPIO 32-39). Con atenuación 11 dB el ADC mide hasta
// ~3.3 V en el pin, es decir ≈ 18 V de batería con 100k/22k (factor 5.545).
constexpr uint8_t BAT_PIN = 34;
constexpr double BAT_R1_OHM = 100000.0;
constexpr double BAT_R2_OHM = 22000.0;
constexpr double BAT_DIVIDER = (BAT_R1_OHM + BAT_R2_OHM) / BAT_R2_OHM;
constexpr unsigned int BAT_SAMPLES = 64;         // lecturas promediadas por medición
constexpr uint32_t BAT_SAMPLE_PERIOD_MS = 2;     // separación entre lecturas
constexpr uint32_t BAT_PERIOD_MS = 1000;         // cada cuánto se envía BAT,<V>

// ───────────────────────── Enlace serial con la interfaz ─────────────────────────
// Por defecto se usa el USB (UART0, Serial). Para otro dispositivo UART a 3.3 V,
// pon UART_USE_SERIAL2 = true y conecta RX/TX más masa común.
constexpr bool UART_USE_SERIAL2 = false;
constexpr int UART2_RX_PIN = 16;
constexpr int UART2_TX_PIN = 17;
constexpr uint32_t UART_BAUD = 115200;          // debe coincidir con la interfaz
constexpr uint32_t POS_PERIOD_MS = 200;         // cada cuánto se envía POS,az,el
constexpr uint32_t UART_LINE_TIMEOUT_MS = 1000; // descarta líneas incompletas
constexpr unsigned int UART_LINE_CAPACITY = 96; // largo máximo de una línea
constexpr unsigned int UART_BYTES_PER_LOOP = 96;

// ───────────────────────── Tareas (FreeRTOS) ─────────────────────────
// Motores: tarea propia (core 1) a 1 kHz, independiente de la lectura serial.
// Sensor: tarea propia en el core 0 (sin WiFi está libre) para que las lecturas I2C
// no retrasen los pulsos STEP.
constexpr int MOTION_TASK_CORE = 1;
constexpr uint32_t MOTION_TASK_STACK_BYTES = 4096;
constexpr int IMU_TASK_CORE = 0;
constexpr uint32_t IMU_TASK_STACK_BYTES = 8192;
constexpr unsigned int COMMAND_QUEUE_LENGTH = 8;

static_assert(AZ_STEPS_PER_DEG > 0 && EL_STEPS_PER_DEG > 0,
              "Pasos/vuelta, microsteps y reducción deben ser positivos");
static_assert(AZ_MAX_SPEED > 0 && EL_MAX_SPEED > 0 &&
                  AZ_MAX_SPEED <= 1000 && EL_MAX_SPEED <= 1000,
              "La tarea de 1 ms emite como máximo 1 paso/ms (1000 pasos/s)");
static_assert(RAMP_ACCELERATION > 0, "RAMP_ACCELERATION debe ser positiva");
static_assert(EL_MIN_DEG <= 0 && EL_MAX_DEG >= 0 && EL_MIN_DEG < EL_MAX_DEG,
              "Los límites de elevación deben incluir 0");
static_assert(BORESIGHT_X != 0 || BORESIGHT_Y != 0 || BORESIGHT_Z != 0,
              "El eje de la antena (BORESIGHT) no puede ser nulo");
static_assert(!(AZ_ABSOLUTE_MAGNETIC && IMU_USE_GAME_ROTATION),
              "El azimut absoluto necesita el magnetómetro: IMU_USE_GAME_ROTATION debe ser false");
static_assert(CAL_SWEEP_DEG > 1 && CAL_SPEED_STEPS > 0 && CAL_SPEED_STEPS <= AZ_MAX_SPEED &&
                  CAL_SPEED_STEPS <= EL_MAX_SPEED, "Parámetros de CALAX inválidos");
static_assert(BAT_SAMPLES > 0 && BAT_R2_OHM > 0, "Parámetros de batería inválidos");
static_assert(CTRL_KP > 0 && GOTO_DEADBAND_DEG >= 0, "Parámetros de control inválidos");
static_assert(TRK_KI_AZ >= 0 && TRK_KI_EL >= 0 && TRK_MAX_INTEGRAL_DEG_S >= 0,
              "Parámetros de la integral de seguimiento inválidos");
static_assert(FAULT_WINDOW_MS > 0 && FAULT_MIN_DEG > 0 && FAULT_MIN_RATIO > 0 &&
                  FAULT_MIN_RATIO < 1, "Parámetros del detector de fallas inválidos");
}  // namespace RotorConfig
