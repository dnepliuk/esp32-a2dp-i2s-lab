#!/usr/bin/env python3
"""Read-only verifier for the local ESP32-A2DP delay-configurable snapshot."""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FORK = ROOT / "third_party" / "ESP32-A2DP-delay-configurable"
UPSTREAM = ROOT / ".pio" / "libdeps" / "a2dp-queued-rate-probe" / "ESP32-A2DP"
MANIFEST = FORK / "fork-manifest.json"
PATCH = ROOT / "docs" / "esp32-a2dp-delay-configurable.patch"
PLATFORMIO = ROOT / "platformio.ini"
AB_SOURCE = ROOT / "src" / "a2dp_queued_fork_delay_ab.cpp"
FREERTOS_CONFIG = (
    Path.home()
    / ".platformio/packages/framework-arduinoespressif32/tools/sdk/esp32/include"
    / "freertos/include/esp_additions/freertos/FreeRTOSConfig.h"
)

UPSTREAM_REPOSITORY = "https://github.com/pschatzmann/ESP32-A2DP"
UPSTREAM_TAG = "v1.8.11"
UPSTREAM_COMMIT = "b6da4744286ca15b6f1dee607c077a4747294dd4"
LICENSE_SHA256 = "2823c51aa01fd0cb6655ae61ff5152bccd4675840e259ad3052a9530bdd772cf"
PATCH_SHA256 = "026ddc605cdf63068f9462cae7af437bcedf385e82222d529e6cb482fa3f10eb"
MODIFIED_FILES = {
    "src/BluetoothA2DPSinkQueued.cpp",
    "src/BluetoothA2DPSinkQueued.h",
}
PROVENANCE_FILES = {"FORK_NOTES.md", "fork-manifest.json"}
ENVIRONMENTS = {
    "a2dp-queued-fork-delay5": ("5", "1440", "22"),
    "a2dp-queued-fork-delay0": ("0", "1440", "22"),
    "a2dp-queued-fork-delay0-write4096": ("0", "4096", "22"),
    "a2dp-queued-fork-d0-w4096-p2": ("0", "4096", "2"),
}


class VerificationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git(*args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(UPSTREAM), *args],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    require(result.returncode == 0, f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def environment_section(text: str, name: str) -> str:
    match = re.search(
        rf"^\[env:{re.escape(name)}\]\s*$\n(.*?)(?=^\[env:|\Z)",
        text,
        flags=re.MULTILINE | re.DOTALL,
    )
    require(match is not None, f"missing PlatformIO environment: {name}")
    return match.group(1)


def verify_provenance(manifest: dict) -> None:
    upstream = manifest.get("upstream", {})
    require(upstream.get("repository") == UPSTREAM_REPOSITORY, "wrong upstream repository")
    require(upstream.get("tag") == UPSTREAM_TAG, "wrong upstream tag")
    require(upstream.get("commit") == UPSTREAM_COMMIT, "wrong upstream commit")
    require(upstream.get("license") == "Apache-2.0", "wrong license identifier")
    require(upstream.get("license_sha256") == LICENSE_SHA256, "wrong recorded license digest")
    require(UPSTREAM.is_dir(), f"verified upstream checkout is missing: {UPSTREAM}")
    require(git("rev-parse", "HEAD") == UPSTREAM_COMMIT, "installed upstream commit mismatch")
    tags = set(git("tag", "--points-at", "HEAD").splitlines())
    require(UPSTREAM_TAG in tags, "installed upstream tag mismatch")
    require(git("status", "--short") == "", "installed upstream checkout is modified")


def verify_snapshot(manifest: dict) -> None:
    entries = manifest.get("files")
    require(isinstance(entries, list) and entries, "manifest file list is empty")
    by_path = {entry["path"]: entry for entry in entries}
    require(len(by_path) == len(entries), "manifest contains duplicate paths")

    recorded_modified = set(manifest.get("modified_files", []))
    require(recorded_modified == MODIFIED_FILES, "modified file allowlist mismatch")

    actual_files = {
        path.relative_to(FORK).as_posix()
        for path in FORK.rglob("*")
        if path.is_file()
    }
    expected_files = set(by_path) | PROVENANCE_FILES
    require(actual_files == expected_files, "fork file list differs from manifest")

    detected_modified: set[str] = set()
    for relative, entry in by_path.items():
        upstream_path = UPSTREAM / relative
        fork_path = FORK / relative
        require(upstream_path.is_file(), f"missing upstream file: {relative}")
        require(fork_path.is_file(), f"missing fork file: {relative}")
        original_hash = sha256(upstream_path)
        fork_hash = sha256(fork_path)
        require(original_hash == entry["upstream_sha256"], f"upstream hash mismatch: {relative}")
        require(fork_hash == entry["fork_sha256"], f"fork hash mismatch: {relative}")
        if original_hash != fork_hash:
            detected_modified.add(relative)

    require(detected_modified == MODIFIED_FILES, "files modified outside allowlist")
    require(sha256(FORK / "LICENSE") == LICENSE_SHA256, "fork LICENSE digest mismatch")
    properties = (FORK / "library.properties").read_text(encoding="utf-8")
    require(re.search(r"(?m)^name=ESP32-A2DP\s*$", properties) is not None, "library name changed")
    require(re.search(r"(?m)^version=1\.8\.11\s*$", properties) is not None, "library version changed")


def verify_patch_and_api() -> None:
    header = (FORK / "src" / "BluetoothA2DPSinkQueued.h").read_text(encoding="utf-8")
    implementation = (FORK / "src" / "BluetoothA2DPSinkQueued.cpp").read_text(encoding="utf-8")
    require("uint32_t i2s_post_write_delay_ms = 5;" in header, "default delay is not 5 ms")
    require(
        "void set_i2s_post_write_delay_ms(uint32_t delay_ms)" in header
        and "void BluetoothA2DPSinkQueued::set_i2s_post_write_delay_ms(uint32_t delay_ms)" in implementation
        and "i2s_post_write_delay_ms = delay_ms;" in implementation,
        "setter is missing or does more than the recorded snapshot",
    )
    require("delay_ms(5);" not in implementation, "direct post-write delay_ms(5) remains")
    require(
        "void set_i2s_task_priority(UBaseType_t prio) { i2s_task_priority = prio; }"
        in header,
        "upstream priority setter signature or behavior changed",
    )
    require(
        "UBaseType_t i2s_task_priority = configMAX_PRIORITIES - 3;" in header,
        "upstream queued-task priority default changed",
    )
    require(
        "xTaskCreatePinnedToCore(ccall_i2s_task_handler, \"BtI2STask\", i2s_stack_size, nullptr, i2s_task_priority, &s_bt_i2s_task_handle, task_core)"
        in implementation,
        "queued-task creation no longer uses the configured priority/core/stack",
    )
    require(FREERTOS_CONFIG.is_file(), "installed FreeRTOSConfig.h is missing")
    freertos_config = FREERTOS_CONFIG.read_text(encoding="utf-8")
    require(
        re.search(r"#define\s+configMAX_PRIORITIES\s+\(\s*25\s*\)", freertos_config)
        is not None,
        "configMAX_PRIORITIES is not the verified value 25",
    )
    require(
        "if (i2s_post_write_delay_ms > 0)" in implementation
        and "delay_ms(i2s_post_write_delay_ms);" in implementation,
        "conditional configurable delay is missing",
    )
    require(PATCH.is_file(), "recorded upstream patch is missing")
    require(sha256(PATCH) == PATCH_SHA256, "recorded upstream patch hash changed")
    result = subprocess.run(
        ["git", "-C", str(UPSTREAM), "apply", "--check", "--whitespace=nowarn", str(PATCH)],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    require(result.returncode == 0, f"recorded patch does not apply to v1.8.11: {result.stderr.strip()}")


def verify_ab_source_and_environments() -> None:
    source = AB_SOURCE.read_text(encoding="utf-8")
    require(
        "#if A2DP_POST_WRITE_DELAY_MS != 0 && A2DP_POST_WRITE_DELAY_MS != 5" in source,
        "compile-time 0/5 validation is missing",
    )
    require(
        "a2dp_sink.set_i2s_post_write_delay_ms(A2DP_POST_WRITE_DELAY_MS);" in source,
        "delay setter is not called from the A/B source",
    )
    require(
        "#if A2DP_I2S_WRITE_SIZE_UPTO != 1440 && A2DP_I2S_WRITE_SIZE_UPTO != 4096"
        in source,
        "compile-time 1440/4096 validation is missing",
    )
    require(
        "a2dp_sink.set_i2s_write_size_upto(A2DP_I2S_WRITE_SIZE_UPTO);" in source,
        "write-size setter is not called from the A/B source",
    )
    require(
        "A2DP_I2S_TASK_PRIORITY <= 0 || A2DP_I2S_TASK_PRIORITY >= configMAX_PRIORITIES"
        in source,
        "priority range validation is missing",
    )
    require(
        "a2dp_sink.set_i2s_task_priority(A2DP_I2S_TASK_PRIORITY);" in source,
        "priority setter is not called from the A/B source",
    )
    require(
        source.index("set_i2s_post_write_delay_ms") < source.index("a2dp_sink.start"),
        "delay setter must be called before start()",
    )
    require(
        source.index("set_i2s_write_size_upto") < source.index("a2dp_sink.start"),
        "write-size setter must be called before start()",
    )
    require(
        source.index("set_i2s_task_priority") < source.index("a2dp_sink.start"),
        "priority setter must be called before start()",
    )
    for forbidden in (
        "xTaskCreate",
        "xQueueCreate",
        "xRingbufferCreate",
        "set_task_core",
        "set_i2s_stack_size",
        "Resample",
        "WiFi.begin",
    ):
        require(forbidden not in source, f"forbidden custom pipeline marker in A/B source: {forbidden}")

    ini = PLATFORMIO.read_text(encoding="utf-8")
    sections: dict[str, str] = {}
    for name, (delay, write_size, priority) in ENVIRONMENTS.items():
        section = environment_section(ini, name)
        sections[name] = section
        require(
            section.count("file://third_party/ESP32-A2DP-delay-configurable") == 1,
            f"{name} must use exactly one local fork dependency",
        )
        require("ESP32-A2DP.git" not in section, f"{name} also references upstream ESP32-A2DP")
        require(
            "+<a2dp_queued_fork_delay_ab.cpp>" in section,
            f"{name} does not build the shared A/B source",
        )
        require(
            f"-DA2DP_POST_WRITE_DELAY_MS={delay}" in section,
            f"{name} has the wrong delay macro",
        )
        require(
            f"-DA2DP_I2S_WRITE_SIZE_UPTO={write_size}" in section,
            f"{name} has the wrong write-size macro",
        )
        require(
            f"-DA2DP_I2S_TASK_PRIORITY={priority}" in section,
            f"{name} has the wrong priority macro",
        )
        require("espressif32@7.1.3" in section, f"{name} platform changed")
        require("huge_app.csv" in section, f"{name} partition layout changed")
        require("monitor_speed = 115200" in section, f"{name} monitor speed changed")
        require("arduino-audio-tools.git#v1.2.5" in section, f"{name} AudioTools version changed")

    normalized_sections = []
    for name, (delay, write_size, priority) in ENVIRONMENTS.items():
        normalized = sections[name].strip()
        normalized = normalized.replace(
            f"-DA2DP_POST_WRITE_DELAY_MS={delay}",
            "-DA2DP_POST_WRITE_DELAY_MS=X",
        )
        normalized = normalized.replace(
            f"-DA2DP_I2S_WRITE_SIZE_UPTO={write_size}",
            "-DA2DP_I2S_WRITE_SIZE_UPTO=X",
        )
        normalized = normalized.replace(
            f"-DA2DP_I2S_TASK_PRIORITY={priority}",
            "-DA2DP_I2S_TASK_PRIORITY=X",
        )
        normalized_sections.append(normalized)
    require(
        len(set(normalized_sections)) == 1,
        "A/B environment bodies differ beyond the delay/write-size/priority macros",
    )


def main() -> int:
    try:
        require(MANIFEST.is_file(), "fork manifest is missing")
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        require(manifest.get("schema_version") == 1, "unsupported manifest schema")
        verify_provenance(manifest)
        verify_snapshot(manifest)
        verify_patch_and_api()
        verify_ab_source_and_environments()
    except (OSError, ValueError, VerificationError) as error:
        print(f"A2DP_DELAY_FORK_VERIFY: FAIL: {error}", file=sys.stderr)
        return 1

    print(f"A2DP_DELAY_FORK_VERIFY: PASS ({len(manifest['files'])} upstream files, 2 modified)")
    print(f"upstream={UPSTREAM_TAG}@{UPSTREAM_COMMIT}")
    print(
        "environments: delay5=5 ms/1440 B/P22, delay0=0 ms/1440 B/P22, "
        "delay0-write4096=0 ms/4096 B/P22, d0-w4096-p2=0 ms/4096 B/P2"
    )
    print("all environments use one shared local fork and source")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
