# Faital Bluetooth Speaker

Мінімальна production-прошивка Bluetooth-колонки на ESP32 з Arduino framework.
Вона приймає стерео PCM через Bluetooth A2DP і без додаткової черги передає
його на зовнішній DAC PCM5102A:

```text
Bluetooth A2DP
→ BluetoothA2DPSink
→ AudioTools I2SStream
→ PCM5102A
```

У застосунку немає власного PCM callback, ring/stream buffer, audio writer
task, resampling, DSP, тестового тону, Wi-Fi чи діагностичних environment'ів.

## Платформа і залежності

Єдиний PlatformIO environment — `speaker-arduino`:

| Компонент | Зафіксована версія |
| --- | --- |
| PlatformIO platform | `espressif32@7.1.3` |
| Arduino-ESP32 | `2.0.17` |
| ESP-IDF base | `4.4.7` |
| ESP32-A2DP | `v1.8.11` |
| arduino-audio-tools | `v1.2.5` |
| ESP32Encoder | `0.12.0` |
| OneButton | `2.6.1` |

Git-залежності беруться з офіційних репозиторіїв, а encoder/button — з
PlatformIO Registry. Перевірені commit ID й точні registry-версії наведені у
`dependencies.lock`. Плата `esp32doit-devkit-v1` має 4 MB flash;
використовується штатна таблиця `huge_app.csv`, бо Bluetooth firmware не
вміщується у стандартний app partition 1.25 MiB.

Build flag
`ESP_A2D_AUDIO_STATE_SUSPEND=ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND` потрібний для
сумісності ESP32-A2DP 1.8.11 з ESP-IDF 4.4.7: у цій версії IDF стан названо
`ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND`, а бібліотека також посилається на коротку
назву `ESP_A2D_AUDIO_STATE_SUSPEND`.

## GPIO та I2S

| Функція | GPIO | Режим |
| --- | ---: | --- |
| I2S BCK | 26 | output |
| I2S WS / LRCK | 25 | output |
| I2S DATA | 19 | output |
| I2S RX | — | disabled |
| Вбудований status LED | 2 | output, active-high |
| EC11 A | 32 | input pull-up, active-low, PCNT |
| EC11 B | 33 | input pull-up, active-low, PCNT |
| EC11 button | 18 | input pull-up, active-low |
| Previous button | 13 | input pull-up, active-low |
| Next button | 14 | input pull-up, active-low |
| Bluetooth button | 23 | input pull-up, active-low |
| Future WS2812 | 27 | reserved, unused |
| Future I²C OLED | 21 / 22 | reserved, unused |
| Future battery ADC | 34 або 35 | reserved, unused |

I2S налаштовано як TX master, Philips I2S, signed 16-bit little-endian PCM,
stereo, з початковою частотою 44100 Hz. Після A2DP negotiation
`BluetoothA2DPSink` передає нову частоту через штатний AudioTools adapter до
`I2SStream::setAudioInfo()`; application code не ресемплює й не перекладає
байти або канали вручну.

GPIO19 є обов'язковим DATA-виходом цієї конфігурації. Перенесення DATA з
GPIO22 на GPIO19 усунуло нерівне, «вібруюче» відтворення, яке відтворювалося з
GPIO22 на двох ESP32. Це результат попереднього апаратного дослідження; нова
production-прошивка ще потребує фізичного тесту після очищення проєкту.

## Поворотний енкодер і керування медіа

Використовується голий механічний EC11 із трьома контактами обертання `A/C/B`
і двома окремими контактами нормально розімкненої кнопки. Це не модуль KY-040:
EC11 не має VCC, а широкі металеві лапки корпусу є лише механічними
кріпленнями. Жоден контакт EC11 не підключається безпосередньо до 3.3 V або
5 V.

| Контакт EC11 | ESP32 |
| --- | --- |
| крайній A у групі з трьох | GPIO32, internal pull-up |
| середній C/common у групі з трьох | GND |
| крайній B у групі з трьох | GPIO33, internal pull-up |
| один із двох контактів кнопки | GPIO18, internal pull-up |
| другий контакт кнопки | GND |
| широкі металеві лапки | не підключати; механічне кріплення |

Кнопка не має полярності. Контакти A, B і кнопка активні в LOW та
підтягуються внутрішніми pull-up ESP32 до 3.3 V. Крайні A/B можна поміняти
місцями: зміниться тільки напрям обертання, який коригується параметром
`ENCODER_REVERSED`.

`ESP32Encoder` читає A/B апаратним ESP32 PCNT у half-quadrature mode. Два raw
counts нормалізуються в один фізичний фіксований крок. За замовчуванням
`ENCODER_REVERSED=false`: clockwise збільшує гучність, counter-clockwise
зменшує.

Керування:

- clockwise — Bluetooth volume +4 у діапазоні 0…127;
- counter-clockwise — Bluetooth volume −4;
- short press — Play/Pause;
- double-click, long press, mute й acceleration не використовуються.

`media_control` є єдиним application-level шаром над AVRCP і volume API
наявного `BluetoothA2DPSink`; через нього також працюють окремі кнопки `Next`
і `Previous`. `rotary_input` знає лише про фізичний encoder і цей
API, без залежності від ESP32-A2DP headers. Bluetooth callback оновлює тільки
короткий стан під critical section, а UART logging виконується пізніше з
Arduino `loop()`.

Локальна зміна використовує штатний `BluetoothA2DPSink::set_volume()`, а
зміна з телефона надходить через `set_on_volumechange()`. Тому повзунок
телефона й енкодер синхронізуються через AVRCP Absolute Volume. Телефон також
має підтримувати Absolute Volume; без цієї підтримки повна двостороння
синхронізація не гарантується. Аналоговий потенціометр підсилювача залишається
незалежним фізичним обмеженням максимальної гучності.

## Кнопки Previous, Next і Bluetooth

Використовуються три звичайні чотириконтактні тактові кнопки, а не готові
модулі. У такій кнопці дві ніжки з одного електричного боку постійно з'єднані;
GPIO та GND треба підключати до протилежних електричних сторін. Для кожної
кнопки схема однакова:

```text
GPIO → button → GND
```

| Кнопка | GPIO | Інша сторона |
| --- | ---: | --- |
| Previous | GPIO13 | GND |
| Next | GPIO14 | GND |
| Bluetooth | GPIO23 | GND |

Зовнішнє живлення кнопкам не потрібне: не підключайте їх до 3.3 V або 5 V.
Прошивка використовує `INPUT_PULLUP`, тому для коротких проводів прототипу
зовнішні pull-up резистори не потрібні. Відпущений рівень — HIGH, натиснутий —
LOW. Кожна кнопка має окремий `OneButton`, debounce 40 ms і лише single-click;
утримання не створює повторів.

Previous і Next проходять через `media_control` та штатні
`BluetoothA2DPSink::previous()` / `next()`. Bluetooth-кнопка запускає
неблокуючу state machine у `bluetooth_audio`: запит → очікування асинхронного
disconnect → connectable/discoverable → normal після наступного connection.
Використовуються public API `disconnect()`, `set_discoverability()` та
`set_connectable()`; NVS і bonding database не очищаються.

`disconnect()` у ESP32-A2DP 1.8.11 вимикає приватний runtime-latch локального
auto reconnect, але залишає політику `AutoReconnect`. Завдяки цьому телефон,
який підключиться у manual pairing mode, стає новим `last_bda`. Після pairing
прошивка один раз використовує public `reconnect()` при наступному звичайному
disconnect, щоб знову ввімкнути штатний retry-механізм у поточному runtime.
Після power cycle звичайний `set_auto_reconnect(true, 5)` працює без змін.

Обмеження: раніше спарений телефон може сам ініціювати вхідне з'єднання, поки
колонка discoverable. На цьому етапі blacklist адрес не реалізується, тому таке
з'єднання також завершує manual pairing mode.

## Підключення PCM5102A

Перед монтажем знеструмте ESP32, DAC і підсилювач. Земля всіх модулів має бути
спільною.

| PCM5102A | Підключення |
| --- | --- |
| VCC | 5V |
| GND | ESP32 GND |
| BCK | GPIO26 |
| LCK / WS | GPIO25 |
| DIN | GPIO19 |
| XMT | 3.3V |
| FLT | GND (LOW) |
| DMP | GND (LOW, de-emphasis off) |
| SCL | GND (LOW) |
| FMT | GND (LOW, I2S format) |

FLT, DMP, SCL і FMT вище фіксують поточне фізично перевірене strap-підключення
модуля. Прошивка цими виводами не керує.

## Status LED

LED керується неблокуючою state machine на `millis()`:

| Стан | Індикація GPIO2 |
| --- | --- |
| Ініціалізація | 100 ms ON / 100 ms OFF |
| Очікування Bluetooth | 500 ms ON / 500 ms OFF |
| Manual pairing / discoverable | 150 ms ON / 150 ms OFF |
| Bluetooth підключений, аудіо не грає | постійно ON |
| Аудіо грає | OFF 100 ms на початку кожного 2 s циклу |
| Помилка ініціалізації | три швидкі 100 ms спалахи щосекунди |

Bluetooth callbacks лише оновлюють короткі стани/прапорець. Логування та LED
обробляються в `loop()` без `delay()`.

## Auto reconnect

Перед `start()` прошивка викликає публічний API
`set_auto_reconnect(true, 5)`. ESP32-A2DP 1.8.11 самостійно:

1. ініціалізує NVS;
2. зберігає BDA останнього успішно підключеного A2DP source у namespace
   `connected_bda` під ключем `last_bda`;
3. читає адресу під час наступного `start()` після reboot;
4. запускає reconnect із обмеженням у п'ять спроб;
5. після невдалих спроб повертає Bluetooth у connectable mode.

Application code не дублює MAC-адресу через `Preferences` і не змінює bonding,
який зберігає Bluedroid. Реалізацію підтверджено у локально встановлених файлах:

- [`BluetoothA2DPSink.h`](.pio/libdeps/speaker-arduino/ESP32-A2DP/src/BluetoothA2DPSink.h) — overload `set_auto_reconnect(bool, int)` та ключ `last_bda`;
- [`BluetoothA2DPSink.cpp`](.pio/libdeps/speaker-arduino/ESP32-A2DP/src/BluetoothA2DPSink.cpp) — читання адреси у `start()`, запис після connection і обмежені retry;
- [`BluetoothA2DPCommon.cpp`](.pio/libdeps/speaker-arduino/ESP32-A2DP/src/BluetoothA2DPCommon.cpp) — NVS namespace `connected_bda`, blob read/write і reconnect timeout;
- [`BluetoothA2DPOutput.cpp`](.pio/libdeps/speaker-arduino/ESP32-A2DP/src/BluetoothA2DPOutput.cpp) — передача negotiated sample rate до AudioTools.

Ці шляхи з'являються після першого Build. Файли в `.pio/libdeps` не
редагуються проєктом.

## Build, Upload і Monitor у PlatformIO GUI

1. Відкрийте кореневу папку цього проєкту у VS Code.
2. Відкрийте **PlatformIO → Project Tasks**.
3. Розгорніть **speaker-arduino**.
4. Для компіляції виберіть **General → Build**.
5. Під'єднайте ESP32 через USB і виберіть **General → Upload**.
6. Після успішного Upload відкрийте **General → Monitor**.
7. Перевірте швидкість `115200` baud і натисніть **EN/RESET** на ESP32.

Очікуваний startup banner:

```text
FAITAL_SPEAKER firmware=arduino-production
Bluetooth name: Faital Bluetooth Speaker
ESP32-A2DP: 1.8.11
AudioTools: 1.2.5
I2S: Philips, 44100 Hz initial, s16le stereo
Pins: BCK=26 WS=25 DATA=19
Status LED: GPIO2 active-high
Auto reconnect: enabled
MEDIA: AVRCP controls initialized
MEDIA: volume range=0..127 step=4
ROTARY: A=GPIO32 B=GPIO33 COMMON=GND
ROTARY: button=GPIO18 active_low pullup=internal
ROTARY: PCNT encoder initialized
BUTTONS: Previous=GPIO13 Next=GPIO14 Bluetooth=GPIO23
BUTTONS: active_low pullup=internal debounce=40ms
BUTTONS: initialized
```

Під час роботи очікуються короткі подієві повідомлення
`MEDIA: local volume request=<value>`,
`MEDIA: remote volume confirmed=<value>`, `MEDIA: play requested` і
`MEDIA: pause requested`. Для кнопок та pairing state machine також
виводяться `BUTTONS: ... pressed`, `MEDIA: next/previous requested` і
`BT: ...`. Сирі переходи A/B та поточні GPIO-рівні не логуються.

## Фізичний тест

Фізичний PASS нових кнопок ще не оголошено. Після Upload виконайте повний цикл:

1. Перевірте енкодер, синхронізацію гучності та Play/Pause після змін.
2. Підключіть телефон і запустіть музику.
3. Один раз натисніть Next — має бути рівно один перехід.
4. Один раз натисніть Previous — має бути рівно один перехід.
5. Утримуйте кожну кнопку й переконайтеся, що серія команд не генерується.
6. Натисніть Bluetooth: музика має зупинитися, телефон від'єднатися, LED —
   перейти на 150/150 ms, а колонка лишитися доступною для підключення.
7. Підключіть інший телефон або вручну повторно підключіть пристрій.
8. Перевірте вихід із pairing mode та повернення LED до connected state.
9. Повторно перевірте Next/Previous, енкодер і Play/Pause.
10. Виконайте power cycle і перевірте штатний auto reconnect.
11. Натисніть Bluetooth без активного з'єднання й перевірте discoverable mode.

## Future work

Для майбутньої міграції без Arduino-бібліотек зовнішні application-level API
модулів зберігаються, а реалізації можна замінити так:

- `ESP32Encoder` → native ESP-IDF PCNT;
- `OneButton` → GPIO + `esp_timer` або FreeRTOS timer.
- `media_control` → native ESP-IDF AVRCP;
- `bluetooth_audio` pairing API залишається application-level контрактом.

NVS для гучності, mute, acceleration та окрема FreeRTOS task навмисно не
додаються на цьому етапі.
