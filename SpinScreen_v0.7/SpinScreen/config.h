// ============================================================
//  SpinScreen config: всё, что относится к твоему железу, здесь.
//  Номера пинов = номера GPIO, подписанные на плате ESP32-S3 Super Mini.
// ============================================================
#pragma once

#define FW_VERSION "0.7.2"

// ---------- Раскладка пинов ----------
// LAYOUT_WIRES:      экран, кнопки и мотор на проводах (как на схеме в README).
// LAYOUT_BREADBOARD: экран и джойстик втыкаются в макетку прямо рядом с ESP32.
//   У платы с одной стороны 5V GND 3V3 13 12 11 10 9 8, с другой TX RX 1 2 3 4 5 6 7.
//   Экран встаёт со стороны TX RX 1..4: его SCL в ряд TX, SDA в ряд RX, RES 1, DC 2, CS 3,
//   BLK 4; его GND и VCC оказываются в двух рядах перед платой, туда 2 провода от GND и 3V3.
//   Джойстик встаёт со стороны 5V GND 3V3: GND в ряд GND, +5V в ряд 3V3, VRx 13, VRy 11, SW 12.
//   Связь с Pi только по USB-кабелю (TX и RX заняты экраном).
#define LAYOUT_WIRES 1
#define LAYOUT_BREADBOARD 2
#define PIN_LAYOUT LAYOUT_WIRES

// ---------- Экран ST7789 (SPI) ----------
#if PIN_LAYOUT == LAYOUT_BREADBOARD
//  экран  ->  ESP32-S3 (тот же ряд макетки)
//  GND    ->  GND   (провод)
//  VCC    ->  3V3   (провод)
//  SCL    ->  TX  (GPIO43)
//  SDA    ->  RX  (GPIO44)
//  RES    ->  1
//  DC     ->  2
//  CS     ->  3
//  BLK    ->  4
#define PIN_TFT_SCL 43
#define PIN_TFT_SDA 44
#define PIN_TFT_RES 1
#define PIN_TFT_DC  2
#define PIN_TFT_CS  3
#define PIN_TFT_BLK 4
#else
//  экран  ->  ESP32-S3
//  GND    ->  GND
//  VCC    ->  3V3   (не 5V!)
//  SCL    ->  12
//  SDA    ->  11
//  RES    ->  8
//  DC     ->  9
//  CS     ->  10    (у квадратных 240x240 часто нет CS: тогда PIN_TFT_CS -1 и TFT_SPI_MODE 3)
//  BLK    ->  7     (подсветка, яркость через PWM)
#define PIN_TFT_SCL 12
#define PIN_TFT_SDA 11
#define PIN_TFT_RES 8
#define PIN_TFT_DC  9
#define PIN_TFT_CS  10
#define PIN_TFT_BLK 7
#endif

// Какой у тебя экран. Интерфейс сам перестроится под размер.
#define PANEL_170x320 1  // 1.9" прямоугольный (лёжа: 320 x 170)
#define PANEL_240x240 2  // 1.3" / 1.54" квадратный
#define PANEL_240x320 3  // 2.0" / 2.4" / 2.8" (лёжа: 320 x 240), например GMT024-08-SPI8P
#define TFT_PANEL PANEL_170x320

#define TFT_ROTATION 1       // 170x320 и 240x320: 1 или 3 (180°). 240x240: 0, 1, 2 или 3
#if PIN_LAYOUT == LAYOUT_BREADBOARD
#define TFT_SPI_HZ 40000000  // макетка: проверено 40 МГц; если по экрану мусор или полосы, верни 20000000
#else
#define TFT_SPI_HZ 40000000  // 40 МГц надёжно на проводах. Можно попробовать 80000000
#endif
#define TFT_SPI_MODE 0       // 0 или 3; 3 для модулей без вывода CS и для части 240x320
#define TFT_INVERT 1         // IPS-экраны обычно с инверсией. Если цвета "негатив": 0
// Сдвиг картинки считается сам. Если всё равно полоса мусора или картинка съехала,
// раскомментируй и подбери (обычно 0, 35 или 80):
// #define TFT_OFFSET_X 0
// #define TFT_OFFSET_Y 35

#if TFT_PANEL == PANEL_240x240
#define DISPLAY_W 240
#define DISPLAY_H 240
#elif TFT_PANEL == PANEL_240x320
#define DISPLAY_W 320
#define DISPLAY_H 240
#else
#define DISPLAY_W 320
#define DISPLAY_H 170
#endif

// ---------- Управление ----------
// INPUT_BUTTONS: 3 кнопки (вверх / вниз / OK). Кнопка: одна нога на пин, другая на GND.
//   Долгое нажатие OK = назад.
// INPUT_ENCODER: крутилка KY-040 (CLK, DT, SW). Нажать = OK, долго = назад.
// INPUT_JOYSTICK: джойстик KY-023 (5 пинов: GND, +5V, VRx, VRy, SW).
//   Вверх / вниз = по списку, вправо или нажать = OK, влево или долго нажать = назад.
#define INPUT_BUTTONS 1
#define INPUT_ENCODER 2
#define INPUT_JOYSTICK 3
#define INPUT_MODE INPUT_BUTTONS

#define PIN_BTN_UP   4
#define PIN_BTN_DOWN 5
#define PIN_BTN_OK   6

#define PIN_ENC_A  4   // CLK
#define PIN_ENC_B  5   // DT
#define PIN_ENC_SW 6   // SW
#define ENC_STEPS_PER_DETENT 4  // если на один щелчок прыгает на 2 пункта: поставь 2→4, или 4→2

// Центр джойстика запоминается при включении: в этот момент ручку не трогать.
#if PIN_LAYOUT == LAYOUT_BREADBOARD
// Джойстик со стороны 5V GND 3V3. Если его пины встают только в обратном порядке
// (SW первым от USB), поставь JOY_MIRRORED 1: тогда он идёт в ряды 12 11 10 9 8,
// а питание ему дают сами пины 9 (3,3 В) и 8 (0 В), ему нужно меньше 1 мА.
#define JOY_MIRRORED 0
#if JOY_MIRRORED
//  SW -> 12, VRy -> 11, VRx -> 10, +5V -> 9, GND -> 8
#define PIN_JOY_GND 8
#define PIN_JOY_VCC 9
#define PIN_JOY_X   10
#define PIN_JOY_Y   11
#define PIN_JOY_SW  12
#else
//  GND -> GND, +5V -> 3V3, VRx -> 13, VRy -> 11, SW -> 12
#define PIN_JOY_GND -1
#define PIN_JOY_VCC -1
#define PIN_JOY_X   13  // VRx: аналоговый вход (ADC2, Wi-Fi не используется)
#define PIN_JOY_Y   11  // VRy: аналоговый вход (ADC2)
#define PIN_JOY_SW  12  // SW: кнопка (нажатие на ручку)
#endif
#else
//  джойстик  ->  ESP32-S3
//  GND       ->  GND
//  +5V       ->  3V3   (не 5V! иначе на входы ESP придёт больше 3,3 В)
//  VRx       ->  4
//  VRy       ->  5
//  SW        ->  6
#define PIN_JOY_GND -1  // -1: джойстик питается от GND и 3V3 платы
#define PIN_JOY_VCC -1
#define PIN_JOY_X   4   // VRx: аналоговый вход (ADC1)
#define PIN_JOY_Y   5   // VRy: аналоговый вход (ADC1)
#define PIN_JOY_SW  6   // SW: кнопка (нажатие на ручку)
#endif
#define JOY_SWAP_XY  0  // 1, если вверх/вниз срабатывает при наклоне вбок
#define JOY_INVERT_Y 0  // 1, если вверх и вниз перепутаны
#define JOY_INVERT_X 0  // 1, если OK и назад перепутаны

// ---------- Шаговый мотор (драйвер TMC2209 / A4988 в режиме STEP/DIR) ----------
#if PIN_LAYOUT == LAYOUT_BREADBOARD
#define PIN_STEP 5
#define PIN_DIR  6
#define PIN_EN   7    // EN у драйвера: LOW = мотор включён (-1: EN драйвера на GND)
#else
#define PIN_STEP 1
#define PIN_DIR  2
#define PIN_EN   13   // EN у драйвера: LOW = мотор включён
#endif
// STEP, DIR и EN должны быть GPIO 0..31 (шаги генерируются прямой записью в регистр)

// Значения по умолчанию. Потом всё меняется прямо в меню «Мотор» и запоминается.
#define MOTOR_STEPS_PER_REV 200  // обычный NEMA17 = 200 шагов (1.8°)
#define MOTOR_MICROSTEPS 16      // как настроен драйвер (TMC2209: перемычки MS1/MS2)
#define MOTOR_GEAR 1.0f          // передаточное число (1.0 = флакон прямо на валу), меняется в Сервис → Мотор
#define MOTOR_ACCEL_RPM_S 6.0f   // плавный разгон: об/мин в секунду

#define RPM_MIN 1.0f
#define RPM_MAX 30.0f
#define RPM_DEFAULT 4.0f        // SpinSlicer по умолчанию: 360 кадров при 24 Гц = 4 об/мин

// ---------- Связь с Raspberry Pi ----------
// Работают оба способа, одновременно (прошивка сама отвечает туда, где Pi):
//  * USB-кабель: USB-C платы в любой USB-порт Pi;
//  * 4 провода к гребёнке Pi (TX и RX накрест):
//      ESP RX (GPIO44)  <-  Pi пин 8  (GPIO14, TX)
//      ESP TX (GPIO43)  ->  Pi пин 10 (GPIO15, RX)
//      ESP GND          --  Pi пин 6  (GND)
//      ESP 5V           <-  Pi пин 2  (5V)   (питание платы, если нет USB-кабеля)
#if PIN_LAYOUT == LAYOUT_BREADBOARD
#define PIN_PI_RX -1    // TX и RX заняты экраном: с Pi только USB-кабель
#define PIN_PI_TX -1
#else
#define PIN_PI_RX 44
#define PIN_PI_TX 43
#endif
#define SERIAL_BAUD 115200
#define PI_TIMEOUT_MS 3000      // если от Pi нет сообщений столько времени: "Pi не подключена"
