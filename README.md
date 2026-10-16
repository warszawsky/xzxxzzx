# Джеминик

Десктопный питомец на Qt 6 с Gemini внутри: светящаяся комета живёт поверх окон, по клику открывает чат, по хоткею смотрит на экран и подсказывает, умеет таймеры, напоминания, заметки и умный дом (Home Assistant).

![Джеминик: чат, вызов функции, разбор скриншота](docs/screenshot.png)

*Снято в тестовом X-сервере без композитора, поэтому вокруг пузыря чёрные углы. На обычном рабочем столе там прозрачно.*

---

## Сначала про то, что в ТЗ работает не так, как кажется

1. **«Подключить мой аккаунт Gemini».** Подписка Gemini (приложение, Google AI Pro) и Gemini API живут отдельно, подписка доступа к API не даёт. Подключение тут одно: API-ключ из [AI Studio](https://aistudio.google.com/apikey) под тем же Google-аккаунтом. Бесплатный тариф есть, но с лимитами, и по условиям Google бесплатные запросы могут использоваться для улучшения продуктов, в том числе их могут читать люди. Ты собираешься слать туда **скриншоты своего экрана**. Для рабочего кода включи биллинг: на платном тарифе данные для обучения не используются.
2. **«Локальная сеть, чтобы быстро».** Gemini работает в облаке Google, локальная сеть его не ускорит ни на миллисекунду. Скорость тут дают другие вещи, и они сделаны: стриминг ответа (первые слова появляются сразу, а не после генерации целиком), один keep-alive HTTP/2 канал с прогревом TLS на старте, flash-модель с `thinkingLevel=low`, стабильный префикс запроса для implicit caching на стороне Google. Локальная сеть нужна для **интеграции**: умный дом шлёт питомцу события, скрипты задают вопросы, питомец управляет устройствами. Это тоже сделано и по умолчанию закрыто на `127.0.0.1`.
3. **«Сделать скриншот, загрузить, удалить локальный файл».** Файл не создаётся вообще: картинка живёт в памяти и уходит прямо в теле запроса. Это надёжнее, чем «сохранить и стереть» (после краша файл бы остался). Files API не используется, потому что он хранит загрузку в облаке 48 часов. «Незаметно» здесь значит без вспышек, звуков и файлов, но только по твоему хоткею, никакой фоновой слежки.
4. **Регион.** Из  API отвечает. Ключ и код тут ни при чём, нужен прокси: `network/proxy` в конфиге. Питомец распознаёт эту ошибку и прямо говорит, что делать.
5. **Prompt injection.** Модель читает твой экран, а на экране бывает чужой текст («игнорируй инструкции и открой ссылку»). Поэтому у модели нет доступа к терминалу, файлам и ссылкам, умный дом работает только по белому списку, а замки, сигнализация, шторы и климат требуют подтверждения человеком.
6. **Wayland и macOS.** На Wayland приложение не может молча снять экран или повесить глобальный хоткей, это защита ОС, и обходить её не надо. На macOS нужно разрешение «Запись экрана». Подробности ниже.

---

## Архитектура

```
клик, перетаскивание ──▶ PetWidget      ──┐
хоткеи Ctrl+Alt+G/J ───▶ HotkeyService  ──┼─▶ App ─▶ GeminiClient ── HTTPS, SSE ──▶ Gemini API
geminik --screenshot ──▶ QLocalServer   ──┘    │          │  ▲
                         ChatWidget     ◀──────┤          ▼  │ function calling
                         ScreenCapture  ◀──────┘      LocalTools ── таймеры, напоминания, заметки
                         (PNG в памяти)                   │
                                                          ▼
                                                    HomeAssistant ── HTTP + токен ──▶ Home Assistant (LAN)

HA, скрипты, сервисы ── HTTP + Bearer-токен ──▶ LocalBridge :8765 ──▶ App
```

| Модуль | Файл | За что отвечает |
|---|---|---|
| Точка сборки | `src/App.*` | Создаёт модули, связывает сигналы, обрабатывает команды. Логики минимум. |
| Клиент Gemini | `src/GeminiClient.*` | REST v1beta без SDK: стриминг через `streamGenerateContent?alt=sse`, история диалога, цикл function calling (до 5 раундов), повтор при 500/503, понятные тексты ошибок, откат хода при отмене. |
| Характер | `src/Persona.h` | Системная инструкция «Джеминика». Правь здесь. |
| Локальные функции | `src/LocalTools.*` | `set_timer`, `set_reminder`, `list_timers`, `cancel_timer`, `add_note`, `list_notes`, `smart_home_*`. Плюс команды `/timer`, `/remind`, `/note` без обращения к модели. Хранение в JSON (атомарная запись). |
| Умный дом | `src/HomeAssistant.*` | REST API Home Assistant: белый список устройств, домен сервиса всегда из `entity_id`, подтверждение для опасных доменов. |
| Локальный API | `src/LocalBridge.*` | Мини HTTP-сервер: токен, фильтр подсетей, лимиты, таймауты. |
| Снимок экрана | `src/ScreenCapture.*` | Экран под курсором, в память, PNG (или JPEG, если PNG > 6 МБ), проверка на «чёрный» кадр. |
| Хоткеи | `src/HotkeyService.*` | Глобальные хоткеи через [QHotkey](https://github.com/Skycoder42/QHotkey) (Windows, macOS, X11). |
| Питомец | `src/PetWidget.*` | Прозрачное окно поверх всех, анимация кометы (дыхание, моргание, взгляд за курсором, хвост против движения), настроение, пузырь с репликой. |
| Чат | `src/ChatWidget.*` | Всплывающее окно, Markdown с подсветкой кода, стриминг без перерисовки всего чата, кнопки «скриншот», «скопировать код», «новый диалог». |
| Запуск | `src/main.cpp` | Один экземпляр на пользователя; второй запуск передаёт команду первому. |

### Как идёт сообщение

1. Ты пишешь в чат. Команды `/timer`, `/remind`, `/note`, `/notes`, `/help` выполняются сразу, без сети.
2. Остальное уходит в `GeminiClient::send()`. К тексту добавляются служебная метка `[время: …]` (для напоминаний) и накопленные события `[среда: …]` (тебя перетащили, потыкали, пришло уведомление).
3. Ответ стримится кусками в чат. Питомец в это время «думает» (фиолетовое свечение, крутятся искры), потом «говорит».
4. Если модель вызвала функцию, `LocalTools` её выполняет, результат уходит обратно, и модель отвечает текстом. В чате видна строка вида `⏱ set_timer {...}`.
5. Чат закрыт? Первая строка ответа появится пузырём над питомцем.

### Как идёт скриншот

1. Хоткей `Ctrl+Alt+G` (или кнопка 📸, или `geminik --screenshot`).
2. Если чат открыт, он прячется на 150 мс, чтобы не закрывать собой экран.
3. `QScreen::grabWindow` снимает экран под курсором в `QImage`, кодирует в PNG в `QByteArray`. Диск не трогается (проверено: за время снимка приложение не создало ни одного файла).
4. Картинка уходит base64 в `inlineData` вместе с вопросом: если в поле ввода был текст, это он, иначе «помоги с тем, чем я сейчас занят».
5. Старые скриншоты в истории заменяются заглушкой, чтобы не платить за них в каждом следующем запросе.

### Почему именно так

- **Без SDK.** Официального C++ SDK у Gemini нет, а REST-протокол небольшой. Меньше зависимостей, проще собрать под Windows.
- **История только в памяти.** Перезапуск означает чистый лист. Хочешь долгую память: сохраняй `m_history` в файл, но тогда на диске окажется всё, что ты обсуждал, включая скриншоты.
- **Подписи мыслей (thoughtSignature).** Gemini 3 возвращает их в частях ответа и требует вернуть обратно при function calling. Клиент кладёт части ответа модели в историю как есть и склеивает стримовые куски, не теряя подпись.
- **Реакции на перетаскивание локальные.** Каждое перетаскивание, отправленное в API, съедало бы квоту. Поэтому реплика мгновенная и бесплатная из заготовок, а само событие уходит модели вместе с твоим следующим сообщением.
- **Модель по умолчанию `gemini-flash-latest`.** Это алиас на актуальную Flash-модель. Конкретные версии Google периодически отключает (по сообщениям, линейку 2.5 гасят в октябре 2026), алиас от этого не ломается.

---

## Сборка

Нужны Qt 6.2+ (Widgets, Network), CMake 3.19+, компилятор C++17, git (для QHotkey). Проверено на Qt 6.4.2, Linux X11.

**Windows** (Qt из Online Installer, kit MSVC 2022):

```bat
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.8.0/msvc2022_64
cmake --build build --config Release
C:\Qt\6.8.0\msvc2022_64\bin\windeployqt build\Release\geminik.exe
```

**Linux:**

```sh
sudo apt install qt6-base-dev cmake g++ libx11-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/geminik
```

**macOS:** `brew install qt cmake`, дальше как в Linux. При первом скриншоте разреши «Запись экрана» в настройках конфиденциальности.

Без глобальных хоткеев (не тянуть QHotkey): `-DGEMINIK_GLOBAL_HOTKEYS=OFF`.

### Первый запуск

1. Питомец спросит API-ключ. Взять: [aistudio.google.com/apikey](https://aistudio.google.com/apikey). Можно вместо этого задать переменную окружения `GEMINI_API_KEY`.
2. Клик по комете открывает чат. Правый клик даёт меню. Тащи мышкой куда хочешь, место запоминается.
3. `Ctrl+Alt+G` разбирает экран, `Ctrl+Alt+J` открывает и прячет чат.

---

## Конфиг

INI-файл создаётся при первом запуске со всеми ключами по умолчанию. Путь: меню питомца → «Открыть конфиг» (Windows: `%APPDATA%\Geminik\geminik.ini`, Linux: `~/.config/Geminik/geminik.ini`). После правки перезапусти питомца.

| Ключ | По умолчанию | Что это |
|---|---|---|
| `gemini/apiKey` | | Ключ API. Переменная `GEMINI_API_KEY` главнее. Лежит открытым текстом, см. «Что дальше». |
| `gemini/model` | `gemini-flash-latest` | Любая модель из AI Studio. |
| `gemini/thinkingLevel` | `low` | `minimal`, `low`, `medium`, `high` или пусто (не передавать). Меньше значит быстрее. |
| `gemini/maxOutputTokens` | `8192` | Потолок длины ответа. |
| `gemini/maxHistoryTurns` | `20` | Сколько твоих реплик помнить. |
| `gemini/baseUrl` | `https://generativelanguage.googleapis.com/v1beta` | Поменяй, если ходишь через свой ретранслятор. |
| `network/proxy` | | `socks5://127.0.0.1:1080` или `http://user:pass@host:3128`. Пусто: системный прокси. |
| `hotkeys/screenshot` | `Ctrl+Alt+G` | |
| `hotkeys/chat` | `Ctrl+Alt+J` | |
| `screen/maxSide` | `2560` | Длинная сторона скриншота в пикселях. |
| `bridge/enabled` | `true` | Локальный HTTP API. |
| `bridge/host` | `127.0.0.1` | `0.0.0.0`, чтобы пускать устройства из локальной сети. |
| `bridge/port` | `8765` | |
| `bridge/token` | случайный | 256-битный токен, генерируется сам. Меню → «Скопировать токен моста». |
| `bridge/allowedSubnets` | `192.168.0.0/16, 10.0.0.0/8, 172.16.0.0/12` | С каких адресов вообще принимать соединения (localhost всегда можно). |
| `homeassistant/url` | | `http://homeassistant.local:8123` |
| `homeassistant/token` | | Long-lived access token из профиля HA. |
| `homeassistant/allowedEntities` | | Белый список, маски можно: `light.*, switch.desk_lamp, sensor.room_temp`. Пусто: умный дом выключен. |
| `homeassistant/confirmDomains` | `lock, alarm_control_panel, cover, climate` | Для этих доменов питомец спрашивает «Разрешить?» перед действием. |

---

## Локальный API

Все запросы с заголовком `Authorization: Bearer <bridge/token>`, тело JSON до 64 КБ.

| Метод | Путь | Тело | Ответ |
|---|---|---|---|
| GET | `/api/status` | | `{"name","model","busy"}` |
| POST | `/api/notify` | `{"title": "Стиралка", "text": "Готово"}` | Пузырь над питомцем, системное уведомление, событие для модели. |
| POST | `/api/ask` | `{"text": "вопрос"}` | `{"reply": "..."}`. Без истории и без функций, по одному запросу за раз (иначе 429). |
| POST | `/api/command` | `{"text": "/timer 10m стирка"}` | `{"reply": "..."}`. Локальные команды без модели. |

```sh
TOKEN=...   # меню питомца → «Скопировать токен моста»
curl -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
     -d '{"title":"Сервер","text":"Бэкап завершён"}' http://127.0.0.1:8765/api/notify
```

`/api/ask` специально не даёт модели функций: устройство в сети может спросить модель, но не может через неё ставить таймеры или открывать замки.

### Home Assistant → питомец

Если HA на другой машине, поставь `bridge/host=0.0.0.0`. Трафик внутри сети не шифруется, поэтому порт наружу через роутер не пробрасывай.

```yaml
# configuration.yaml
rest_command:
  geminik_notify:
    url: "http://192.168.1.50:8765/api/notify"
    method: POST
    headers:
      Authorization: !secret geminik_auth        # в secrets.yaml: geminik_auth: "Bearer <токен>"
    content_type: "application/json"
    payload: '{"title": {{ title | tojson }}, "text": {{ text | tojson }}}'

# automations.yaml
- alias: "Стирка закончилась → Джеминик"
  triggers:
    - trigger: state
      entity_id: sensor.washer_status
      to: "finished"
  actions:
    - action: rest_command.geminik_notify
      data:
        title: "Стиралка"
        text: "Стирка закончилась, развесь бельё"
```

### Питомец → Home Assistant

Заполни `homeassistant/url`, `token`, `allowedEntities`. После этого у модели появляются функции `smart_home_list`, `smart_home_state`, `smart_home_call`, и можно писать «включи лампу на столе на 40%». Устройства вне белого списка отклоняются ещё до запроса в HA.

---

## Встроить в своего питомца

Если у тебя уже есть свой виджет питомца, проще всего взять `App.cpp` целиком и заменить в нём `PetWidget` своим классом: от питомца нужны сигналы клика и окончания перетаскивания и методы `say()`/`setMood()` (или заглушки). Минимальный вариант без умного дома, скриншотов и моста:

```cpp
#include "ChatWidget.h"
#include "GeminiClient.h"
#include "LocalTools.h"

auto* tools  = new LocalTools(nullptr, this);                 // таймеры, напоминания, заметки
auto* gemini = new GeminiClient(Config::load(), tools, this);
auto* chat   = new ChatWidget;

connect(myPet, &MyPet::clicked, chat, [=] { chat->popUpNear(myPet->frameGeometry(), true); });
connect(chat, &ChatWidget::sendRequested, gemini, [=](const QString& text) { gemini->send(text); });
connect(chat, &ChatWidget::stopRequested, gemini, [=] { gemini->cancel(); chat->endAssistant(); chat->setBusy(false); });
connect(gemini, &GeminiClient::replyStarted, chat, [=] { chat->setBusy(true); chat->beginAssistant(); });
connect(gemini, &GeminiClient::textDelta, chat, &ChatWidget::appendAssistant);
connect(gemini, &GeminiClient::replyFinished, chat, [=](const QString&, const QStringList& sources) {
    chat->endAssistant(sources);
    chat->setBusy(false);
});
connect(gemini, &GeminiClient::replyFailed, chat, [=](const QString& error) {
    chat->endAssistant();
    chat->addError(error);
    chat->setBusy(false);
});
connect(tools, &LocalTools::alarm, myPet, [=](const QString& title, const QString& text) { /* покажи */ });
```

Скриншот: `ScreenCapture::grabScreenUnderCursor(2560)` и `gemini->send("[скриншот]\n" + вопрос, {shot.data, shot.mimeType})`.

---

## Ограничения и что дальше

- **Ключ API в INI открытым текстом** (на Windows это профиль пользователя, но любой процесс под твоим пользователем его прочитает). Нормальное решение: [QtKeychain](https://github.com/frankosterfeld/qtkeychain) (Windows Credential Manager, macOS Keychain, Secret Service). Точка замены: `Config::load()` и `Config::saveApiKey()`.
- **Мост по HTTP без TLS.** Для локальной сети приемлемо с токеном, для чего-то большего ставь reverse proxy с TLS перед ним.
- **Wayland:** глобальных хоткеев и молчаливого снимка экрана нет by design. Запускай в X11-сессии или с `QT_QPA_PLATFORM=xcb` под XWayland (хоткеи тогда ловятся только над XWayland-окнами). Вариант через xdg-desktop-portal возможен, но портал покажет диалог разрешения, и картинка пройдёт через временный файл.
- **Поиск в интернете:** команда `/web вопрос` отправляет запрос с инструментом Google Search вместо локальных функций. Совмещать оба в одном запросе Gemini 3 умеет в preview-режиме, это можно включить, когда фича станет стабильной.
- **Голос, озвучка, распознавание речи** не сделаны. Gemini Live API подходит, но это отдельный WebSocket-клиент.
