// ============================================================
//  SpinScreen config: всё, что относится к твоему железу, здесь.
//  Номера пинов = номера GPIO, подписанные на плате ESP32-S3 Super Mini.
// ============================================================
#pragma once

#define FW_VERSION "0.7.0"

// ---------- Экран ST7789 (SPI) ----------
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

// Какой у тебя экран. Интерфейс сам перестроится под размер.
#define PANEL_170x320 1  // 1.9" прямоугольный (лёжа: 320 x 170)
#define PANEL_240x240 2  // 1.3" / 1.54" квадратный
#define TFT_PANEL PANEL_170x320

#define TFT_ROTATION 1       // 170x320: 1 или 3 (180°). 240x240: 0, 1, 2 или 3
#define TFT_SPI_HZ 40000000  // 40 МГц надёжно на проводах. Можно попробовать 80000000
#define TFT_SPI_MODE 0       // 0 обычно; 3 для модулей 240x240 без вывода CS
#define TFT_INVERT 1         // IPS-экраны обычно с инверсией. Если цвета "негатив": 0
// Сдвиг картинки считается сам. Если всё равно полоса мусора или картинка съехала,
// раскомментируй и подбери (обычно 0, 35 или 80):
// #define TFT_OFFSET_X 0
// #define TFT_OFFSET_Y 35

#if TFT_PANEL == PANEL_240x240
#define DISPLAY_W 240
#define DISPLAY_H 240
#else
#define DISPLAY_W 320
#define DISPLAY_H 170
#endif

// ---------- Управление ----------
// INPUT_BUTTONS: 3 кнопки (вверх / вниз / OK). Кнопка: одна нога на пин, другая на GND.
//   Долгое нажатие OK = назад.
// INPUT_ENCODER: крутилка KY-040 (CLK, DT, SW). Нажать = OK, долго = назад.
#define INPUT_BUTTONS 1
#define INPUT_ENCODER 2
#define INPUT_MODE INPUT_BUTTONS

#define PIN_BTN_UP   4
#define PIN_BTN_DOWN 5
#define PIN_BTN_OK   6

#define PIN_ENC_A  4   // CLK
#define PIN_ENC_B  5   // DT
#define PIN_ENC_SW 6   // SW
#define ENC_STEPS_PER_DETENT 4  // если на один щелчок прыгает на 2 пункта: поставь 2→4, или 4→2

// ---------- Шаговый мотор (драйвер TMC2209 / A4988 в режиме STEP/DIR) ----------
#define PIN_STEP 1
#define PIN_DIR  2
#define PIN_EN   13   // EN у драйвера: LOW = мотор включён
// STEP, DIR и EN должны быть GPIO 0..31 (шаги генерируются прямой записью в регистр)

// Значения по умолчанию. Потом всё меняется прямо в меню «Мотор» и запоминается.
#define MOTOR_STEPS_PER_REV 200  // обычный NEMA17 = 200 шагов (1.8°)
#define MOTOR_MICROSTEPS 16      // как настроен драйвер (TMC2209: перемычки MS1/MS2)
#define MOTOR_GEAR 1.0f          // передаточное число (1.0 = флакон прямо на валу), меняется в Сервис → Мотор
#define MOTOR_ACCEL_RPM_S 6.0f   // плавный разгон: об/мин в секунду

#define RPM_MIN 1.0f
#define RPM_MAX 30.0f
#define RPM_DEFAULT 4.0f        // SpinSlicer по умолчанию: 360 кадров при 24 Гц = 4 об/мин

// ---------- Связь с Raspberry Pi (USB-кабель) ----------
#define SERIAL_BAUD 115200
#define PI_TIMEOUT_MS 3000      // если от Pi нет сообщений столько времени: "Pi не подключена"
