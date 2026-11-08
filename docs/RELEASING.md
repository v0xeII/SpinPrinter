# Как выпускать релиз SpinPrinter

Релиз — это **тег `vX.Y.Z` на коммите из `main`**, в котором все файлы говорят `X.Y.Z`.
Из этого тега берутся все три части принтера, поэтому они всегда одной версии:

| что | откуда |
|---|---|
| прошивка платы на [странице флешера](https://v0xeii.github.io/SpinPrinter/flasher/) | CI собирает **последний** тег `v*` (не `main`) |
| `spinscreen-vX.Y.Z-merged.bin` на странице релиза | CI собирает этот тег |
| SpinPi на Raspberry Pi (`get.sh`) | скачивает последний релиз |

Изменения в `main` без тега до пользователей не доходят: можно спокойно коммитить.

## Шаги

```bash
git checkout main && git pull
python3 dev/version.py 0.8.2          # версия в config.h, spinpi.py, manifest.json, README прошивки
python3 -m unittest discover tests    # и python3 dev/selftest.py, если есть время
git commit -am "Version 0.8.2"
git push origin main
git tag v0.8.2 && git push origin v0.8.2
```

Дальше всё делает CI (`.github/workflows/firmware.yml`):

1. проверяет, что тег стоит на `main` и что везде `0.8.2` (иначе падает и релиза нет);
2. собирает прошивку и создаёт релиз на GitHub с `merged.bin`;
3. перезапускает себя на `main`, и страница флешера начинает прошивать `0.8.2`.

Если поменялась распиновка, в том же релизе должны быть `config.h`, `docs/wiring*.svg`,
`docs/INSTALL*.md` и `SpinScreen_v0.7/README*.md`: флешер показывает схему из того же тега.

## Чего не делать

- Не ставить тег на ветку, которая не влита в `main` (так вышло с первым v0.8.1).
- Не переписывать историю `main` после того, как на неё поставлены теги: теги останутся на
  старых коммитах. Если всё же пришлось — перевесить теги на новые коммиты с тем же деревом
  (`git rev-parse <старый>^{tree}` = `git rev-parse <новый>^{tree}`) и `git push --force origin <тег>`.
- Не коммитить `flasher/fw/*.bin`: их собирает CI. Чтобы попробовать флешер у себя:
  `bash SpinScreen_v0.7/tools/update_flasher.sh`.
