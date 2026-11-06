# SpinPrinter

Прошивка и софт принтера SpinCAL (томографическая печать в колбе). Парный репозиторий к слайсеру [SpinSlicer](https://github.com/yunahinata/SpinSlicer).

## Состав

| Путь | Что это |
|---|---|
| `pi/spinprinter/` | Python-сервис на Raspberry Pi 3B+: конечный автомат печати, плеер кадров, API |
| `pi/web/` | веб-интерфейс для телефона (без сборки) |
| `firmware/motion-esp32/` | ESP32 «мотор»: TMC2209, заслонка, блокировки (PlatformIO) |
| `firmware/panel-s3/` | ESP32-S3 с 5" экраном: интерфейс принтера на LVGL (PlatformIO) |
| `system/` | systemd-юниты, avahi, автомонтирование, установщик |
| `tests/` | pytest |

Архитектура, формат задания `.spincal`, протоколы, калибровка и вехи — в [docs/plan.md](docs/plan.md).

## Статус

Каркас. Реализации пока нет.
