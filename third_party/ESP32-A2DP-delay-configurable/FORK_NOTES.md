# ESP32-A2DP delay-configurable snapshot

- Upstream repository: https://github.com/pschatzmann/ESP32-A2DP
- Upstream tag: `v1.8.11`
- Upstream commit: `b6da4744286ca15b6f1dee607c077a4747294dd4`
- License: Apache-2.0 (unchanged upstream `LICENSE`)
- Snapshot date: 2026-09-21
- Snapshot source: tagged PlatformIO checkout; tag, commit, clean worktree and
  per-file SHA-256 values were verified before copying

## Modification

Only these upstream files are modified:

- `src/BluetoothA2DPSinkQueued.h`
- `src/BluetoothA2DPSinkQueued.cpp`

The fork adds this public API:

```cpp
void set_i2s_post_write_delay_ms(uint32_t delay_ms);
```

It stores the value in:

```cpp
uint32_t i2s_post_write_delay_ms = 5;
```

The unconditional upstream post-item `delay_ms(5)` becomes:

```cpp
if (i2s_post_write_delay_ms > 0) {
    delay_ms(i2s_post_write_delay_ms);
}
```

The default remains `5 ms`, so code that does not call the setter retains the
upstream `v1.8.11` behavior. The setter is intended to be called before
`BluetoothA2DPSinkQueued::start()`. Runtime mutation/thread-safety after start
is deliberately not provided.

No ring-buffer, prefetch, receive timeout, write size, task, semaphore,
partial-write, AudioTools adapter, I2S lifecycle, codec or Bluetooth callback
logic is changed.

## Reason and status

This snapshot supports a controlled physical A/B test of the hard-coded 5 ms
consumer pause. It is not a claim that removing the pause fixes the observed
audio defect. A physical PASS has not yet been obtained.

The exact patch is stored in `docs/esp32-a2dp-delay-configurable.patch`. The
machine-readable provenance and original/fork hashes are in
`fork-manifest.json`.
