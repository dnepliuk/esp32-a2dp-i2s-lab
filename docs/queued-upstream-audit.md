# Аудит `BluetoothA2DPSinkQueued`: upstream tuning і пропускна здатність

Дата перевірки: 2026-09-21.

## Результат

Обрано **шлях C**: підтримуваного бібліотекою способу прибрати фіксовану
паузу queued consumer немає, а новішого стабільного ESP32-A2DP після
`v1.8.11` не опубліковано. Тому новий PlatformIO environment не створено.

Підтверджений на рівні коду механізм, який може сповільнювати consumer:
після кожного успішно отриманого та переданого output-у item виконується
безумовний `delay_ms(5)`. Значення `5` є літералом у реалізації, не полем
конфігурації. Наявний `set_i2s_ticks()` керує лише timeout-ом
`xRingbufferReceiveUpTo()` і не змінює цю паузу.

Це підтверджена додаткова затримка в hot path, але ще не підтверджена єдина
фізична першопричина дефекту. DMA може частково перекривати паузу передачею
вже поставлених даних, тому причинний висновок потребує A/B-тесту тієї самої
версії з 5 ms і без 5 ms. Такий A/B неможливо реалізувати дозволеними в цьому
завданні public/protected setter-ами.

## Перевірені версії

| Компонент | Фактично встановлено | Перевірка |
| --- | --- | --- |
| PlatformIO platform | `espressif32@7.1.3` | `platformio.ini`; локальний `platform.json` |
| Arduino-ESP32 | `2.0.17` (`framework-arduinoespressif32` package `4.20017.260907+sha.dcc1105b`) | локальний `package.json` |
| ESP-IDF | `4.4.7` | локальний `esp_idf_version.h` |
| ESP32-A2DP | `v1.8.11` | `library.properties`; локальний Git tag |
| ESP32-A2DP commit | `b6da4744286ca15b6f1dee607c077a4747294dd4` | локальний `git rev-parse HEAD` |
| AudioTools | `v1.2.5` | `library.properties`; локальний Git tag |
| AudioTools commit | `2390c44fac3bdd1df40b80a088f00c9a203053b9` | локальний `git rev-parse HEAD` |

ESP32-A2DP `v1.8.11` є останнім офіційним stable release на дату аудиту.
Його [release notes](https://github.com/pschatzmann/ESP32-A2DP/releases/tag/v1.8.11)
не містять виправлення queued consumer, fixed delay, partial write,
ring-buffer overflow або output latency. Локальний checkout точно стоїть на
тому самому tag/commit; `git diff v1.8.11 --` для перевірених upstream-файлів
порожній.

Офіційний `main` також досі містить той самий `delay_ms(5)` у
[`BluetoothA2DPSinkQueued.cpp`](https://github.com/pschatzmann/ESP32-A2DP/blob/main/src/BluetoothA2DPSinkQueued.cpp),
але branch не розглядався як допустима залежність для тесту.

## Точні локальні джерела

Базовий каталог аудиту:

```text
.pio/libdeps/a2dp-queued-rate-probe/ESP32-A2DP/
```

Перевірено:

- `src/BluetoothA2DPSinkQueued.h:7-8` — defaults ring buffer і prefetch;
- `src/BluetoothA2DPSinkQueued.h:79-97` — public queued setter-и;
- `src/BluetoothA2DPSinkQueued.h:99-128` — protected state, defaults і
  вирівнювання prefetch до 4 bytes;
- `src/BluetoothA2DPSinkQueued.cpp:6-23` — створення ring buffer, semaphore і
  pinned consumer task;
- `src/BluetoothA2DPSinkQueued.cpp:45-89` — повний consumer loop;
- `src/BluetoothA2DPSinkQueued.cpp:91-140` — producer, prefetch/drop і
  semaphore;
- `src/BluetoothA2DPSink.h:404-417` — base output/write setter-и;
- `src/BluetoothA2DPSink.h:434-498` — protected output state та defaults;
- `src/BluetoothA2DPSink.cpp:1296-1315` — `i2s_write_data()` і partial write;
- `src/BluetoothA2DPOutput.cpp:195-215` — AudioTools begin/write adapter;
- `src/BluetoothA2DPOutput.cpp:229-243` — upstream automatic sample-rate
  propagation;
- `src/BluetoothA2DPCommon.h:303-314` — common task/core/event setter-и;
- `src/BluetoothA2DPCommon.h:402-410` — task/core/event defaults;
- `src/BluetoothA2DPCommon.cpp:609-615` — `delay_ms()` реалізований через
  Arduino `delay()` у цій збірці;
- `src/config.h:25-30` — base max-write compile-time defaults;
- `examples/bt_music_receiver_queued/bt_music_receiver_queued.ino:17-32` —
  офіційний queued example.

Посилання на незмінний tag/commit:

- [`BluetoothA2DPSinkQueued.h`](https://github.com/pschatzmann/ESP32-A2DP/blob/b6da4744286ca15b6f1dee607c077a4747294dd4/src/BluetoothA2DPSinkQueued.h#L79-L128)
- [`BluetoothA2DPSinkQueued.cpp`, consumer](https://github.com/pschatzmann/ESP32-A2DP/blob/b6da4744286ca15b6f1dee607c077a4747294dd4/src/BluetoothA2DPSinkQueued.cpp#L45-L89)
- [`BluetoothA2DPSinkQueued.cpp`, producer/drop](https://github.com/pschatzmann/ESP32-A2DP/blob/b6da4744286ca15b6f1dee607c077a4747294dd4/src/BluetoothA2DPSinkQueued.cpp#L91-L140)
- [`BluetoothA2DPSink::i2s_write_data()`](https://github.com/pschatzmann/ESP32-A2DP/blob/b6da4744286ca15b6f1dee607c077a4747294dd4/src/BluetoothA2DPSink.cpp#L1296-L1315)
- [`BluetoothA2DPOutputAudioTools`](https://github.com/pschatzmann/ESP32-A2DP/blob/b6da4744286ca15b6f1dee607c077a4747294dd4/src/BluetoothA2DPOutput.cpp#L195-L243)
- [офіційний queued example](https://github.com/pschatzmann/ESP32-A2DP/blob/b6da4744286ca15b6f1dee607c077a4747294dd4/examples/bt_music_receiver_queued/bt_music_receiver_queued.ino)

## Параметри `BluetoothA2DPSinkQueued` 1.8.11

| Параметр | Фактичний default | API | Доступність і точна дія |
| --- | ---: | --- | --- |
| Post-item consumer delay | `5 ms` | немає | Літерал `delay_ms(5)` у `.cpp`; виконується після `vRingbufferReturnItem()` для кожного обробленого item |
| Receive timeout | `20 ms` | `set_i2s_ticks(int)` | Public; лише timeout `xRingbufferReceiveUpTo(..., pdMS_TO_TICKS(i2s_ticks), ...)` |
| Consumer priority | `configMAX_PRIORITIES - 3`; у локальному FreeRTOS це `22` | `set_i2s_task_priority(UBaseType_t)` | Public; змінює тільки queued consumer priority |
| Consumer core | `1` | `set_task_core(BaseType_t)` | Public, успадкований; окремого I2S-core setter-а немає, те саме поле використовується також common task-ами |
| Consumer stack | `2048 bytes` | `set_i2s_stack_size(int)` | Public |
| Maximum received item/write block | `240 * 6 = 1440 bytes` | `set_i2s_write_size_upto(size_t)` | Public; верхня межа `xRingbufferReceiveUpTo()` |
| Ring-buffer size | `32 * 1024 = 32768 bytes` | `set_i2s_ringbuffer_size(int)` | Public; використовується під час `xRingbufferCreate()` |
| Prefetch | `65%`; фактичний поріг `21296 bytes` | `set_i2s_ringbuffer_prefetch_percent(int)` | Public; приймає `0..100`, результат вирівнюється вниз до 4 bytes |
| Base output chunk | `A2DP_I2S_MAX_WRITE_SIZE = 5120 bytes` | `set_max_write_size(int)` | Public base API; ділить `i2s_write_data()`, але default queued item `1440` і так менший |
| Delay між base output chunks | `A2DP_I2S_MAX_WRITE_DELAY_MS = 0` | `set_max_write_delay_ms(int)` | Public base API; це не post-item 5 ms |

Офіційна сторінка
[A2DP Sink Optimizations](https://github.com/pschatzmann/ESP32-A2DP/wiki/A2DP-Sink-Optimizations)
перелічує queued setter-и. Вона вказує prefetch `70`, але фактично встановлений
source tag `v1.8.11` задає `RINGBUF_PREFETCH_PERCENT 65`; для цього аудиту
джерело tag-а є авторитетним.

У `BluetoothA2DPSinkQueued` немає `private` секції: internal state і handler-и
protected. Це не створює підтримуваного setter-а для 5 ms. Підклас міг би
перевизначити весь virtual `i2s_task_handler()`, але це вже власний consumer
loop, прямо заборонений умовами. `delay_ms()` не virtual, а `5` не зберігається
в protected member, тому приховати метод у підкласі недостатньо.

## Фактична поведінка consumer

Спрощений псевдокод, що відповідає `BluetoothA2DPSinkQueued.cpp:45-89`:

```text
is_starting = true
forever:
    if is_starting:
        wait indefinitely for prefetch semaphore
        is_starting = false

    data, size = ringbuffer.receive_up_to(
        timeout = pdMS_TO_TICKS(i2s_ticks),
        maximum = i2s_write_size_upto)

    if size == 0:
        switch to PREFETCHING if needed
        continue

    if output is active:
        written = i2s_write_data(data, size)
        if written == 0:
            log error
            continue

    ringbuffer.return_item(data)
    delay_ms(5)
```

Отже, 5 ms виконуються не лише при underflow/error. Вони виконуються після
кожного успішно отриманого item, після output write і повернення item у ring
buffer. У цій Arduino-збірці `delay_ms(5)` викликає Arduino `delay(5)`, яке
віддає task scheduler-у; значення задається в milliseconds. На відміну від
нього, `i2s_ticks` спочатку є milliseconds у member, а в receive call
перетворюється на ticks через `pdMS_TO_TICKS()`.

## Producer, prefetch і drop

`write_audio()` робить non-blocking `xRingbufferSend(..., timeout=0)`:

1. у `PREFETCHING` накопичує дані до `21296` bytes, переводить mode у
   `PROCESSING` і дає binary semaphore;
2. при невдалому send логуються `ringbuffer overflowed...` і mode переходить
   у `DROPPING`;
3. у `DROPPING` нові packet-и повертають `0` і логують
   `ringbuffer is full, drop this packet!`, доки free size не опуститься до
   prefetch threshold;
4. `semphore give failed` — буквальний upstream-текст при невдалому
   `xSemaphoreGive()`.

Ring buffer збільшує запас часу, але не збільшує сталу швидкість consumer.
Якщо середня швидкість consumer нижча за producer, більший buffer тільки
відсуває overflow.

## Partial output write

Queued handler передає item у base `i2s_write_data(data, item_size)`. Цей метод:

```text
open = item_size
processed = 0
while open > 0:
    written = output.write(data + processed, min(open, max_write_size))
    open -= written
    processed += written
    delay_ms(max_write_delay_ms)  // default 0
return processed
```

Тому позитивний partial write дозаписується з правильного byte offset і
залишок не відкидається. `BluetoothA2DPOutputAudioTools::write()` просто
повертає результат `Print/AudioStream::write()`, тож short count доходить до
цього циклу.

Є окремий крайовий випадок: якщо output повертає `0`, `open` не зменшується,
а цикл не має break. За default `max_write_delay_ms=0` це може стати нескінченним
циклом. Якщо `i2s_write_data()` поверне `0` через inactive-state race, queued
handler виконає `continue` до `vRingbufferReturnItem()`. Setter-а, що змінює
цю логіку, немає.

Для AudioTools 1.2.5 шлях має такий вигляд:

```text
BluetoothA2DPSinkQueued::i2s_task_handler
  -> BluetoothA2DPSink::i2s_write_data
  -> BluetoothA2DPOutputAudioTools::write
  -> AudioTools I2SStream::write
  -> ESP32 I2S::writeBytes
  -> ESP-IDF i2s_write(..., portMAX_DELAY)
```

Локальні AudioTools locations:

- `.pio/libdeps/a2dp-queued-rate-probe/audio-tools/src/AudioTools/CoreAudio/AudioI2S/I2SStream.h:117-122`;
- `.pio/libdeps/a2dp-queued-rate-probe/audio-tools/src/AudioTools/CoreAudio/AudioI2S/I2SESP32.h:94-109,163`.

## Розрахунок пропускної здатності

PCM rate для `44100 Hz`, stereo, signed 16-bit:

```text
R = 44100 * 2 * 2 = 176400 B/s
```

Час відтворення default queued block:

```text
T_audio = 1440 / 176400 = 0.008163265 s = 8.163265 ms
```

У запитаній послідовній моделі «blocking write займає весь audio time, потім
окремі 5 ms»:

```text
T_cycle = block_bytes / 176400 + 0.005
R_consumer = block_bytes / T_cycle
```

| Block | Audio time | Cycle з 5 ms | Теоретична service rate | Дефіцит до 176400 |
| ---: | ---: | ---: | ---: | ---: |
| `1440 B` | `8.163 ms` | `13.163 ms` | `109395 B/s` | `37.984%` |
| `5120 B` | `29.025 ms` | `34.025 ms` | `150478 B/s` | `14.695%` |
| `8192 B` | `46.440 ms` | `51.440 ms` | `159254 B/s` | `9.720%` |
| `16384 B` | `92.880 ms` | `97.880 ms` | `167389 B/s` | `5.108%` |
| `32768 B` | `185.760 ms` | `190.760 ms` | `171776 B/s` | `2.621%` |

Для будь-якого скінченного блока ця модель дає rate нижче `176400 B/s`;
збільшення `set_i2s_write_size_upto()` лише асимптотично зменшує overhead.
У межах default ring buffer навіть блок `32768 B` теоретично не дорівнює
producer rate.

Ця таблиця не є прямим прогнозом виміряної UART rate: `i2s_write()` записує у
DMA queue, тому частина 5 ms може перекриватися фізичною передачею вже
поставлених bytes, а wall time одного write залежить від поточної заповненості
DMA. Саме тому кодовий факт «є 5 ms на item» відділено від гіпотези «саме це
повністю пояснює 146–150 kB/s».

## Порівняння з latest stable та upstream-свідчення

| Область | Встановлена `v1.8.11` | Latest stable | Висновок |
| --- | --- | --- | --- |
| Consumer loop | write, return item, `delay_ms(5)` | та сама `v1.8.11` | виправлення відсутнє |
| Partial write | base loop дозаписує positive remainder | те саме | без змін |
| Queue/drop | 32 KiB byte ring buffer, prefetch/drop modes | те саме | без змін |
| Semaphore | binary semaphore, upstream typo `semphore give failed` | те саме | без змін |
| Task settings | public priority/stack, inherited core | те саме | немає delay setter-а |
| AudioTools adapter | forwards write count, automatic sample rate | те саме | не усуває 5 ms |
| Arduino-ESP32 2.0.17 / IDF 4.4.7 | фактично resolved у цьому проєкті | підтримується поточним кодом | update framework не потрібен |

Релевантні офіційні матеріали:

- [releases: `v1.8.11` позначено Latest](https://github.com/pschatzmann/ESP32-A2DP/releases);
- [`v1.8.11`, commit `b6da474`](https://github.com/pschatzmann/ESP32-A2DP/commit/b6da4744286ca15b6f1dee607c077a4747294dd4);
- [офіційний перелік queued tuning API](https://github.com/pschatzmann/ESP32-A2DP/wiki/A2DP-Sink-Optimizations);
- [офіційна class reference](https://pschatzmann.github.io/ESP32-A2DP/html/class_bluetooth_a2_d_p_sink_queued.html);
- [discussion #705 про weird audio та ring-buffer overflow](https://github.com/pschatzmann/ESP32-A2DP/discussions/705).

Discussion #705 є лише додатковим свідченням схожих симптомів; це не доказ
причини саме для цього обладнання. Release notes `v1.8.9` згадують лише
приглушення spam для underflow log, `v1.8.10` — source-side fixes, а
`v1.8.11` — інші API/compatibility зміни. Заявленого stable fix для queued
tempo, fixed 5 ms, overflow/drop або volume latency немає.

## Чому відхилено шляхи A і B

### Шлях A — supported tuning 1.8.11

Відхилено як виправлення:

- `set_i2s_ticks(0)` змінить receive timeout, не post-write delay;
- priority/core/stack не прибирають детерміновані 5 ms;
- ring-buffer size і prefetch змінюють запас/latency, не сталу service rate;
- `set_i2s_write_size_upto()` може зменшити частоту 5 ms і придатний для
  окремого діагностичного sweep, але не прибирає паузу й не гарантує
  `176400 B/s`;
- `set_max_write_delay_ms(0)` уже є default і керує іншою паузою — між
  chunks усередині base `i2s_write_data()`.

Тому environment `a2dp-queued-supported-tuning` не створено: він видавав би
евристику за підтримуване виправлення.

### Шлях B — newer stable upstream fix

Відхилено, бо `v1.8.11` уже є latest stable. Немає нового stable tag/SHA з
релевантним виправленням, який можна чесно закріпити. `main` не використано.

## Мінімальні подальші дії

1. Відкрити upstream issue з цим аудитом і фізичними логами
   `a2dp-queued-rate-probe`; попросити зробити post-item delay параметром або
   прибрати його.
2. Мінімальний upstream-compatible patch для обговорення: додати member
   `i2s_post_write_delay_ms` з default `5`, public setter і замінити літерал у
   consumer. Це дасть чистий A/B `5 ms` проти `0 ms` без зміни решти pipeline.
3. Якщо upstream прийме fix і опублікує stable tag, додати окремий environment,
   pinned одночасно на точний tag і commit SHA; не використовувати branch.
4. До такого release можна виконати лише явно позначений fork/patch A/B у
   окремому досліді. У цьому завданні його не створено: не патчено
   `.pio/libdeps`, не копійовано queued class і не додано custom consumer.
5. Паралельно з A/B вимірювати producer accepted rate, consumer written rate,
   `i2s_write()` duration/gaps і approximate pending. Це відокремить fixed-delay
   effect від DMA blocking, scheduler contention та інших причин.

## Межа виконаних змін

Створено лише цей документ. `platformio.ini`, усі наявні `src/*`, старі
environments і залежності не змінено. Upload/Monitor не виконувалися. Оскільки
обрано шлях C і нового environment немає, build/ELF/UART/фізичний тест для
неіснуючого A/B environment не застосовуються.
