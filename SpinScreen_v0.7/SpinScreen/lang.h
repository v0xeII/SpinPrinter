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
  S_REBOOT, S_POWEROFF, S_SURE, S_SURE_HINT, S_REBOOT_SHORT, S_POWEROFF_SHORT,
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
  // settings groups
  S_G_DISPLAY, S_G_PROJ, S_G_MOTOR, S_G_SYSTEM,
  // system page: what the Raspberry tells (SYS), counters, firmware update
  S_NET, S_SPACE, S_PRINTS, S_BOARD, S_CABLE, S_GB, S_HOURS, S_FWUP, S_FW_FLASHING, S_FW_DONT,
  // screen sleep
  S_SLEEP, S_MIN,
  // first-setup wizard
  S_G_WIZARD, S_WIZ_STEP, S_WIZ_HELLO, S_WIZ_HELLO1, S_WIZ_LINK, S_WIZ_LINK_OK, S_WIZ_LINK_NO, S_WIZ_HDMI,
  S_WIZ_FOCUS, S_WIZ_FOCUS1, S_WIZ_CENTER1, S_WIZ_VIAL1, S_WIZ_VIAL2, S_WIZ_DIR, S_WIZ_DIR1, S_WIZ_DIR2,
  S_WIZ_TURN1, S_WIZ_TURN2, S_WIZ_SPEED1, S_WIZ_SPEED2, S_WIZ_SCREEN1, S_WIZ_DONE1, S_WIZ_NEED_PI,
  S_WIZ_HINT, S_WIZ_HINT0, S_WIZ_HINT_NEXT, S_WIZ_HINT_PIC, S_WIZ_HINT_TURN, S_WIZ_HINT_DIR, S_WIZ_HINT_END,
  S_NOT_CONNECTED_M, S_RASP_OLD, S_UPD_ASK, S_UPD_FOUND,
  // the main page's readiness line
  S_READY, S_NO_RASP, S_NO_PROJ, S_OLD_SPINPI, S_UPD_HAVE,
  // updates over the air (the Raspberry asks GitHub)
  S_UPD_CHECK, S_UPD_SHORT, S_UPD_DO, S_UPD_NONE, S_UPD_ERR, S_UPD_PI, S_UPD_PI_SUB, S_UPD_LATEST, S_UPD_NOANSWER,
  S_COUNT
};

static const char* const STR[2][S_COUNT] = {
    {
        "Печать", "Подключение", "Настройки",
        "Проектор", "Мотор", "Экран", "Raspberry Pi", "О принтере", "Назад",
        "Чёрный", "Белый", "Сетка", "Фокус", "Круг", "Центровка", "Ширина флакона",
        "Вращение", "Скорость", "Направление", "Разгон", "Микрошаг", "Шагов/оборот", "Передача", "Ровно 1 оборот",
        "Обнулить угол",
        "Яркость", "Язык", "Тёмный при печати",
        "Перезагрузить Raspberry", "Выключить Raspberry", "Точно?", "→ да · ← нет", "Перезагрузить", "Выключить",
        "Видео", "Загружаю список…", "Видео нет.", "Вставь флешку в Raspberry или загрузи по QR.", "USB", "кадров",
        "Обороты", "Поправка", "Пауза до света", "ПЕЧАТЬ", "↑↓ выбор · → OK · ← назад", "до стопа", "с",
        "вкл", "выкл", "об/мин", "Русский", "по час.", "против", "об/мин за с", "пикс",
        "Raspberry", "нет Raspberry", "Сначала подключи Raspberry", "Жду Raspberry…", "Вращение включено", "Вращение выключено", "Сохранено",
        "Угол обнулён", "Ошибка", "Raspberry подключена", "Отправлено",
        "Печать", "осталось", "прошло", "← дважды — стоп", "Останавливаю…", "синхр.", "оборот", "экран притушен",
        "Готово!", "Деталь напечатана",
        "↑↓ шаг · держи — быстрее", "↑↓ изменить · → готово", "°/с", "с/об", "видно на проекторе",
        "Открой на телефоне", "или", "Wi-Fi", "Raspberry не подключена.", "Подключи её к ESP по USB.",
        "Версия", "Работаю", "Raspberry", "подключена", "нет связи", "← назад", "шагов на оборот",
        "объёмная 3D-печать",
        "оборотов", "расчёт", "без расчёта SpinSlicer: подбери обороты", "кадры неровно: экспортируй 30 к/с",
        "доза", "разгон мотора", "прогрев проектора", "пауза до света", "из",
        "пропуски кадров", "Как получилось?", "Недосвет", "Норма", "Пересвет", "Raspberry не отвечает: печать остановлена",
        "Кастомизация", "Главная", "Окраска", "Розовый", "Золотой", "Дикий", "Чёрный", "Голубой", "Радуга RGB",
        "выбрать видео", "Карточка", "Аксолотль",
        "← ещё раз — стоп", "Печатать?", "из SpinSlicer", "Запускаю…", "Печать отменена",
        "↑↓ выбор · → ответ · ← пропуск",
        "Экран", "Проектор", "Мотор", "Система",
        "Сеть", "Место", "Напечатано", "Плата", "кабель", "ГБ", "ч", "Обновить прошивку", "Обновляю прошивку…",
        "Не выключай принтер",
        "Засыпать", "мин",
        "Мастер настройки", "Настройка", "Привет! Настроим принтер", "Связь, проектор, мотор: пара минут.",
        "Связь", "Всё на месте: дальше проектор.", "Соедини плату с Raspberry USB-кабелем.", "Проектор (HDMI)",
        "Резкость", "Крути объектив, пока картинка не станет резкой.", "Совмести линию с осью флакона.",
        "Подгони полосы к краям флакона.", "Это число нужно в SpinSlicer.", "Направление",
        "Флакон крутится медленно.", "Сверху — как задано в SpinSlicer?",
        "Отметь флакон: он сделает ровно 1 оборот.", "Метка вернулась на место? Нет — «Передача».",
        "Крутится на скорости печати.", "Ровно, без биения и рывков?", "Яркость экрана",
        "Принтер настроен. Повторить: Настройки.", "Нужна Raspberry: → пропустить",
        "↑↓ изменить · → дальше · ← назад", "↑↓ язык · → дальше · ← пропустить", "→ дальше · ← назад",
        "↑↓ картинка · → дальше · ← назад", "↑ ещё раз · → дальше · ← назад", "↑↓ сменить · → дальше · ← назад",
        "→ к печати · ← назад",
        "не подключён", "старый SpinPi", "Проверяю обновления…", "Есть %s: нажми ещё раз",
        "Готов к печати", "Raspberry не подключена", "Проектор не подключён", "Обнови SpinPi на Raspberry",
        "Есть обновление %s",
        "Проверить обновления", "Обновления", "Обновить до", "нет новых", "ошибка", "Обновляю Raspberry…",
        "от 2 до 5 минут, перезапустится сама", "Всё обновлено", "Raspberry не ответила",
    },
    {
        "Print", "Connect", "Settings",
        "Projector", "Motor", "Display", "Raspberry Pi", "About", "Back",
        "Black", "White", "Grid", "Focus", "Circle", "Centering", "Vial width",
        "Rotation", "Speed", "Direction", "Ramp", "Microsteps", "Steps/rev", "Gear ratio", "Exactly 1 turn",
        "Zero angle",
        "Brightness", "Language", "Dark while printing",
        "Reboot Raspberry", "Shut down Raspberry", "Sure?", "→ yes · ← no", "Reboot", "Shut down",
        "Videos", "Loading list…", "No videos.", "Plug a USB stick into the Raspberry or upload via QR.", "USB", "frames",
        "Turns", "Correction", "Dark spin-up", "PRINT", "↑↓ select · → OK · ← back", "until stop", "s",
        "on", "off", "rpm", "English", "CW", "CCW", "rpm per s", "px",
        "Raspberry", "no Raspberry", "Connect the Raspberry first", "Waiting for Raspberry…", "Rotation on", "Rotation off", "Saved",
        "Angle zeroed", "Error", "Raspberry connected", "Sent",
        "Printing", "left", "elapsed", "← twice = stop", "Stopping…", "sync", "turn", "screen dimmed",
        "Done!", "Part is ready",
        "↑↓ step · hold = faster", "↑↓ change · → done", "°/s", "s/rev", "shown on the projector",
        "Open on your phone", "or", "Wi-Fi", "The Raspberry is not connected.", "Plug it into the ESP via USB.",
        "Version", "Uptime", "Raspberry", "connected", "offline", "← back", "steps per rev",
        "volumetric 3D printing",
        "turns", "plan", "no SpinSlicer plan: pick the turns", "uneven frames: export at 30 fps",
        "dose", "motor spin-up", "projector warm-up", "dark spin-up", "of",
        "dropped frames", "How did it come out?", "Under", "Good", "Over", "Raspberry not responding: print stopped",
        "Customize", "Home", "Colour", "Pink", "Golden", "Wild", "Black", "Blue", "Rainbow RGB",
        "choose a video", "Card", "Axolotl",
        "← again = stop", "Print?", "from SpinSlicer", "Starting…", "Print cancelled",
        "↑↓ choose · → answer · ← skip",
        "Display", "Projector", "Motor", "System",
        "Network", "Storage", "Prints", "Board", "cable", "GB", "h", "Update firmware", "Updating firmware…",
        "Do not switch the printer off",
        "Sleep after", "min",
        "Setup wizard", "Setup", "Hi! Let's set up the printer", "Link, projector, motor: two minutes.",
        "Connection", "All there: the projector is next.", "Plug the board into the Raspberry (USB).",
        "Projector (HDMI)",
        "Focus", "Turn the lens until the picture is sharp.", "Line up the stripe with the vial axis.",
        "Fit the bars to the vial's edges.", "SpinSlicer needs this number.", "Direction",
        "The vial turns slowly.", "From the top: as set in SpinSlicer?",
        "Mark the vial: it makes exactly 1 turn.", "Mark back in place? No: check \"Gear ratio\".",
        "Spinning at print speed.", "Smooth, no wobble or jerks?", "Screen brightness",
        "The printer is set up. Again: Settings.", "Needs the Raspberry: → skip",
        "↑↓ change · → next · ← back", "↑↓ language · → next · ← skip", "→ next · ← back",
        "↑↓ picture · → next · ← back", "↑ again · → next · ← back", "↑↓ switch · → next · ← back",
        "→ to printing · ← back",
        "not connected", "old SpinPi", "Checking for updates…", "%s is out: press again",
        "Ready to print", "Raspberry not connected", "Projector not connected", "Update SpinPi on the Raspberry",
        "Update %s available",
        "Check for updates", "Updates", "Update to", "up to date", "error", "Updating the Raspberry…",
        "2 to 5 minutes, it restarts by itself", "Everything is up to date", "No answer from the Raspberry",
    },
};
