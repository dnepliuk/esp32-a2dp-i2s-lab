# Аудит життєвого циклу `AudioInfo` у ESP32-A2DP та AudioTools

Дата перевірки: 2026-09-21.

## Висновок

Гіпотеза про зайву реконфігурацію I2S для вже активного формату
`44100 Hz / 2 channels / 16 bits` **спростована статичним кодом**.

У фактично встановленому тракті є два незалежні захисти:

1. `BluetoothA2DPOutputAudioTools::set_sample_rate()` порівнює всі три поля
   поточного `AudioInfo` з `sample_rate/2/16` і взагалі не викликає
   `setAudioInfo()`, якщо вони вже збігаються.
2. Навіть якщо тотожний `AudioInfo` викликати безпосередньо,
   `I2SDriverESP32::setAudioInfo()` порівнює його зі своєю активною
   конфігурацією та повертає `true` без `end()`, `begin()`,
   `i2s_set_sample_rates()` або перевстановлення драйвера.

Тому умова переходу до lifecycle A/B не виконана. Environments
`a2dp-queued-fork-delay0-lifecycle-observe` та
`a2dp-queued-fork-delay0-skip-identical` не створювалися. Локальний fork,
його manifest, patch і hashes не змінювалися.

## Перевірені версії

| Компонент | Фактична версія | Локальне підтвердження |
| --- | --- | --- |
| PlatformIO platform | `espressif32 7.1.3` | `%USERPROFILE%/.platformio/platforms/espressif32/platform.json` |
| Arduino-ESP32 | `2.0.17`, package `4.20017.260907+sha.dcc1105b` | package `framework-arduinoespressif32/package.json` |
| ESP-IDF | `4.4.7` | `esp_idf_version.h:22-26` |
| ESP32-A2DP | `v1.8.11` | `library.properties`; tag `v1.8.11` |
| ESP32-A2DP commit | `b6da4744286ca15b6f1dee607c077a4747294dd4` | clean local tagged checkout |
| AudioTools | `v1.2.5` | `library.properties`; tag `v1.2.5` |
| AudioTools commit | `2390c44fac3bdd1df40b80a088f00c9a203053b9` | local Git checkout |

Основні каталоги аудиту:

```text
.pio/libdeps/a2dp-queued-fork-delay0/ESP32-A2DP/
.pio/libdeps/a2dp-queued-fork-delay0/audio-tools/
%USERPROFILE%/.platformio/packages/framework-arduinoespressif32/
```

## Точний call graph

```text
BluetoothA2DPSink::start()
  -> init_i2s()
  -> BluetoothA2DPOutputAudioTools::begin()
  -> AdapterAudioStreamToAudioOutput::begin()
  -> I2SStream::begin()
  -> I2SDriverESP32::begin(config)
  -> i2s_driver_install(...)

ESP-IDF ESP_A2D_AUDIO_CFG_EVT
  -> BluetoothA2DPSink::av_hdl_a2d_evt()
  -> BluetoothA2DPSink::handle_audio_cfg()
       parses negotiated SBC sample rate and channel mode
       invokes optional sample_rate_callback
       -> BluetoothA2DPOutputAudioTools::set_sample_rate(rate)
            reads Adapter/I2SStream::audioInfo()
            compares current sample_rate, channels and bits_per_sample
            identical 44100/2/16:
              -> log "sample_rate not changed"
              -> return; no setAudioInfo call
            different:
              -> force AudioInfo(rate, 2, 16)
              -> AdapterAudioStreamToAudioOutput::setAudioInfo()
              -> virtual I2SStream::setAudioInfo()
                   -> AudioStream::setAudioInfo()
                   -> I2SDriverESP32::setAudioInfo()
                        identical: return true, no driver action
                        sample-rate-only difference:
                          -> i2s_set_sample_rates(...)
                        channel/bit difference or unsupported state:
                          -> return false
                   -> only after false and a real format difference:
                        i2s.end()
                        i2s.begin(updated_config)

ESP-IDF ESP_A2D_AUDIO_STATE_EVT
  -> BluetoothA2DPSink::handle_audio_state()
  -> set_i2s_active(true/false) when applicable
  -> BluetoothA2DPOutputAudioTools::set_output_active()
  -> AdapterAudioStreamToAudioOutput::begin()/end()
```

Остання гілка є окремим output lifecycle і не викликає
`set_sample_rate()` або `setAudioInfo()`. У поточному delay0 probe також
задано `set_output_active_by_state(false)`, тому Pause не деактивує output
через цей optional режим.

### Окремий одноразовий double-begin під час setup

У поточному `src/a2dp_queued_fork_delay_ab.cpp:337-359` application спочатку
викликає `i2s.begin(config)`, а потім `a2dp_sink.start()`. Другий виклик через
`BluetoothA2DPSink::start() -> init_i2s() -> out->begin()` доходить до
`I2SStream::begin()` ще раз. `I2SDriverESP32::begin(config)` на
`I2SESP32.h:205-209` бачить `is_started`, викликає `end()` і заново встановлює
driver.

Це реальний одноразовий restart, але він відбувається під час `start()` до
`init_bluetooth()` (`BluetoothA2DPSink.cpp:171-175`), тобто до codec
negotiation, STARTED і PCM playback. Обидва запропоновані A/B variants мали б
однаковий double-begin, тому policy `forward/skip identical setAudioInfo` його
не ізолює і не змінює. Він не є повторним `setAudioInfo()` із гіпотези та не є
періодичною дією, здатною безпосередньо пояснити сталий дефіцит протягом
60–90 секунд.

## Докази за source

### ESP32-A2DP

- `BluetoothA2DPSink.cpp:139-178` — `start()` викликає `init_i2s()` до запуску
  Bluetooth stack.
- `BluetoothA2DPSink.cpp:210-215` — `init_i2s()` викликає `out->begin()` і
  позначає output активним.
- `BluetoothA2DPSink.cpp:509-527` — `ESP_A2D_AUDIO_STATE_EVT` та
  `ESP_A2D_AUDIO_CFG_EVT` мають незалежні handler-и.
- `BluetoothA2DPSink.cpp:548-628` — codec configuration парсить SBC,
  оновлює `m_sample_rate`, викликає user callback, потім
  `out->set_sample_rate(m_sample_rate)`.
- `BluetoothA2DPSink.cpp:639-668` — Pause/Resume обробляється лише як audio
  state; sample-rate method тут не викликається.
- `BluetoothA2DPSink.cpp:670-680` — output begin/end можливі тільки при зміні
  `is_i2s_active`.
- `BluetoothA2DPOutput.cpp:229-245` — ключовий guard:

```cpp
audio_tools::AudioInfo info = p_audio_print->audioInfo();
if (info.sample_rate != m_sample_rate || info.channels != 2 ||
    info.bits_per_sample != 16) {
  info.sample_rate = m_sample_rate;
  info.channels = 2;
  info.bits_per_sample = 16;
  p_audio_print->setAudioInfo(info);
} else {
  // sample_rate not changed; setAudioInfo() is not called
}
```

- `BluetoothA2DPOutput.h:87-100` — `AudioStream` обгортається в
  `AdapterAudioStreamToAudioOutput`.
- `BluetoothA2DPSinkQueued.cpp:49-94` — queued consumer не викликає
  `setAudioInfo()`; він лише отримує PCM item і передає його output-у.
- `BluetoothA2DPSinkQueued.cpp:97-107` — аварійний `out->begin()` існує лише
  для стану `!is_i2s_active`; це інший механізм, не codec configuration.

### AudioTools

- `AudioTools/CoreAudio/AudioIO.h:346-375` — adapter передає
  `setAudioInfo()`, `begin()`, `end()` та `write()` у справжній
  `AudioStream` через virtual dispatch.
- `AudioTools/CoreAudio/BaseStream.h:43-48` — `BaseStream::begin()` і
  `BaseStream::end()` virtual.
- `AudioTools/CoreAudio/BaseStream.h:123-156` — `AudioStream::setAudioInfo()`
  та `audioInfo()` virtual; внутрішнє `info` змінюється лише при відмінності.
- `AudioTools/CoreAudio/AudioTypes.h:55-92` — `AudioInfo::equals()` порівнює
  `sample_rate`, `channels`, `bits_per_sample`; `equalsExSampleRate()` вимагає
  однакових channels/bits.
- `AudioTools/CoreAudio/AudioI2S/I2SStream.h:54-68` — no-argument `begin()`;
  він virtual через успадковану virtual-сигнатуру.
- `AudioTools/CoreAudio/AudioI2S/I2SStream.h:71-82` — `begin(I2SConfig)`
  зберігає початковий `AudioInfo` до запуску driver.
- `AudioTools/CoreAudio/AudioI2S/I2SStream.h:85-92` — `end()`; virtual через
  успадковану сигнатуру.
- `AudioTools/CoreAudio/AudioI2S/I2SStream.h:95-115` — virtual
  `setAudioInfo(AudioInfo)`; restart можливий лише коли driver повернув
  `false` і format реально відрізняється.
- `AudioTools/CoreAudio/AudioI2S/I2SESP32.h:37-50` — другий guard:

```cpp
if (is_started) {
  if (info.equals(cfg)) return true;
  if (info.equalsExSampleRate(cfg)) {
    cfg.sample_rate = info.sample_rate;
    return i2s_set_sample_rates(...) == ESP_OK;
  }
}
return false;
```

- `AudioTools/CoreAudio/AudioI2S/I2SESP32.h:84-89` — driver `end()` викликає
  `i2s_driver_uninstall()`.
- `AudioTools/CoreAudio/AudioI2S/I2SESP32.h:178-215` — повний `begin(config)`
  виконує `end()` лише якщо driver уже started, а потім
  `i2s_driver_install()`.

### ESP-IDF API

- `esp_a2dp_api.h:56-61` — окремі datapath states STOPPED/STARTED/SUSPEND.
- `esp_a2dp_api.h:85-92` — `ESP_A2D_AUDIO_STATE_EVT` і
  `ESP_A2D_AUDIO_CFG_EVT` є різними callback events.
- `esp_a2dp_api.h:105-119` — вони мають різні payload-и.

Локальний public header не задає жорсткої кількості або загального порядку
цих подій. Тому твердження «рівно один CFG event і він завжди перед STARTED»
не можна довести лише цим API. Можна довести, що кожен CFG event синхронно
проходить codec handler, а Pause/Resume handler сам не генерує
`set_sample_rate()`.

## Сигнатури та можливість перехоплення

| Метод | Фактична сигнатура | Virtual | Що може побачити subclass `I2SStream` |
| --- | --- | --- | --- |
| Audio info | `void setAudioInfo(AudioInfo)` | так | Виклики, що дійшли через adapter |
| PCM write | `size_t write(const uint8_t*, size_t)` | так | Усі adapter writes |
| Start current config | `bool begin()` | так, через `BaseStream` | Adapter `begin()` |
| Stop | `void end()` | так, через `BaseStream` | Adapter `end()` |
| Start explicit config | `bool begin(I2SConfig)` | ні | Setup-виклик статично, не через adapter |
| Driver format update | `I2SDriverESP32::setAudioInfo(AudioInfo)` | ні | Не перехоплюється subclass-ом |
| Driver begin/end | `I2SDriverESP32::begin/end` | ні | Внутрішній restart із `I2SStream::setAudioInfo()` не проходить через subclass `begin/end` |

Отже subclass міг би коректно рахувати adapter entry points, але не міг би
чесно називати свої `begin/end` counters повним числом внутрішніх driver
restart-ів. У цьому аудиті такий subclass не потрібен: ідентичний запит
відсіюється ще до нього.

## Відповіді на питання аудиту

1. **Скільки викликів на підключення?** Sink викликає
   `out->set_sample_rate()` один раз на кожен валідний
   `ESP_A2D_AUDIO_CFG_EVT`. Кількість CFG events задає Bluetooth stack і
   локальним wrapper-кодом не обмежена. Для вже активного `44100/2/16`
   кількість викликів `I2SStream::setAudioInfo()` дорівнює нулю незалежно від
   кількості однакових CFG events.
2. **До чи після STARTED?** Виклик виконується всередині CFG handler. Source
   не містить власного переходу до STARTED і не гарантує глобального порядку
   двох зовнішніх events. Нормальний negotiation очікує CFG перед PCM start,
   але це не інваріант, який перевіряє бібліотека.
3. **Pause/Resume?** Audio-state handler не викликає sample-rate update.
   Повтор можливий лише якщо сам stack окремо видасть новий CFG event.
4. **Той самий rate?** `out->set_sample_rate(44100)` викликається на CFG
   event, але adapter не передає тотожний `AudioInfo` далі.
5. **Які поля?** Adapter читає поточний `AudioInfo`, а перед forwarding
   задає sample rate із negotiation, channels=`2`, bits=`16`.
6. **Чи I2SStream порівнює формат?** Так, через driver; усі три поля
   порівнюються.
7. **Чи тотожний формат ігнорується?** Так: спершу A2DP adapter-ом, потім
   повторно I2S driver-ом.
8. **Чи є end/begin/reconfigure?** Для тотожного формату — ні. Для зміни
   лише sample rate — `i2s_set_sample_rates()`. Повний restart можливий для
   іншої channel/bit конфігурації або іншого unsupported update. Окремо є
   описаний вище одноразовий setup double-begin до запуску Bluetooth; він не
   спричинений codec `setAudioInfo()`.
9. **Чи begin/end virtual?** No-argument `I2SStream::begin()` і `end()` є
   virtual overrides успадкованих методів, навіть без повтореного keyword.
   `begin(I2SConfig)` та внутрішні driver methods не virtual.
10. **Що можна перевизначити без patch?** `setAudioInfo`, `write`, no-arg
    `begin`, `end`. Не можна через dynamic dispatch перехопити
    `I2SDriverESP32` або внутрішній driver restart.
11. **Разово чи періодично?** Codec path event-driven, не PCM-loop-driven.
    Playback loop не викликає `setAudioInfo()` періодично.
12. **Чи пояснює сталий дефіцит 60–90 секунд?** Ні. Для досліджуваного
    формату driver action відсутня; навіть реальна зміна sample rate була б
    одноразовою дією на CFG event, а не постійною паузою кожного PCM block.

## Факти, висновки та гіпотези

### Факти

- Початковий `begin(config)` записує `44100/2/16` в `AudioStream`.
- A2DP adapter порівнює `sample_rate/channels/bits` до forwarding.
- Тотожний формат не доходить до `I2SStream::setAudioInfo()`.
- Driver також має власний identical-format fast path.
- Pause/Resume handler не викликає sample-rate update.
- PCM/queued loops не містять періодичного `setAudioInfo()`.
- Наявний application flow має одноразовий повторний `begin()` до запуску
  Bluetooth, однаковий для обох запропонованих policies.

### Висновки

- Запропоновані observe та skip-identical variants у цьому проєкті були б
  функціонально однаковими для negotiation `44100/2/16`.
- Skip-identical variant не мав би жодного додаткового виклику, який можна
  пропустити порівняно з уже встановленим upstream adapter-ом.
- Створення двох environments не дало б причинного A/B і суперечило б умові
  зупинки завдання.

### Непідтверджені припущення

- Статичний код не доводить фактичну кількість CFG events від конкретного
  телефону; це можна підтвердити лише UART/Bluetooth trace.
- Статичний код не доводить реальну послідовність CFG/STARTED для кожного
  телефону.
- Цей висновок не пояснює залишкову нерівність тону при delay=0; він лише
  виключає тотожний `setAudioInfo()` як її причину.

## Наступна найвужча гіпотеза

Наступним доцільно перевірити не format lifecycle, а **часову поведінку
зв'язаного queued consumer → `I2SStream::write()`** після вилучення 5 ms:

- розподіл фактичного `item_size`;
- `i2s_write()` duration та start-to-start gaps;
- cumulative producer/consumer bytes лише за час стану STARTED;
- кореляцію довгих write/gap інтервалів із зміною ring-buffer mode та
  `pending_estimate`.

Це використовує вже наявний `a2dp-queued-fork-delay0` і його counters; новий
pipeline або lifecycle fork не потрібні. Якщо cumulative rates збігаються, а
5-секундні windows коливаються, дефіцит є артефактом burst/window timing. Якщо
producer стабільно нижчий саме під час довгих I2S write/gaps, наступний
одновимірний тест має стосуватися scheduling/pinning consumer task, а не
`AudioInfo`.
