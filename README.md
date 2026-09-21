# Minimal ESP32 A2DP sink test

This independent PlatformIO project isolates the Bluetooth stack used by
Arduino-ESP32 2.0.17 / ESP-IDF 4.4.7. Audio follows the direct library path:

```text
phone -> BluetoothA2DPSink -> AudioTools I2SStream -> PCM5102A
```

There is no queued sink, custom buffer, audio task, PCM callback, DSP, or
periodic statistics code. The first physical test deliberately leaves the DAC
and amplifier disconnected.

The DOIT board has 4 MB of flash. The project selects PlatformIO's stock
`huge_app.csv` layout because the Bluetooth application is larger than the
default 1.25 MiB application partition; no custom partition file is used.

## Build

```text
pio run -e a2dp-arduino-minimal
```

## Manual test in VS Code

1. Open this folder in a separate VS Code window.
2. Open **PlatformIO -> Project Tasks**.
3. Run **a2dp-arduino-minimal -> General -> Build**.
4. Run **General -> Upload**.
5. Open **Platform -> Monitor** at 115200 baud.
6. Press the ESP32 **EN/RESET** button.
7. Check the startup banner and the reported Arduino-ESP32 and ESP-IDF versions.
8. Pair the same phone with **Faital A2DP Arduino**.
9. Start the same locally downloaded track from its beginning.
10. Let it play for 60-90 seconds.
11. Press Pause.
12. Keep recording the serial monitor for another 10 seconds.
13. Save the complete, unfiltered log beginning at RESET.

For this first run, power only the ESP32 through USB. Disconnect the PCM5102A
and amplifier, turn off the phone's Wi-Fi/hotspot, keep the phone 20-30 cm from
the ESP32, and use the same phone and track as in earlier measurements.

The run passes if pairing and streaming work without reset/disconnect and there
are no recurring `BT_APPL: Sequence numbers error` messages. A single warning
near start/pause is acceptable only if it does not repeat. If that run passes,
repeat with the PCM5102A and amplifier connected at low volume; playback must
then remain continuous.

Do not conclude that the original issue is fixed until the complete UART log
and the physical audio test have both been reviewed.

## Queued always-on A/B test

Use this test to compare the upstream queued A2DP output while keeping the I2S
output active across Bluetooth pause and resume transitions.

1. In PlatformIO, open **Project Tasks**.
2. Select `a2dp-queued-always-on`.
3. Run **General -> Upload**.
4. Open **Platform -> Monitor** at `115200` baud.
5. Press **EN/RESET** and verify the banner contains
   `ARDUINO_A2DP_TEST variant=queued-always-on`.
6. Connect the phone to `Faital A2DP Always On`.
7. Play a 1000 Hz sine wave for 30 seconds.
8. Pause for 10 seconds, then resume for 30 seconds. Repeat this pause/resume
   cycle twice.
9. Play the same music track for 60 seconds and test volume changes.
10. Pause, leave the monitor open for another 10 seconds, and save the complete,
    unfiltered UART log.

PASS criteria:

- the sine wave is steady, without a Morse-like interruption pattern;
- music is steady;
- pause/resume is immediate and volume changes do not introduce multi-second
  delays;
- after startup there are no repeated `I2SStream::begin()` messages and Pause
  does not produce `I2SStream::end()`;
- there are no repeated `ringbuffer overflowed`, `drop this packet`, or
  `write_audio(): semaphore give failed` messages.

FAIL criteria:

- overflow/drop warnings remain even though I2S is not restarted;
- the tone remains interrupted or latency accumulates after pause/resume.

If the test fails without repeated I2S begin/end activity, do not tune the
buffers. The next diagnostic step is to measure the actual A2DP producer and
queued-consumer byte rates.

## Queued rate probe and APLL A/B test

The `a2dp-queued-rate-probe` and `a2dp-queued-apll` environments measure the
accepted A2DP PCM byte rate and the actual I2S write rate. Both use the
upstream queued sink and its default queue, prefetch, task, timeout, and write
settings. The APLL environment differs only by enabling `use_apll` for the I2S
clock.

`pending_estimate` is `cumulative accepted - cumulative written`. It is an
approximation, not the actual FreeRTOS ring-buffer fill level: an item being
written is no longer in the ring buffer, ring-buffer bookkeeping is not
counted, and short or failed I2S writes make the difference include bytes that
have already left the queue.

### Test A - default clock

1. In PlatformIO **Project Tasks**, select `a2dp-queued-rate-probe`.
2. Run **General -> Upload**.
3. Open **Platform -> Monitor** at `115200` baud.
4. Press **EN/RESET** and verify the `queued-rate-probe` banner.
5. Connect the phone to `Faital A2DP Rate Probe`.
6. Start the same local 1000 Hz sine file from the beginning.
7. Do not change volume or press Pause for 70 seconds.
8. After 70 seconds, press Pause and keep recording the monitor for another
   10 seconds.
9. Save the complete, unfiltered UART log beginning at RESET.

### Test B - APLL clock

Repeat the same procedure with `a2dp-queued-apll` and connect to
`Faital A2DP APLL`. Keep all conditions identical: use the same phone, local
file, volume, and distance; disable Wi-Fi/hotspot; and allow 70 seconds without
interaction.

If either variant has no overflow during 70 seconds, repeat that variant for
at least 120 seconds.

### Interpreting the measurements

- If `producer_rate` is approximately `176400 B/s`, `consumer_rate` is
  consistently lower, `interval_delta` is positive, and `pending_estimate`
  keeps increasing, the consumer/I2S side is not keeping up.
- If APLL aligns the producer and consumer rates, stops the growth of
  `pending_estimate`, and removes overflow, I2S clock accuracy is implicated.
  This conclusion requires the physical A/B result; enabling APLL alone is not
  considered a fix.
- If average rates match but large `consumer_gap_ms_max` values precede an
  overflow, the consumer is periodically blocked or not scheduled.
- If `producer_rate` is noticeably above `176400 B/s`, investigate decoded
  PCM pacing from A2DP/the source.
- If rates and the pending estimate remain stable but audio is still defective,
  test PCM frame integrity and ordering next without changing the audio.

## Wiring for the second run

| I2S signal | ESP32 pin |
| --- | ---: |
| BCK | GPIO26 |
| WS / LRCK | GPIO25 |
| DATA OUT | GPIO22 |

I2S is configured for transmit, Philips/I2S format, 44100 Hz, 16-bit stereo,
with data input disabled.

## Direct and queued null-output diagnostic

The `a2dp-direct-null-probe` and `a2dp-queued-null-probe` environments discard
decoded PCM immediately after counting it. They do not start I2S or any audio
hardware. The direct variant has no additional queue; the queued variant keeps
the unmodified upstream ESP32-A2DP ring buffer, consumer task, and all their
default settings. In queued statistics, `pending_estimate` is the approximate
difference between bytes accepted by the upstream queue and bytes delivered to
the null stream; it is not the actual ring-buffer fill level.

For the installed versions, the AudioTools output adapter reports decoded PCM
to its `AudioStream` as 16-bit, two-channel frames and the sink processes data
in four-byte `Frame` units. Therefore the expected output rate is calculated
from the `AudioInfo` actually delivered to the null stream (normally
`sample_rate * 2 * 2`). The negotiated SBC channel mode is logged separately.
The installed ESP-IDF headers describe the callback payload as PCM decoded from
SBC but do not expose the prebuilt Bluedroid decoder's mono-to-stereo conversion
implementation, so its exact internal conversion point remains unverified.

Use the same locally stored 1000 Hz file and identical phone volume for both
tests. Power the ESP32 through USB; the PCM5102A and amplifier may be physically
disconnected because these environments generate no I2S clocks.

### Test 1 - direct null

1. Upload `a2dp-direct-null-probe` from PlatformIO **Project Tasks**.
2. Open **Platform -> Monitor** at `115200` baud.
3. Press **EN/RESET** and verify the `direct-null-probe` banner.
4. Connect the phone to `Faital A2DP Direct Null`.
5. Start the local 1000 Hz file from the beginning.
6. Do not change volume or press Pause for 70 seconds.
7. Press Pause, record the monitor for 10 more seconds, and save the complete
   UART log beginning at RESET.

### Test 2 - queued null

Repeat the same procedure and conditions with `a2dp-queued-null-probe` and
`Faital A2DP Queued Null`.

If direct null is stable near `176400 B/s` without gaps, repeat it for 120
seconds. If direct null instead remains near 145-150 kB/s, repeat that same
direct-null firmware once with a Windows laptop and the same local PCM/WAV
file. The laptop run is a source A/B check, not the primary test.

### Interpreting null-output results

- Direct and queued both near `176400 B/s`: Bluetooth/SBC delivery is healthy;
  investigate the real I2S output path or its interaction with the sink.
- Direct near `176400 B/s`, but queued near 145-150 kB/s or overflowing: the
  upstream queued path/task/ring buffer is implicated.
- Direct and queued both near 145-150 kB/s with similar gaps: the deficit is
  before the queue and I2S, in the source/controller/Bluedroid/SBC delivery.
- Phone near 145-150 kB/s but laptop near `176400 B/s`: behavior depends on the
  phone or its A2DP source implementation.
- Both sources near 145-150 kB/s: investigate ESP32 Bluetooth controller,
  Bluedroid/framework configuration, or decoded callback semantics.
- Average rate near expected with recurring 50-100 ms gaps: investigate
  packet-delivery jitter rather than average throughput.

These are diagnostic interpretations, not established causes. Preserve all
Bluetooth and queue warnings in the UART logs.

## Direct-I2S A/B probe

`a2dp-direct-i2s-probe` is the direct-path counterpart to
`a2dp-queued-rate-probe`. It keeps the same board, pinned dependencies, I2S
pins and format, initial 44100 Hz rate, `use_apll=false`, AudioTools Info
logging, and upstream automatic sample-rate handling. The deliberate runtime
difference is the sink class: the direct probe uses `BluetoothA2DPSink` and
writes to the real counted `I2SStream` without an application-level queue,
writer task, extra buffer, PCM callback, or resampler.

Build the direct probe, then rebuild the queued control:

```powershell
pio run -e a2dp-direct-i2s-probe -t clean
pio run -e a2dp-direct-i2s-probe
pio run -e a2dp-queued-rate-probe
```

Physical A/B procedure:

1. Upload `a2dp-direct-i2s-probe` from PlatformIO **Project Tasks**.
2. Open **Platform -> Monitor** at `115200`.
3. Press **EN/RESET**.
4. Confirm the `direct-i2s-probe` banner.
5. Connect the phone to `Faital A2DP Direct I2S`.
6. Start the same local 1000 Hz sine file from the beginning.
7. Do not change volume or press Pause for 70-90 seconds.
8. Record whether the tone is steady from the start and whether it changes
   over time.
9. Press Pause.
10. Leave the monitor open for another 10 seconds.
11. Save the complete, unfiltered UART log beginning at RESET.
12. Close the monitor.

Run the first cycle with the PCM5102A and amplifier connected at low volume so
the metrics and physical sound are evaluated together. Statistics intentionally
stop while A2DP is suspended, so pause time is not reported as a playback-rate
deficit. Then upload `a2dp-queued-rate-probe` without changing the phone,
source file, wiring, volume, distance, or test duration; repeat the same
sequence and save that UART log.

Compare the negotiated sample rate, SBC configuration, `callback_rate`,
`i2s_rate`, entry-gap maxima and buckets, direct/base write time, I2S write
time, short writes/errors, free heap, and all queued-probe queue occupancy and
drop counters. Interpret the result as follows:

- If the direct probe is clean while the queued probe breaks up, the remaining
  evidence makes the upstream queued path necessary to reproduce the defect;
  next measure queued item-size distribution/cadence or isolate its 5 ms delay.
- If direct is also near 145000-150000 B/s, the queue is not a necessary
  condition; investigate blocking of the Bluetooth callback by direct I2S,
  active-I2S/driver scheduling, or a repeated begin/configuration path.
- If direct is near 176400 B/s but audio remains uneven, average byte rate is
  insufficient; measure PCM continuity or inspect WS/BCK/DATA with a logic
  analyzer.
- If `base_write_us_avg/max` is high while callback rate falls, direct I2S
  backpressure is blocking the A2DP callback; correlate that with gaps and the
  measured rate.
- If base writes are short but callback rate is low, investigate active-I2S/ISR
  or lifecycle/configuration interference rather than blocking direct writes.

## Compatibility note

ESP32-A2DP 1.8.11 uses the ESP-IDF 6 name
`ESP_A2D_AUDIO_STATE_SUSPEND`. ESP-IDF 4.4.7 exposes the same suspended state
as `ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND`, so `platformio.ini` supplies that
compatibility alias as a build definition. This matches the guard added in the
upstream library after 1.8.11 and does not modify either pinned dependency.
