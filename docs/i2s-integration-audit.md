# Аудит інтеграції ESP32-A2DP та AudioTools I2S

Дата аудиту: 2026-09-20.

## Висновок

Наявні вимірювання локалізують проблему до одночасної роботи Bluetooth-декодування, upstream `BluetoothA2DPSinkQueued` та активного I2S, але не доводять конкретну першопричину.

Підтверджено таке:

- локальний `I2SStream` стабільно передає приблизно `176400 B/s`, тому базова I2S-конфігурація та апаратний тракт справні;
- Bluetooth без реального I2S дає потрібний середній PCM rate як у direct, так і в queued варіанті;
- у проблемному варіанті обидва видимі лічильники — accepted producer bytes і written consumer bytes — падають до приблизно `145000–150000 B/s`; це не схоже лише на повільний blocking `I2SStream::write`, бо виміряний виклик зазвичай триває лише десятки мікросекунд;
- `set_output_active_by_state(false)` і APLL не усувають дефект;
- початковий `i2s.begin(config)` та `a2dp_sink.start()` справді спричиняють дві інсталяції legacy I2S driver. Другий `begin()` зберігає налаштовані піни, TX-only, формат, DMA та APLL, але upstream не перевіряє його результат, а AudioTools 1.2.5 може повернути `true` навіть після помилки install/pin;
- наявність legacy I2S символів ESP32-A2DP в ELF не означає, що цей альтернативний output працює: конструктор з `AudioStream&` у runtime вибирає AudioTools output adapter;
- `format=0Hz/0ch/0bit` у null-probe є дефектом діагностики, не доказом несправного `setAudioInfo` forwarding.

Тому оголошувати винним AudioTools, ISR, scheduler або ESP-IDF I2S driver поки рано. Найцінніший наступний тест — direct проти queued з тим самим реальним `CountedI2SStream`; він змінює лише клас sink і покаже, чи потрібна upstream queue для відтворення дефекту.

## Межі та джерела

Аудит виконано без зміни firmware, `platformio.ini` або upstream-бібліотек, без build, Upload і Monitor. Єдина створена сутність — цей документ.

Закріплені версії підтверджені у `dependencies.lock:1-24`:

- PlatformIO `espressif32 7.1.3`;
- Arduino-ESP32 `2.0.17`, ESP-IDF base `4.4.7`;
- ESP32-A2DP `1.8.11`, commit `b6da4744286ca15b6f1dee607c077a4747294dd4`;
- AudioTools `1.2.5`, commit `2390c44fac3bdd1df40b80a088f00c9a203053b9`.

Локальні upstream-посилання нижче використовують скорочення:

- `A2DP/` = `.pio/libdeps/a2dp-queued-rate-probe/ESP32-A2DP/src/`;
- `AT/` = `.pio/libdeps/a2dp-queued-rate-probe/audio-tools/src/`;
- `SDK/` = `C:/Users/1dima/.platformio/packages/framework-arduinoespressif32/`.

Внутрішня реалізація Bluedroid/SBC та реалізація ESP-IDF legacy I2S driver у встановленому framework постачаються у prebuilt archives. Доступні публічні headers, sdkconfig та виклики бібліотек, але не весь код усередині цих двох компонентів. Там, де це важливо, висновок нижче позначено як inference, а не як безпосередньо прочитаний факт.

### Вхідні фізичні вимірювання

Ці результати надані як уже підтверджені користувачем; сирі UART logs у workspace не збережені, тому аудит не переобчислював їх:

| Тракт | Спостереження |
|---|---|
| local tone → I2S | чистий 1000 Hz при 10%, 50%, 90%; близько 176400 B/s; short/error = 0 |
| Bluetooth → direct null | повні інтервали 174454–177730 B/s, середнє близько 176400 B/s; max gap 78 ms; без overflow/Bluetooth warnings |
| Bluetooth → queued null | producer 176056–176877 B/s; rejected = 0; pending snapshots 0–3872 B без накопичення; max consumer gap 62 ms; після pause accepted = consumed = 15028224 B; без overflow/semaphore warnings |
| Bluetooth → queued → real I2S | producer і consumer переважно 145000–150000 B/s; pending регулярно повертається до нуля; є overflow/drop/semaphore warnings; short/error = 0; середній measured `I2SStream::write` близько 13–62 us; tone звучить як «Морзе», музика рветься |
| always-on output | прибрав повторні stop/start при Pause/Resume, але не виправив звук або PCM deficit |
| APLL | `use_apll=true` підтверджено, істотної зміни результату немає |

Те, що `pending_estimate` повертається до нуля, не суперечить drop warnings: rejected packets не входять у cumulative accepted, а prefetch/drop state machine може циклічно спорожнювати queue після відкидання даних.

## 1. Точний PCM path

### 1.1 Від декодера до upstream ring buffer

1. ESP32-A2DP реєструє `ccall_audio_data_callback` через `esp_a2d_sink_register_data_callback()` у `A2DP/BluetoothA2DPSink.cpp:1083-1089`. ESP-IDF 4.4 документує, що callback отримує вже декодований SBC PCM і виконується в контексті A2DP sink task. Локальний wrapper передає дані в `BluetoothA2DPSink::audio_data_callback()` (`A2DP/BluetoothA2DPSink.cpp:29-33`).

2. A2DP sink task належить Bluedroid. Встановлений sdkconfig має `CONFIG_BT_BLUEDROID_PINNED_TO_CORE=0` та `CONFIG_BTDM_CTRL_PINNED_TO_CORE=0` (`SDK/tools/sdk/esp32/sdkconfig:343-345,384-387`). Отже decoded callback працює на core 0. Точне ім'я/priority внутрішньої A2DP task не видно, бо Bluedroid prebuilt; stack size у конфігурації — 8192 bytes.

3. `audio_data_callback()` послідовно:

   - опційно міняє L/R місцями;
   - викликає raw callback, якщо заданий;
   - викликає `volume_control()->update_audio_data((Frame*)data, len / 4)`;
   - викликає post-volume stream callback, якщо заданий;
   - virtual `write_audio(data, len)`.

   Код: `A2DP/BluetoothA2DPSink.cpp:1155-1192`.

4. Default volume path не бере `s_volume_lock` на кожному PCM block. Гарячий цикл у `A2DP/A2DPVolumeControl.h:75-94` взагалі нічого не робить, доки не ввімкнений volume або mono-downmix; після ввімкнення обробляє кожен stereo frame. `_lock_acquire()` є лише в операціях зміни значення гучності (`A2DP/BluetoothA2DPSink.cpp:1347-1369`). Отже shared volume lock не пояснює постійну затримку кожного PCM packet. Водночас queued-null проходить той самий volume path і був стабільний, тому сам volume loop не узгоджується з різницею null/I2S.

5. У rate-probe virtual dispatch доходить до `ProbedBluetoothA2DPSinkQueued::write_audio()` (`src/a2dp_rate_probe_common.h:91-110`). Метод спочатку викликає upstream `BluetoothA2DPSinkQueued::write_audio()`, а вже потім оновлює producer counters.

6. Upstream producer виконує `xRingbufferSend(..., 0)` — timeout нуль, тобто enqueue не блокується (`A2DP/BluetoothA2DPSinkQueued.cpp:91-140`). Return semantics точні для цієї версії: `size`, якщо весь block прийнятий, і `0`, якщо не прийнятий. Partial accept upstream не повертає.

7. Producer counter `accepted` тому означає саме bytes, прийняті upstream ring buffer. Однак timestamp entry у decoded callback не вимірюється: оскільки counter оновлюється після upstream call, будь-яка затримка всередині base method входить у часову поведінку producer, але не виділяється окремо. У поточному processing mode enqueue timeout нуль; додаткові `200 ms` можливі лише в аварійній гілці `!is_i2s_active` (`A2DP/BluetoothA2DPSinkQueued.cpp:96-101`).

### 1.2 Upstream queue та consumer

8. Під час Bluetooth connection upstream створює queue task у `BluetoothA2DPSink::handle_connection_state()` (`A2DP/BluetoothA2DPSink.cpp:776-785`). `BluetoothA2DPSinkQueued` створює byte ring buffer і `BtI2STask` (`A2DP/BluetoothA2DPSinkQueued.cpp:6-23`).

9. Фактичні defaults ESP32-A2DP 1.8.11 (`A2DP/BluetoothA2DPSinkQueued.h:7-8,79-111`):

   - ring buffer: 32768 bytes;
   - prefetch: 65%, вирівняний до 4 bytes;
   - consumer stack: 2048 bytes;
   - consumer priority: `configMAX_PRIORITIES - 3` = 22, бо SDK має `configMAX_PRIORITIES=25`;
   - receive upper bound: `240 * 6` = 1440 bytes;
   - receive timeout: 20 ticks;
   - inherited `task_core`: 1 (`A2DP/BluetoothA2DPCommon.h:402-410`).

   `CONFIG_FREERTOS_HZ=1000`, тому 20 ticks = 20 ms (`SDK/tools/sdk/esp32/sdkconfig:1144`). Це timeout очікування відсутніх даних, а не обов'язкова пауза після кожного успішного receive.

10. На старті consumer чекає prefetch semaphore з `portMAX_DELAY`. Далі отримує до 1440 bytes через `xRingbufferReceiveUpTo(..., pdMS_TO_TICKS(20), 1440)`, викликає `i2s_write_data()`, повертає item через `vRingbufferReturnItem()`, а потім безумовно виконує `delay_ms(5)` (`A2DP/BluetoothA2DPSinkQueued.cpp:45-88`). В Arduino build `delay_ms()` викликає `delay()` (`A2DP/BluetoothA2DPCommon.cpp:609-616`), тобто task yields приблизно на 5 ticks.

11. Максимум 1440 bytes на одну успішну ітерацію плюс 5 ms дає грубу верхню межу `288000 B/s` без урахування write та scheduling. Це вище `176400 B/s`, а queued-null з тими самими defaults реально тримає потрібний rate. Отже сама константа 5 ms не є достатнім поясненням. Але ефективний ceiling залежить від фактичного середнього `item_size`: наприклад, 768 bytes на ітерацію вже дають близько `153600 B/s`. Поточний звіт не виводить `requested_delta / consumer_calls_delta` як окреме поле, хоча ці величини в рядку є і це можна порахувати з UART log.

12. Ring-buffer item повертається лише після завершення всього `i2s_write_data()`. При `written == 0` код робить `continue` до `vRingbufferReturnItem()` (`A2DP/BluetoothA2DPSinkQueued.cpp:76-87`), тобто item не повертається. Це підтверджений upstream defect, але в наявному тесті `errors=0`, тому він не доведений як активна причина.

### 1.3 Output adapter та фактичний I2S write

13. `BluetoothA2DPSinkQueued(audio_tools::AudioStream&)` передає stream у default output (`A2DP/BluetoothA2DPSinkQueued.h:58-68`). `BluetoothA2DPOutputAudioTools::set_output(AudioStream&)` зберігає:

   - `p_print = &output` для PCM writes;
   - `p_audio_print = &AdapterAudioStreamToAudioOutput` для lifecycle/AudioInfo.

   Код: `A2DP/BluetoothA2DPOutput.h:87-100`.

14. `BluetoothA2DPOutputDefault::write()` вибирає `out_tools`, коли adapter валідний (`A2DP/BluetoothA2DPOutput.h:203-221`). AudioTools output пише безпосередньо в `p_print->write()` (`A2DP/BluetoothA2DPOutput.cpp:207-215`). Тому PCM virtual dispatch дійсно входить у `CountedI2SStream::write()`.

15. `CountedI2SStream::write()` вимірює час навколо явного виклику `audio_tools::I2SStream::write(data, len)`, додає requested і фактично повернуті bytes та повертає той самий результат (`src/a2dp_rate_probe_common.h:53-88`). Отже counters не замінюють I2S write і рахують потрібні bytes.

16. `BluetoothA2DPSink::i2s_write_data()` ділить block на частини до `max_write_size=5120`, викликає output write, віднімає фактичний return і після кожної частини робить `delay_ms(max_write_delay_ms)`, default 0 (`A2DP/BluetoothA2DPSink.cpp:1296-1315`, `A2DP/config.h:25-30`). Queue вже обмежує item до 1440 bytes, тому у штатному queued path додаткового splitting немає.

17. AudioTools `I2SStream::write()` перевіряє active/data/len і викликає legacy driver `writeBytes()` (`AT/AudioTools/CoreAudio/AudioI2S/I2SStream.h:117-122`). Для stereo немає channel expansion. Driver викликає ESP-IDF `i2s_write(..., ticks_to_wait_write)`, default `portMAX_DELAY`, і повертає `bytes_written` (`AT/AudioTools/CoreAudio/AudioI2S/I2SESP32.h:94-110,157-163`). За документацією IDF це blocking wait без timeout; short return можливий при timeout, але тут timeout нескінченний. `ESP_ERR_*` лише trace-логується, а caller отримує кількість bytes.

18. Return handling має ще два підтверджені дефекти:

   - `i2s_write_data()` не має guard для `written == 0` або `written > open`; zero залишає `open` незмінним і створює нескінченний loop;
   - queued consumer перевіряє тільки `written == 0`; якби `i2s_write_data()` повернув partial nonzero, item був би повернений повністю і tail втрачений. За теперішньої внутрішньої retry-loop частіше буде або повний результат, або зависання на zero, але контракт коду все одно некоректний.

Поточні `short=0, errors=0` означають лише, що спостережувані виклики base `I2SStream::write()` повертали повний `len`. Це не доводить, що DMA фізично передала всі samples: IDF `i2s_write` підтверджує копіювання у TX DMA buffers, а не завершення передачі на BCK/WS.

## 2. Task/core, блокування і locks

| Компонент | Core / priority | Підтвердження | Blocking/yield |
|---|---:|---|---|
| Arduino `setup()/loop()` | core 1, loop priority 1 | `CONFIG_ARDUINO_RUNNING_CORE=1`; `cores/esp32/main.cpp:71` | application `delay()` |
| BT controller | core 0 | sdkconfig `343-345` | prebuilt |
| Bluedroid/A2DP sink callback | core 0; internal priority не встановлено з доступного source | IDF callback-context contract + `CONFIG_BT_BLUEDROID_PINNED_TO_CORE=0` | prebuilt decoder/task |
| ESP32-A2DP `BtAppT` event task | core 1, priority 15, stack 3072 | `A2DP/BluetoothA2DPCommon.cpp:480-486`; defaults `Common.h:402-410` | queue receive in event loop |
| `BtI2STask` queued consumer | core 1, priority 22, stack 2048 | `A2DP/BluetoothA2DPSinkQueued.cpp:17-23`; `Queued.h:104-110` | initial semaphore forever; receive 20 ms; 5 ms delay after item; I2S write forever |
| I2S interrupt | імовірно core 1, але не виміряно | обидві install виконуються з Arduino setup на core 1; IDF interrupt allocator прив'язує external interrupt до core caller | точна `i2s_driver_install` implementation prebuilt |

`intr_alloc_flags=0` означає default interrupt priority, не IRAM (`AT/AudioTools/CoreAudio/AudioI2S/I2SESP32.h:186-196`). За правилом ESP-IDF external interrupt allocation він має бути встановлений на core виклику. Проте, оскільки реалізація `i2s_driver_install` у цьому framework prebuilt і handle не логувався через `esp_intr_get_cpu()`, core 1 є сильно обґрунтованим inference, а не прямим runtime-доказом.

Немає project-level mutex між producer та consumer, окрім:

- FreeRTOS ring-buffer internals;
- binary prefetch semaphore;
- короткого `portMUX` лише навколо діагностичних counters;
- внутрішніх locks prebuilt Bluedroid та I2S driver, які з доступного source не видно.

## 3. I2S lifecycle

### Перша та друга ініціалізація

1. `setupProbe()` створює TX config і викликає `i2s.begin(config)` (`src/a2dp_rate_probe_common.h:167-186`). Конфігурація: 44100 Hz, 16-bit, 2 channels, Philips/I2S, BCK 26, WS 25, DATA 22, RX -1; APLL лише для відповідного variant.

2. Далі `a2dp_sink.start()` (`src/a2dp_rate_probe_common.h:188-191`) завжди викликає `BluetoothA2DPSink::init_i2s()` перед стартом Bluetooth (`A2DP/BluetoothA2DPSink.cpp:139-181`). `init_i2s()` викликає `out->begin()` і безумовно виставляє `is_i2s_active=true`, ігноруючи bool result (`A2DP/BluetoothA2DPSink.cpp:210-215`).

3. AudioTools adapter forwarding викликає `I2SStream::begin()` (`AT/AudioTools/CoreAudio/AudioIO.h:346-371`). No-argument begin бере поточний driver config, замінює лише поля `AudioInfo` і викликає `i2s.begin(cfg)` (`AT/AudioTools/CoreAudio/AudioI2S/I2SStream.h:54-67`). Тому піни, TX-only, Philips format, buffer count/size, auto-clear і APLL зберігаються.

4. На ESP-IDF 4.4.7 `USE_LEGACY_I2S=true` (`AT/AudioTools/PlatformConfig/esp32.h:62-63`). Legacy driver `begin(I2SConfigESP32)` не використовує свій no-op `begin()`: при `is_started` він спочатку викликає `end()`/`i2s_driver_uninstall()`, а потім знову `i2s_driver_install()` і `i2s_set_pin()` (`AT/AudioTools/CoreAudio/AudioI2S/I2SESP32.h:55-74,178-245`). Отже це справді друга інсталяція driver, а не лише повторний log line.

Ця послідовність збігається з офіційним ESP32-A2DP прикладом custom pins: `i2s.begin(cfg); a2dp_sink.start(...)`. Тому сам факт другого begin є штатною поведінкою цієї інтеграції, хоча error handling у конкретних версіях слабкий.

### Ефективна конфігурація після другого begin

- mode: master + TX, RX вимкнений;
- sample rate: 44100 Hz до negotiation;
- signed PCM 16-bit stereo, `I2S_CHANNEL_FMT_RIGHT_LEFT`;
- Philips standard: `I2S_COMM_FORMAT_STAND_I2S`;
- pins: BCK 26, WS 25, DATA out 22, DATA in `I2S_PIN_NO_CHANGE`;
- DMA: 6 buffers × 512 stereo frames. Для s16 stereo це 12288 bytes, приблизно 69.7 ms при 176400 B/s;
- `tx_desc_auto_clear=true`;
- `use_apll=false` у rate-probe, `true` лише в APLL variant;
- write wait: `portMAX_DELAY`;
- I2S port 0;
- interrupt flags 0.

### Negotiated sample rate

`handle_audio_cfg()` спочатку викликає user sample-rate callback, потім `out->set_sample_rate(m_sample_rate)` (`A2DP/BluetoothA2DPSink.cpp:620-628`). AudioTools output читає поточний `audioInfo()` і викликає `setAudioInfo()` лише якщо rate/channels/bits відрізняються (`A2DP/BluetoothA2DPOutput.cpp:229-245`).

При negotiated 44100/2/16 поточний config уже такий самий, тому жодного restart або `i2s_set_sample_rates()` немає. При зміні тільки rate legacy AudioTools driver викликає `i2s_set_sample_rates()` без reinstall (`AT/AudioTools/CoreAudio/AudioI2S/I2SESP32.h:36-50`). Отже гіпотеза 48000-vs-44100 спростовується, якщо UART справді показав negotiated 44100; сам code path автоматичної синхронізації правильний.

Є defect error path: driver присвоює `cfg.sample_rate` до перевірки результату `i2s_set_sample_rates()`. Якщо IDF call поверне error, `I2SStream::setAudioInfo()` побачить, що requested info вже дорівнює driver config, і залогує `no change` замість restart (`AT/.../I2SESP32.h:37-49`, `AT/.../I2SStream.h:94-114`). Це не підтверджена причина поточного 44100 test, де зміни rate немає.

### Begin/install error handling

- project зберігає `i2s_started`, але використовує його лише для APLL banner і не abort-ить (`src/a2dp_rate_probe_common.h:182-186`);
- `main.cpp`, queued та always-on взагалі ігнорують return `i2s.begin()`;
- AudioTools legacy driver логуватиме error від `i2s_driver_install()` або `i2s_set_pin()`, але продовжить, виставить `is_started=true` і поверне `true` (`AT/.../I2SESP32.h:211-244`);
- `BluetoothA2DPSink::init_i2s()` ігнорує bool output begin та виставляє active=true.

Отже `begin()==true` у цих версіях не є надійним доказом успішної install/pin операції.

### Pause/resume

У rate-probe викликано `set_output_active_by_state(false)`. При START `set_i2s_active(true)` не викликає begin, якщо output уже active; при SUSPEND output не деактивується (`A2DP/BluetoothA2DPSink.cpp:639-683`). Це прибирає подальші pause/resume reinstall, але не початковий другий begin. Фізичний always-on тест не виправив rate, тому pause/resume lifecycle не є достатньою причиною.

## 4. Порівняння effective configurations

| Ознака | local-tone | direct-null | queued-null | queued-rate-probe |
|---|---|---|---|---|
| Source | `local_tone.cpp` | `a2dp_direct_null_probe.cpp` | `a2dp_queued_null_probe.cpp` | `a2dp_queued_rate_probe.cpp` + common header |
| Bluetooth/A2DP | ні | upstream direct | upstream queued | upstream queued |
| Реальний I2S | AudioTools I2SStream | ні | ні | AudioTools I2SStream |
| Runtime output | I2SStream | AudioTools adapter → null stream | AudioTools adapter → null stream | AudioTools adapter → counted I2SStream |
| A2DP legacy output compiled | n/a | примусово ні | примусово ні | так, default для IDF < 5 |
| A2DP legacy output used | n/a | ні | ні | ні, бо `out_tools` валідний |
| AudioTools I2S implementation | legacy ESP-IDF API | n/a | n/a | legacy ESP-IDF API |
| Initial I2S begin | один | немає | немає | два installs до playback |
| DMA | 6 × 512, defaults | немає | немає | 6 × 512, defaults |
| APLL | false | n/a | n/a | false |
| Queue defaults | n/a | n/a | 32768/65%/1440/20 ticks/5 ms | ті самі |
| CPU | `F_CPU=240000000L` | 240 MHz | 240 MHz | 240 MHz |
| App/queue cores | loop core 1 | callback core 0, loop core 1 | callback 0, queue 1, loop 1 | callback 0, queue 1, loop/I2S install 1 |
| Logging | core debug 2, без AudioTools Info | core debug 2 | core debug 2 | плюс AudioTools Info |

Всі environments використовують одну board definition, `huge_app.csv`, platform і library pins з `platformio.ini`. Compatibility alias `ESP_A2D_AUDIO_STATE_SUSPEND=ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND` є в усіх Bluetooth environments і лише виправляє API-name compatibility IDF 4.4.7; він не змінює data path.

Два різні macro не слід змішувати:

- `A2DP_LEGACY_I2S_SUPPORT` компілює альтернативний legacy output усередині ESP32-A2DP;
- `USE_LEGACY_I2S` вибирає legacy ESP-IDF driver усередині AudioTools.

У failing variant перший macro дорівнює 1, але runtime використовує `BluetoothA2DPOutputAudioTools`; другий macro теж 1 і саме він визначає фактичний AudioTools → ESP-IDF path. Одночасного runtime запису через два I2S outputs код не показує.

Реальні відмінності queued-null від queued-rate-probe, крім null проти I2S:

- null builds задають `A2DP_LEGACY_I2S_SUPPORT=0`; rate-probe залишає default 1, але цей alternate output runtime не вибраний;
- rate-probe вмикає AudioTools log level Info; null — ні. На Info немає per-PCM logging (`I2SStream::write` має Debug log), однак warning/info під час mode changes можуть додавати UART навантаження;
- rate-probe робить дві I2S install і має DMA/ISR; null — жодної.

Саме остання різниця узгоджується з фізичними результатами, але ще не визначає, чи причина в driver, ISR, scheduler, queue cadence або їх взаємодії.

## 5. Аудит instrumentation

### `format=0Hz/0ch/0bit`

Причина підтверджена:

1. Окремі diagnostic counters `sample_rate/channels/bits_per_sample` починаються з нуля і оновлюються тільки в override `CountingNullAudioStream::setAudioInfo()` (`src/a2dp_null_probe_common.h:13-35,101-108`).
2. Base `AudioStream::audioInfo()` вже має defaults 44100/2/16, бо `AudioInfo` ініціалізується через `DEFAULT_SAMPLE_RATE`, `DEFAULT_CHANNELS`, `DEFAULT_BITS_PER_SAMPLE` (`AT/AudioTools/CoreAudio/AudioTypes.h:55-64`; `AT/AudioToolsConfig.h:85-95`).
3. ESP32-A2DP adapter читає ці defaults і при negotiated 44100/2/16 навмисно не викликає `setAudioInfo()` (`A2DP/BluetoothA2DPOutput.cpp:229-243`).
4. Adapter forwarding справний: `AdapterAudioStreamToAudioOutput::setAudioInfo()` і `audioInfo()` делегують stream (`AT/AudioTools/CoreAudio/AudioIO.h:346-366`).

Отже нулі — лише display/instrumentation defect. PCM rate counters не залежать від цих трьох полів; неправильними є тільки `expected_rate` і `rate_percent`.

Мінімальне майбутнє виправлення: під час ініціалізації diagnostic counters скопіювати `null_output.audioInfo()` або ініціалізувати їх у constructor `CountingNullAudioStream` з inherited `audioInfo()`. Не треба примусово викликати `setAudioInfo()` upstream і не треба hardcode negotiated rate.

### Інтервали та snapshots

- Rate-probe ставить baseline одразу після `a2dp_sink.start()` (`src/a2dp_rate_probe_common.h:193-194`) і друкує кожні 5 секунд від boot. Перший report може охоплювати connection idle; pause intervals також закономірно знижують rate. `elapsed_ms` у рядку — тривалість поточного report interval, не cumulative playback time.
- Null-probe починає denominator від timestamp першого PCM (`src/a2dp_null_probe_common.h:230-249`), тому перший report правильно позначений `partial=yes`. Після першого PCM він продовжує звітувати і під час pause, отже pause interval може мати нульовий/низький rate.
- Counter snapshots узгоджені між собою, бо копіюються під одним `portMUX` (`src/a2dp_rate_probe_common.h:117-134`; null header `115-141`). Timestamp `millis()/esp_timer_get_time()` береться поза тим самим critical section, тому є мала boundary skew, але не 15% систематична похибка.
- `consumer_gap_us_max` і `write_us_max` cumulative, а не interval maxima. Gap між start двох write включає очікування PCM, scheduler latency, 5 ms delay і queue receive, тому не відрізняє starvation від нормальної packet cadence.
- `consumer_write_us_total`, byte/call counters і microsecond timestamps 32-bit. Byte counters wrap приблизно через 6.76 h при 176400 B/s; microseconds — приблизно через 71.6 min. Unsigned interval subtraction коректна через один wrap, але cumulative displayed values та `pending_estimate` стають неоднозначними після wrap.
- `interval_delta = static_cast<int32_t>(accepted_delta - written_delta)` спершу робить unsigned subtraction. Для малих різниць на двокомплементному ESP32 результат практично очікуваний, але це не надійний portable signed subtraction для різниць понад `INT32_MAX`.
- Rate-probe `pending_estimate = cumulative accepted - cumulative written` unsigned і може underflow після counter wrap або при порушенні invariant. Null-probe saturates від'ємний результат до нуля, що приховує таку аномалію. Обидва значення лише приблизні й не є фактичним ring-buffer occupancy.
- Accepted return у queued producer справді означає whole packet accepted (`size`) або rejected (`0`). Але producer counter оновлюється після upstream write; окремого виміру callback arrival cadence до enqueue немає.
- `write_us_avg=13–62 us` означає лише час копіювання конкретного item у доступний I2S DMA buffer. Після underrun/простою DMA може бути вільним, тому швидкий write не доводить сталого фізичного потоку.
- 4096 bytes PCM при 176400 B/s — 23.22 ms. Тому сам `gap > 20 ms` без знання block size не є starvation.
- SBC min/max bitpool у codec config — погоджений діапазон, а не виміряне значення кожного packet.

## 6. Підтверджені дефекти, але не підтверджені першопричини

1. **Queued item leak на zero write.** `continue` перед `vRingbufferReturnItem()` залишає отриманий item неповернутим. У поточному log zero/error не спостерігався.
2. **Некоректний zero/over-return handling у `i2s_write_data()`.** Zero створює нескінченний loop; return більше requested може зламати арифметику. Поточні counters цього не показали.
3. **Partial nonzero на рівні queued consumer не обробляється як втрата.** Поточні `short=0` це не активували.
4. **I2S begin install/pin errors не доходять до caller.** Driver все одно повертає true; A2DP adapter ігнорує begin result. У наданих фізичних результатах немає відповідного error log, а local tone працює.
5. **`setAudioInfo` може приховати помилку `i2s_set_sample_rates`.** Не застосовується до negotiated 44100 без зміни clock.
6. **Null format counters не ініціалізовані з inherited AudioInfo.** Це лише display defect.
7. **Rate-probe reports змішують playback з idle/pause на boundary та мають wrap/signed обмеження.** Це може спотворити окремі інтервали, але не пояснює одночасно audible «Морзе» та upstream overflow/drop warnings.

## 7. Невизначеності

- Немає повного UART log з per-interval `consumer_calls_delta`; без нього не відомий фактичний середній item size і не можна перевірити, чи 5 ms post-item delay створює близький до 150 kB/s ceiling.
- Немає timestamp/counter на самому вході decoded callback до volume/enqueue. Тому невідомо, чи pre-queue PCM вже надходить зі швидкістю 145–150 kB/s, чи час втрачається всередині producer path.
- Немає фактичного ring-buffer occupancy; `pending_estimate` — різниця cumulative accepted/written і не відображає dropped-before-counter data або item metadata/fragmentation.
- Немає runtime вимірювання I2S ISR core, interrupt rate, DMA underrun count або реальної BCK/LRCK частоти logic analyzer-ом.
- Внутрішні Bluedroid decoder/task та ESP-IDF legacy I2S driver prebuilt. Їх locks, ISR body і DMA service timing неможливо підтвердити лише локальним source audit.
- Немає реального-I2S direct probe з тими самими counters. Саме він потрібен, щоб відокремити queued implementation від загальної Bluetooth + I2S взаємодії.

## 8. Upstream-дослідження

1. [ESP32-A2DP v1.8.11 release, 2026-05-30](https://github.com/pschatzmann/ESP32-A2DP/releases/tag/v1.8.11): release додає `set_output_active_by_state(bool)`, але не заявляє виправлення queued throughput/ringbuffer. Це саме встановлена версія. Always-on тест використав новий API й не усунув дефект, тому pause/resume reinit не є достатньою причиною.
2. [AudioTools v1.2.5 release, 2026-06-23](https://github.com/pschatzmann/arduino-audio-tools/releases/tag/v1.2.5): для legacy ESP32 2.0.17 зазначене compile correction, але немає заявленого runtime throughput fix. Це саме встановлена версія.
3. [AudioTools v1.2.6 release, 2026-09-02](https://github.com/pschatzmann/arduino-audio-tools/releases/tag/v1.2.6): пізніша версія згадує ESP32 I2S deadlock fix. Вона не встановлена, симптом тут не deadlock (`write` повертає швидко, stream продовжується), а обмеження забороняє update. Тому release note є напрямком для окремого source diff, не підтвердженням цієї причини й не рекомендацією оновлювати dependency.
4. [ESP32-A2DP A2DP Sink Optimizations wiki, редакція 2023-10-21](https://github.com/pschatzmann/ESP32-A2DP/wiki/A2DP-Sink-Optimizations/ce64a359cf761f851c6cde5f4e4563e2df487d15): upstream описує queued sink як окремий ring buffer/task для jitter/volume-delay cases і перелічує tuning APIs. Wiki каже prefetch 70%, але встановлений v1.8.11 source має 65%; для цього аудиту авторитетним є локальний version-pinned code.
5. [ESP32-A2DP README, custom AudioTools I2S pins](https://github.com/pschatzmann/ESP32-A2DP/blob/main/README.md#defining-pins): офіційний приклад робить `i2s.begin(cfg)` перед `a2dp_sink.start()`, тобто спостережений double begin відповідає рекомендованій інтеграції. Це не доводить її безпомилковість у 1.2.5, але забороняє називати саму послідовність випадковою project-specific race.
6. [ESP-IDF 4.4 A2DP API](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32/api-reference/bluetooth/esp_a2dp.html): підтверджує decoded PCM callback та його виконання в A2DP sink task. Разом з локальним sdkconfig це обґрунтовує core 0 для producer callback.
7. [ESP-IDF 4.4 I2S API](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32/api-reference/peripherals/i2s.html): `i2s_write` пише в DMA TX buffer, timeout задається в RTOS ticks, `portMAX_DELAY` означає wait без timeout, а returned bytes — bytes copied до DMA. Це пояснює, чому повний швидкий return не є доказом завершеної передачі на шині.
8. [ESP-IDF interrupt allocation guide](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32/api-reference/system/intr_alloc.html): external interrupt зазвичай прив'язується до core, який виконує allocation. Це підтримує, але без runtime handle не завершує доказ core 1 для I2S ISR.
9. [ESP32-A2DP discussion #705, 2025-05-03](https://github.com/pschatzmann/ESP32-A2DP/discussions/705): є сторонній звіт про weird/low-pitch audio та queued ringbuffer overflows. Конфігурація складніша й автор згадує PSRAM; це лише схожий симптом, не доказ нашої причини.
10. [ESP32-A2DP issue #517, 2024-02-08](https://github.com/pschatzmann/ESP32-A2DP/issues/517): описує clicks на suspend/resume, які зникають при постійно активному I2S. Це релевантно лише lifecycle; наш always-on test не виправив steady-rate deficit, тому issue не пояснює поточний дефект.

Upstream-пошук не знайшов version-matched issue, яка доводить конкретну причину `176400 → 145000–150000 B/s` для ESP32-A2DP 1.8.11 + AudioTools 1.2.5 + Arduino-ESP32 2.0.17.

## 9. Гіпотези у порядку обґрунтованості

### 1. Взаємодія queued cadence з активним I2S, а не сирий bandwidth I2S

За: defect з'являється лише з real I2S; queue має 1440-byte cap, 5 ms yield після кожного item, prefetch/drop state machine та consumer priority 22 на core 1. Якщо real-I2S варіант систематично отримує менші items, fixed 5 ms overhead може дати ceiling близько спостережуваного. Overflow/drop і «Морзе» узгоджуються з циклічним prefetch/drop recovery.

Проти: queued-null з тими самими queue defaults стабільний; measured I2S writes короткі; без середнього item size і occupancy механізм не доведений.

### 2. Scheduler/resource interference між active legacy I2S і Bluetooth producer

За: accepted producer rate теж падає, хоча enqueue nonblocking; Bluetooth callback/core 0 та I2S consumer/ймовірний ISR/core 1 взаємодіють через prebuilt drivers, DMA, memory bus та FreeRTOS. Active I2S — головна підтверджена відмінність failing variant. Швидкий individual write не виключає missed producer scheduling або decoder starvation між writes.

Проти: немає task-run-time, callback-entry timestamps, ISR telemetry чи видимого shared lock. Це клас механізмів, не встановлена конкретна причина.

### 3. Початкова повторна інсталяція залишає некоректний I2S driver state

За: другий begin реально uninstall/install; install/pin errors маскуються; local tone має лише одну install.

Проти: це офіційно показаний upstream pattern для custom pins; config зберігається; немає install error log; APLL та always-on не змінили результат. Гіпотеза слабша за перші дві.

Sample-rate mismatch не входить до top-3: automatic forwarding існує, при 44100 update навмисно no-op, а фізичні тести rate-sync не показали 48000 як причину. Volume loop також не входить: він спільний для null/I2S paths, а volume lock не береться на кожному PCM block.

## 10. Один рекомендований наступний A/B-тест

**Змінна:** тільки sink class — `BluetoothA2DPSinkQueued` проти `BluetoothA2DPSink` — при тому самому реальному `CountedI2SStream`, I2S config, logger, sample-rate callback, `set_output_active_by_state(false)`, Bluetooth source, 1000 Hz file і тривалості тесту. Не змінювати buffer/DMA/task priority/core/APLL.

**Контроль:** наявний `a2dp-queued-rate-probe`.

**B-варіант для майбутньої окремої реалізації:** direct sink з тим самим `CountedI2SStream`, тією самою post-upstream producer instrumentation і тим самим форматом report, але без власного PCM callback, queue або writer task. Використати готовий upstream `BluetoothA2DPSink`; це штатний API та базовий upstream example. Поточний аудит цей variant не створює.

**Чому тест розрізняє гіпотези:** він прибирає лише upstream queue/consumer cadence, залишаючи Bluetooth decoder, volume path, AudioTools adapter, double begin, legacy I2S driver, GPIO, DMA та PCM format незмінними.

**Дискримінуючі результати:**

- direct real-I2S ≈176400 B/s і рівний звук, queued лишається 145–150 kB/s: сильно підтримує гіпотезу 1; далі треба виміряти queued item-size distribution/occupancy, а не збільшувати buffer навмання;
- direct також ≈145–150 kB/s: queue не потрібна для дефекту; гіпотеза 1 слабшає, фокус переходить на callback-entry cadence та active-I2S/driver scheduling (гіпотеза 2 або 3);
- direct має довгі blocking writes, але тримає 176400 B/s і рівний звук: DMA pacing справне, а проблема специфічна для handoff/state machine queued consumer;
- direct і queued мають 176400 B/s, але звук все ще нерівний: byte-rate метрика не ловить packet/sample discontinuity; потрібна перевірка PCM continuity або logic analyzer, не buffer tuning.

До цього A/B випадково змінювати ring buffer, prefetch, priority, stack, write size або APLL недоцільно: кожна така зміна змішує кілька можливих механізмів і не відповідає на головне питання — чи upstream queue необхідна для відтворення дефекту.
