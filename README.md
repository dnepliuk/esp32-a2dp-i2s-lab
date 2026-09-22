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

Обидві бібліотеки беруться з офіційних репозиторіїв GitHub. Перевірені commit
ID наведені у `dependencies.lock`. Плата `esp32doit-devkit-v1` має 4 MB flash;
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

I2S налаштовано як TX master, Philips I2S, signed 16-bit little-endian PCM,
stereo, з початковою частотою 44100 Hz. Після A2DP negotiation
`BluetoothA2DPSink` передає нову частоту через штатний AudioTools adapter до
`I2SStream::setAudioInfo()`; application code не ресемплює й не перекладає
байти або канали вручну.

GPIO19 є обов'язковим DATA-виходом цієї конфігурації. Перенесення DATA з
GPIO22 на GPIO19 усунуло нерівне, «вібруюче» відтворення, яке відтворювалося з
GPIO22 на двох ESP32. Це результат попереднього апаратного дослідження; нова
production-прошивка ще потребує фізичного тесту після очищення проєкту.

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
```

## Фізичний тест

Фізичний PASS ще не оголошено. Після Upload виконайте повний цикл:

1. Натисніть reset і перевірте banner та повільне блимання LED.
2. Уперше спарте телефон з **Faital Bluetooth Speaker**; LED має засвітитися
   постійно.
3. Відтворюйте локальний тон 1000 Hz протягом 30 секунд на низькій безпечній
   гучності; звук має бути рівним, без вібрації, пропусків або зміни тону.
4. Відтворюйте музику 2–3 хвилини.
5. Перевірте pause/resume та відповідні LED/log transitions.
6. Вимкніть і знову ввімкніть ESP32; телефон має автоматично підключитися до
   збереженого пристрою.
7. Вимкніть Bluetooth телефону; ESP32 не повинна зависнути, а LED має
   повернутися до режиму очікування. Увімкніть Bluetooth і перевірте повторне
   підключення.

## Future work

Наступні функції навмисно не входять до цієї бази: encoder, кнопки
Play/Pause/Next/Previous, керування гучністю та amplifier mute/enable. Їх слід
додавати лише після фізичного PASS базового аудіотракту.
