// ============================================================
//  SpinScreen config: железо принтера. Вариант один:
//    плата    ESP32-S3 Super Mini
//    экран    ST7789 2.4" 240x320 (например GMT024-08-SPI8P), на проводах
//    ручка    джойстик KY-023
//    мотор    шаговый, драйвер TMC2209 (STEP / DIR / EN)
//    с Pi     USB-кабель
//  Номера пинов = номера GPIO, подписанные на плате.
// ============================================================
#pragma once

#define FW_VERSION "0.8.2"

// ---------- Экран ST7789 240x320 (SPI) ----------
//  экран  ->  ESP32-S3
//  GND    ->  GND
//  VCC    ->  3V3   (не 5V!)
//  SCL    ->  43    (TX платы)
//  SDA    ->  44    (RX платы)
//  RES    ->  1
//  DC     ->  2
//  CS     ->  3
//  BLK    ->  4     (подсветка, яркость через PWM)
#define PIN_TFT_SCL 43
#define PIN_TFT_SDA 44
#define PIN_TFT_RES 1
#define PIN_TFT_DC  2
#define PIN_TFT_CS  3
#define PIN_TFT_BLK 4

#define TFT_ROTATION 1   // 1 или 3: экран лёжа (320 x 240), 3 = перевёрнут на 180°
#define TFT_SPI_MHZ 40   // мусор или полосы на длинных проводах: 20
#define TFT_SPI_MODE 3   // режим SPI 3 для этой проводки (как в пресете breadboard 0.7.1)
#define TFT_INVERT 1     // цвета «негатив»: 0
#define TFT_BGR 0        // красный и синий перепутаны: 1

// ---------- Джойстик KY-023 ----------
//  джойстик  ->  ESP32-S3
//  GND       ->  GND
//  +5V       ->  3V3   (не 5V! иначе на входы ESP придёт больше 3,3 В)
//  VRx       ->  13    (аналоговый вход, ADC2)
//  VRy       ->  11    (аналоговый вход, ADC2)
//  SW        ->  12    (нажатие на ручку)
// Вверх / вниз = по списку, вправо или нажать = OK, влево или долго нажать = назад.
// Центр запоминается при включении: в этот момент ручку не трогать.
#define PIN_JOY_X   13
#define PIN_JOY_Y   11
#define PIN_JOY_SW  12
#define JOY_SWAP_XY  0  // 1, если вверх/вниз срабатывает при наклоне вбок
#define JOY_INVERT_Y 0  // 1, если вверх и вниз перепутаны
#define JOY_INVERT_X 0  // 1, если OK и назад перепутаны

// ---------- Шаговый мотор (TMC2209 в режиме STEP/DIR) ----------
#define PIN_STEP 5
#define PIN_DIR  6
#define PIN_EN   7    // EN у драйвера: LOW = мотор включён

// Значения по умолчанию. Потом всё меняется прямо в меню «Мотор» и запоминается.
#define MOTOR_STEPS_PER_REV 200  // обычный NEMA17 = 200 шагов (1.8°)
#define MOTOR_MICROSTEPS 16      // как настроен драйвер (TMC2209: перемычки MS1/MS2)
#define MOTOR_GEAR 1.0f          // передаточное число (1.0 = флакон прямо на валу), меняется в Сервис → Мотор
#define MOTOR_ACCEL_RPM_S 6.0f   // плавный разгон: об/мин в секунду

#define RPM_MIN 1.0f
#define RPM_MAX 30.0f
#define RPM_DEFAULT 4.0f        // SpinSlicer по умолчанию: 360 кадров при 24 Гц = 4 об/мин

// ---------- Связь с Raspberry Pi: USB-кабель (USB-C платы в любой USB-порт Pi) ----------
#define SERIAL_BAUD 115200
#define PI_TIMEOUT_MS 3000      // если от Pi нет сообщений столько времени: "Pi не подключена"
