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
| Adafruit SSD1306 | `2.5.17` |
| Adafruit GFX Library | `1.12.6` |
| Adafruit NeoPixel | `1.13.0` |
| Adafruit BusIO (transitive) | `1.17.4` |

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
| WS2812 DATA via SN74AHCT125N | 27 | output, 800 kHz GRB |
| OLED SDA | 21 | I²C data |
| OLED SCL | 22 | I²C clock |
| Battery ADC | 34 | ADC1 input-only, 12-bit, 11 dB attenuation |

I2S налаштовано як TX master, Philips I2S, signed 16-bit little-endian PCM,
stereo, з початковою частотою 44100 Hz. Після A2DP negotiation
`BluetoothA2DPSink` передає нову частоту через штатний AudioTools adapter до
`I2SStream::setAudioInfo()`; application code не ресемплює й не перекладає
байти або канали вручну.

GPIO19 є обов'язковим DATA-виходом цієї конфігурації. Перенесення DATA з
GPIO22 на GPIO19 усунуло нерівне, «вібруюче» відтворення, яке відтворювалося з
GPIO22 на двох ESP32. Це результат попереднього апаратного дослідження; нова
production-прошивка ще потребує фізичного тесту після очищення проєкту.

## Батарея 3S2P та OLED

`battery_monitor` і `battery_display` ізольовані від Bluetooth-аудіотракту.
Перший модуль працює лише з ADC1 GPIO34, фільтрує та публікує вимірювання;
другий працює лише з I²C/OLED і ніколи не читає ADC. Обидва викликаються з
Arduino `loop()` без окремих FreeRTOS tasks і без `delay()`.

Дільник підключається тільки до загальної напруги пакета після запобіжника та
головного вимикача. Не підключайте ADC до балансувальних точок BMS:

```text
BAT+ after fuse and main switch
        |
      120 kΩ
        |
        +------ GPIO34 / ADC1
        |
        +------ 27 kΩ ------ GND
        |
        +------ 100 nF ----- GND
```

GND дільника, ESP32, OLED та решти системи мають бути спільними. За 12.6 V на
пакеті на GPIO34 очікується приблизно 2.31 V. Заборонено подавати 12.6 V
безпосередньо на ESP32: це значно перевищує допустиму напругу ADC і пошкодить
мікроконтролер.

Прошивка використовує каліброване `analogReadMilliVolts()`, 12-bit resolution
і `analogSetPinAttenuation(GPIO34, ADC_11db)`. Формула перерахунку:

```text
Vbat = Vadc × ((120000 + 27000) / 27000) × calibration_gain
       + calibration_offset
     = Vadc × 5.444444... × calibration_gain + calibration_offset
```

Замість одиночного шумного ADC-read накопичуються 20 вимірювань по одному за
виклик `update()`. Два найменші та два найбільші значення відкидаються,
решта усереднюється, після чого до напруги пакета застосовується повільний EMA.
Стабільний результат публікується приблизно раз на секунду; heap allocation у
періодичному шляху немає.

Відсоток є приблизною оцінкою за напругою, а не coulomb counter. Під
навантаженням підсилювача напруга просідає, після Pause може трохи відновитися,
тому точність відсотка нижча за точність показаної напруги. Використовуються
такі точки з лінійною інтерполяцією між ними та clamp у 0…100%:

| Напруга пакета | Оцінка |
| ---: | ---: |
| 12.60 V | 100% |
| 12.30 V | 90% |
| 12.00 V | 75% |
| 11.70 V | 55% |
| 11.40 V | 35% |
| 11.10 V | 20% |
| 10.80 V | 10% |
| 10.20 V | 0% |

LOW BATTERY вмикається за напруги `≤ 10.8 V` і вимикається лише за
`≥ 11.1 V`; гістерезис 0.3 V не дає попередженню блимати через басові піки.
Якщо напруга поза фізично розумним діапазоном 8.0…13.2 V, ADC повернув 0 чи
неможливе значення або дільник від'єднано, стан стає invalid. OLED показує
`BATTERY ERROR` і `--.- V`, UART-помилка обмежена за частотою, а Bluetooth та
керування продовжують працювати.

OLED SSD1306 128×64 з адресою `0x3C` живиться тільки від 3.3 V:

| OLED | ESP32 |
| --- | --- |
| GND | GND |
| VCC | 3.3V |
| SCL | GPIO22 |
| SDA | GPIO21 |

Якщо `0x3C` не відповідає або ініціалізація дисплея невдала, прошивка записує
один короткий лог і продовжує запуск Bluetooth speaker без OLED.

### Калібрування мультиметром

1. Виміряйте реальну напругу всього пакета мультиметром.
2. Порівняйте її зі значенням `pack` у UART.
3. Обчисліть `gain = voltage_multimeter / voltage_firmware`.
4. Запишіть коефіцієнт у `kBatteryCalibrationGain` у `src/app_config.h`;
   `kBatteryCalibrationOffsetV` зазвичай лишається `0.0f`.
5. Повторіть перевірку щонайменше у двох точках заряду. Якщо похибка має
   сталий зсув, після перевірки gain скоригуйте offset окремо.

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

## WS2812/NeoPixel: індикація стану

Лінійка з 8 адресних RGB LED є основною індикацією стану колонки. Прошивка
використовує `Adafruit NeoPixel@1.13.0`, порядок каналів `GRB`, протокол 800 kHz і
програмне обмеження глобальної яскравості `48/255`. Повний білий на максимальній
яскравості не використовується. OLED, як і раніше, показує лише батарею; LOW BATTERY
і відсоток заряду на WS2812 не дублюються.

Стани лінійки:

- `initializing` — коротка тепла жовто-помаранчева рухома точка;
- `waiting` — спокійна синя рухома точка з коротким приглушеним хвостом; сюди
  входять disconnected, pairing і auto-reconnect;
- `connected-idle` — постійний слабкий cyan із одноразовою плавною появою;
- `playing` — повільне integer-based синьо-зелене breathing без аналізу PCM.

Анімація виконується неблокуюче з Arduino `loop()` через `millis()`: ініціалізація
оновлюється кожні 100 ms, очікування — 150 ms, fade і playing — 40 ms (не більше
25 кадрів/с). `show()` викликається лише тоді, коли фактичний кадр змінився. Для 8
RGB-пікселів передавання 24 біт на LED при 800 kHz займає приблизно 240 µs плюс
latch. У цій платформі NeoPixel 1.13.0 використовує ESP-IDF 4.x legacy RMT backend,
який синхронно чекає завершення короткої передачі; тому після монтажу обов'язковий
фізичний audio regression test.

GPIO2 більше не кодує Bluetooth-стан. Це active-high heartbeat: імпульс 50 ms раз
на 1000 ms, який генерує лише `StatusLed::update()` у головному
`loop()`. Якщо `loop()` перестає виконуватися, послідовність heartbeat-пульсів також
припиняється.

### Підключення через SN74AHCT125N (DIP-14)

WS2812 живиться від окремих стабілізованих 5 V. Не керуйте DIN безпосередньо з
GPIO27 у production-схемі. Земля блока 5 V, ESP32, SN74AHCT125N і WS2812 має бути
спільною.

Для першого каналу SN74AHCT125N:

| DIP-14 pin | Назва | Підключення |
| ---: | --- | --- |
| 1 | `/1OE` | GND, постійно дозволяє канал |
| 2 | `1A` | ESP32 GPIO27; цей вузол через 10 kΩ до GND |
| 3 | `1Y` | через 330 Ω до DIN першого WS2812 |
| 7 | GND | спільний GND |
| 14 | VCC | стабілізовані 5 V |

Невикористані канали треба вимкнути: `/2OE` pin 4, `/3OE` pin 10 і `/4OE` pin 13
під'єднати до 5 V; входи `2A` pin 5, `3A` pin 9 і `4A` pin 12 — до GND; виходи
`2Y` pin 6, `3Y` pin 8 і `4Y` pin 11 залишити непідключеними. Біля мікросхеми між
pin 14 і pin 7 встановити керамічний 100 nF. Біля лінійки між 5 V і GND встановити
електролітичний 1000 µF із правильною полярністю. Резистор 330 Ω має стояти якомога
ближче до DIN/лінії даних.

Між вузлом `ESP32 GPIO27 / SN74AHCT125N pin 2 (1A)` та GND обов'язково встановити
апаратний pulldown 10 kΩ. Оскільки `/1OE` (pin 1) постійно під'єднаний до GND і
канал завжди активний, pulldown утримує вхід level shifter у LOW під час reset і
до того, як прошивка переведе GPIO27 у `OUTPUT LOW`, запобігаючи startup glitch.

Вісім WS2812 при повному білому можуть споживати близько 480 mA. Програмне
обмеження яскравості зменшує робоче споживання, але джерело 5 V, проводка й захист
мають бути розраховані на повний апаратно можливий струм із запасом.

### Фізичний тест WS2812 і аудіо

Фізичний PASS цієї зміни до перевірки на платі не оголошено. Після Upload:

1. Перевірте UART-рядки `STATUS LED`, `LED STRIP` і одноразові повідомлення про
   переходи стану; кадри та номери пікселів логуватися не повинні.
2. Без телефона перевірте синю `waiting`-анімацію та короткий heartbeat GPIO2.
3. Підключіть телефон: має з'явитися слабкий cyan `connected-idle`.
4. Запустіть, призупиніть і відновіть музику: `playing` має відповідати лише
   підтвердженому callback-стану `STARTED`.
5. Відтворюйте музику 10–15 хвилин, одночасно перевіряючи енкодер, Play/Pause,
   Next/Previous, OLED і reconnect. Не повинно бути тріску, drop-out, reboot або
   помітної затримки керування під час оновлення лінійки.
6. Зупиніть виклики головного `loop()` лише в контрольованій debug-збірці й
   переконайтеся, що heartbeat більше не пульсує; production-код для цього не
   модифікуйте.

Очікувані додаткові UART-переходи:

```text
LED STRIP: state=waiting
LED STRIP: state=connected-idle
LED STRIP: state=playing
```

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
STATUS LED: GPIO=2 mode=heartbeat period=1000 ms pulse=50 ms
LED STRIP: GPIO=27 count=8 order=GRB brightness=48
LED STRIP: Adafruit NeoPixel initialized
LED STRIP: state=initializing
Auto reconnect: enabled
BATTERY: ADC GPIO=34 divider=120000/27000
BATTERY: ADC resolution=12 attenuation=ADC_11db
BATTERY: calibration gain=1.000000 offset=0.000 V
DISPLAY: SSD1306 128x64 address=0x3C SDA=21 SCL=22
DISPLAY: initialized
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

Фізичний PASS нової battery/OLED реалізації не оголошено. Після Upload
виконайте повний цикл:

1. Перевірте startup banner і наявність OLED за адресою `0x3C`.
2. Мультиметром виміряйте напругу пакета та напругу безпосередньо на GPIO34;
   остання має відповідати дільнику і ніколи не перевищувати допустимий ADC
   діапазон.
3. Порівняйте мультиметр із `adc`, `pack` у UART та напругою на OLED; за
   потреби виконайте описане вище калібрування.
4. Перевірте відсоток, заповнення іконки, LOW BATTERY та invalid state на
   контрольованому лабораторному джерелі або безпечних рівнях заряду пакета.
5. Запустіть музику на 2–3 хвилини й перевірте, що показання не стрибають від
   басових піків, звук лишається чистим, а керування не затримується.
6. Вимкніть і ввімкніть колонку; перевірте auto reconnect та повторну
   ініціалізацію OLED.
7. Від'єднайте OLED і окремо сигнальний провід дільника лише при знеструмленій
   системі; після ввімкнення Bluetooth має працювати, а відповідні UART-помилки
   не повинні спричиняти зависання або reboot.

Після battery/OLED перевірок повторіть regression-тест керування:

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
- Arduino ADC API → ESP-IDF ADC oneshot + calibration;
- `Wire` → ESP-IDF I²C master;
- Adafruit SSD1306/GFX → окремий ESP-IDF-compatible SSD1306 renderer;
- Adafruit NeoPixel → native ESP-IDF RMT/LED-strip driver зі збереженням API `led_strip`;
- API `battery_monitor` і `battery_display` лишаються application-level
  контрактами.

NVS для гучності, mute, acceleration та окрема FreeRTOS task навмисно не
додаються на цьому етапі.
