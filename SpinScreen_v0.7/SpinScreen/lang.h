// All on-screen text, Russian + English. Add a language = add a column.
#pragma once

enum Str {
  // main
  S_PRINT, S_CONNECT, S_SERVICE,
  // service
  S_PROJECTOR, S_MOTOR, S_SCREEN, S_PI_MENU, S_ABOUT, S_BACK,
  // projector
  S_P_BLACK, S_P_WHITE, S_P_GRID, S_P_FOCUS, S_P_CIRCLE, S_P_CENTER, S_P_VIAL,
  // motor
  S_ROTATION, S_SPEED, S_DIRECTION, S_ACCEL, S_MICROSTEP, S_STEPS_REV, S_GEAR, S_ONEREV, S_ZERO,
  // screen
  S_BRIGHT, S_LANG, S_DARK,
  // pi
  S_REBOOT, S_POWEROFF, S_SURE, S_SURE_HINT,
  // files + setup
  S_FILES, S_LOADING, S_NO_FILES1, S_NO_FILES2, S_USB, S_FRAMES,
  S_SU_TURNS, S_SU_FACTOR, S_SU_DELAY, S_PRINT_BTN, S_SETUP_HINT, S_UNTIL_STOP, S_SEC,
  // values
  S_ON, S_OFF, S_RPM, S_LANGNAME, S_CW, S_CCW, S_RPM_S, S_PX,
  // top bar + toasts
  S_PI_OK, S_PI_NO, S_NEED_PI, S_WAIT_PI, S_SPINNING, S_STOPPED, S_SAVED, S_ZEROED, S_ERROR, S_PI_HELLO, S_SENT,
  // print
  S_PRINTING, S_LEFT, S_ELAPSED, S_HOLD_STOP, S_STOPPING, S_SYNC, S_REV, S_DARKENED,
  S_DONE, S_DONE_SUB,
  // edit
  S_HINT_FINE, S_HINT, S_DEG_S, S_SEC_REV, S_HINT_LIVE,
  // connect
  S_OPEN_PHONE, S_OR, S_WIFI, S_NOPI1, S_NOPI2,
  // about
  S_VERSION, S_UPTIME, S_PI, S_CONNECTED, S_NOT_CONNECTED, S_BACK_HINT, S_STEPS,
  S_TAGLINE,
  // exposure
  S_TURNS_UNIT, S_PLAN, S_NO_PLAN, S_W_CADENCE, S_W_DOSE, S_WAIT_SPIN, S_WAIT_WARM, S_WAIT_DARK, S_OF,
  S_DROPS, S_HOW, S_R_UNDER, S_R_OK, S_R_OVER, S_PI_LOST,
  // axolotl
  S_AXO, S_AXO_SHOW, S_AXO_COLOR, S_C_PINK, S_C_GOLD, S_C_WILD, S_C_BLACK, S_C_BLUE, S_C_RGB,
  // customisation
  S_CARD_PICK, S_HOME_CARD, S_HOME_AXO,
  S_STOP_AGAIN, S_ASK_TITLE, S_ASK_FROM, S_ASK_STARTING, S_ASK_NO,
  S_DONE_HINT,
  S_COUNT
};

static const char* const STR[2][S_COUNT] = {
    {
        "Печать", "Подключение", "Сервис",
        "Проектор", "Мотор", "Экран", "Raspberry Pi", "О принтере", "Назад",
        "Чёрный", "Белый", "Сетка", "Фокус", "Круг", "Центровка", "Ширина флакона",
        "Вращение", "Скорость", "Направление", "Разгон", "Микрошаг", "Шагов/оборот", "Передача", "Ровно 1 оборот",
        "Обнулить угол",
        "Яркость", "Язык", "Тёмный при печати",
        "Перезагрузить Pi", "Выключить Pi", "Точно?", "→ да · ← нет",
        "Видео", "Загружаю список…", "Видео нет.", "Вставь флешку в Pi или загрузи по QR.", "USB", "кадров",
        "Обороты", "Поправка", "Пауза до света", "ПЕЧАТЬ", "↑↓ выбор · → OK · ← назад", "до стопа", "с",
        "вкл", "выкл", "об/мин", "Русский", "по час.", "против", "об/мин за с", "пикс",
        "Pi", "нет Pi", "Сначала подключи Pi", "Жду Pi…", "Вращение включено", "Вращение выключено", "Сохранено",
        "Угол обнулён", "Ошибка", "Pi подключена", "Отправлено",
        "Печать", "осталось", "прошло", "← дважды — стоп", "Останавливаю…", "синхр.", "оборот", "экран притушен",
        "Готово!", "Деталь напечатана",
        "↑↓ шаг · держи — быстрее", "↑↓ изменить · → готово", "°/с", "с/об", "видно на проекторе",
        "Открой на телефоне", "или", "Wi-Fi", "Pi не подключена.", "Подключи её к ESP по USB.",
        "Версия", "Работаю", "Pi", "подключена", "нет связи", "← назад", "шагов на оборот",
        "объёмная 3D-печать",
        "оборотов", "расчёт", "без расчёта SpinSlicer: подбери обороты", "кадры неровно: экспортируй 30 к/с",
        "доза", "разгон мотора", "прогрев проектора", "пауза до света", "из",
        "пропуски кадров", "Как получилось?", "Недосвет", "Норма", "Пересвет", "Pi не отвечает: печать остановлена",
        "Кастомизация", "Главная", "Окраска", "Розовый", "Золотой", "Дикий", "Чёрный", "Голубой", "Радуга RGB",
        "выбрать видео", "Карточка", "Аксолотль",
        "← ещё раз — стоп", "Печатать?", "из SpinSlicer", "Запускаю…", "Печать отменена",
        "↑↓ выбор · → ответ · ← пропуск",
    },
    {
        "Print", "Connect", "Service",
        "Projector", "Motor", "Display", "Raspberry Pi", "About", "Back",
        "Black", "White", "Grid", "Focus", "Circle", "Centering", "Vial width",
        "Rotation", "Speed", "Direction", "Ramp", "Microsteps", "Steps/rev", "Gear ratio", "Exactly 1 turn",
        "Zero angle",
        "Brightness", "Language", "Dark while printing",
        "Reboot Pi", "Shut down Pi", "Sure?", "→ yes · ← no",
        "Videos", "Loading list…", "No videos.", "Plug a USB stick into the Pi or upload via QR.", "USB", "frames",
        "Turns", "Correction", "Dark spin-up", "PRINT", "↑↓ select · → OK · ← back", "until stop", "s",
        "on", "off", "rpm", "English", "CW", "CCW", "rpm per s", "px",
        "Pi", "no Pi", "Connect the Pi first", "Waiting for Pi…", "Rotation on", "Rotation off", "Saved",
        "Angle zeroed", "Error", "Pi connected", "Sent",
        "Printing", "left", "elapsed", "← twice = stop", "Stopping…", "sync", "turn", "screen dimmed",
        "Done!", "Part is ready",
        "↑↓ step · hold = faster", "↑↓ change · → done", "°/s", "s/rev", "shown on the projector",
        "Open on your phone", "or", "Wi-Fi", "The Pi is not connected.", "Plug it into the ESP via USB.",
        "Version", "Uptime", "Pi", "connected", "offline", "← back", "steps per rev",
        "volumetric 3D printing",
        "turns", "plan", "no SpinSlicer plan: pick the turns", "uneven frames: export at 30 fps",
        "dose", "motor spin-up", "projector warm-up", "dark spin-up", "of",
        "dropped frames", "How did it come out?", "Under", "Good", "Over", "Pi not responding: print stopped",
        "Customize", "Home", "Colour", "Pink", "Golden", "Wild", "Black", "Blue", "Rainbow RGB",
        "choose a video", "Card", "Axolotl",
        "← again = stop", "Print?", "from SpinSlicer", "Starting…", "Print cancelled",
        "↑↓ choose · → answer · ← skip",
    },
};
