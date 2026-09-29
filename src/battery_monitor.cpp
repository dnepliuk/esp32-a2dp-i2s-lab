#include "battery_monitor.h"

#include <math.h>

#include "app_config.h"

namespace {

static_assert(AppConfig::kBatterySampleCount >= 16,
              "Battery filter requires at least 16 samples");
static_assert(2U * AppConfig::kBatteryTrimCountPerEnd <
                  AppConfig::kBatterySampleCount,
              "Battery trimmed mean must retain samples");

struct BatteryCurvePoint {
  float voltage;
  uint8_t percent;
};

// Resting-voltage estimate for a 3S Li-ion pack. This is an indicator, not a
// coulomb counter; amplifier load sag and recovery after Pause are expected.
constexpr BatteryCurvePoint kBatteryCurve[] = {
    {10.20f, 0},  {10.80f, 10}, {11.10f, 20}, {11.40f, 35},
    {11.70f, 55}, {12.00f, 75}, {12.30f, 90}, {12.60f, 100},
};

uint32_t s_samples[AppConfig::kBatterySampleCount] = {};
uint8_t s_sampleCount = 0;
uint32_t s_lastSampleAt = 0;
uint32_t s_lastPublishAt = 0;
uint32_t s_lastLogAt = 0;
uint32_t s_filteredAdcMillivolts = 0;
float s_emaVoltage = 0.0f;
float s_publishedVoltage = 0.0f;
uint8_t s_percent = 0;
bool s_emaInitialized = false;
bool s_hasPublished = false;
bool s_valid = false;
bool s_low = false;
bool s_newSample = false;
bool s_batchReady = false;
bool s_logInitialized = false;

float dividerRatio() {
  return static_cast<float>(AppConfig::kBatteryDividerHighOhm +
                            AppConfig::kBatteryDividerLowOhm) /
         static_cast<float>(AppConfig::kBatteryDividerLowOhm);
}

uint32_t trimmedMeanMillivolts() {
  uint64_t sum = 0;
  for (uint8_t i = 0; i < AppConfig::kBatterySampleCount; ++i) {
    sum += s_samples[i];
  }

  // Remove one low and one high sample per pass. Mark removed entries with
  // sentinels; the static buffer is overwritten by the next sampling batch.
  for (uint8_t trim = 0; trim < AppConfig::kBatteryTrimCountPerEnd; ++trim) {
    uint8_t minIndex = 0;
    uint8_t maxIndex = 0;
    for (uint8_t i = 1; i < AppConfig::kBatterySampleCount; ++i) {
      if (s_samples[i] < s_samples[minIndex]) {
        minIndex = i;
      }
      if (s_samples[i] > s_samples[maxIndex]) {
        maxIndex = i;
      }
    }

    sum -= s_samples[minIndex];
    s_samples[minIndex] = UINT32_MAX;

    // Re-find the maximum if it was also the removed minimum (only possible
    // when all retained readings are identical).
    maxIndex = 0;
    for (uint8_t i = 1; i < AppConfig::kBatterySampleCount; ++i) {
      if (s_samples[i] != UINT32_MAX &&
          (s_samples[maxIndex] == UINT32_MAX ||
           s_samples[i] > s_samples[maxIndex])) {
        maxIndex = i;
      }
    }
    sum -= s_samples[maxIndex];
    s_samples[maxIndex] = UINT32_MAX;
  }

  constexpr uint8_t kRetained =
      AppConfig::kBatterySampleCount -
      2U * AppConfig::kBatteryTrimCountPerEnd;
  return static_cast<uint32_t>((sum + kRetained / 2U) / kRetained);
}

uint8_t voltageToPercent(float voltage) {
  constexpr size_t kPointCount = sizeof(kBatteryCurve) / sizeof(kBatteryCurve[0]);
  if (voltage <= kBatteryCurve[0].voltage) {
    return 0;
  }
  if (voltage >= kBatteryCurve[kPointCount - 1].voltage) {
    return 100;
  }

  for (size_t i = 1; i < kPointCount; ++i) {
    if (voltage <= kBatteryCurve[i].voltage) {
      const BatteryCurvePoint& low = kBatteryCurve[i - 1];
      const BatteryCurvePoint& high = kBatteryCurve[i];
      const float fraction =
          (voltage - low.voltage) / (high.voltage - low.voltage);
      const float interpolated =
          low.percent + fraction * (high.percent - low.percent);
      const int rounded = static_cast<int>(interpolated + 0.5f);
      return static_cast<uint8_t>(constrain(rounded, 0, 100));
    }
  }
  return 100;
}

const char* stateName() {
  if (!s_valid) {
    return "invalid";
  }
  return s_low ? "low" : "normal";
}

void logPublishedReading(uint32_t nowMs) {
  if (s_logInitialized &&
      static_cast<uint32_t>(nowMs - s_lastLogAt) <
          AppConfig::kBatteryLogIntervalMs) {
    return;
  }
  s_logInitialized = true;
  s_lastLogAt = nowMs;

  Serial.print("BATTERY: adc=");
  Serial.print(s_filteredAdcMillivolts);
  Serial.print(" mV pack=");
  if (s_valid) {
    Serial.print(s_publishedVoltage, 2);
    Serial.print(" V percent=");
    Serial.print(s_percent);
    Serial.print("% state=");
  } else {
    Serial.print("--.- V percent=-- state=");
  }
  Serial.println(stateName());
}

void publishReading(uint32_t nowMs) {
  const bool previousLow = s_low;
  const bool previousValid = s_valid;
  s_publishedVoltage = s_emaVoltage;
  s_valid = isfinite(s_publishedVoltage) &&
            s_filteredAdcMillivolts > 0U &&
            s_filteredAdcMillivolts <= AppConfig::kBatteryAdcMaxMillivolts &&
            s_publishedVoltage >= AppConfig::kBatteryValidMinV &&
            s_publishedVoltage <= AppConfig::kBatteryValidMaxV;

  if (!s_valid) {
    s_percent = 0;
    s_low = false;
  } else {
    s_percent = voltageToPercent(s_publishedVoltage);
    if (s_low) {
      if (s_publishedVoltage >= AppConfig::kBatteryLowThresholdV +
                                    AppConfig::kBatteryLowHysteresisV) {
        s_low = false;
      }
    } else if (s_publishedVoltage <= AppConfig::kBatteryLowThresholdV) {
      s_low = true;
    }
  }

  if (s_valid && s_low && (!previousValid || !previousLow)) {
    Serial.println("BATTERY: LOW BATTERY entered");
  } else if (s_valid && previousValid && previousLow && !s_low) {
    Serial.println("BATTERY: LOW BATTERY cleared");
  }

  s_newSample = true;
  s_lastPublishAt = nowMs;
  logPublishedReading(nowMs);
}

void finishBatch(uint32_t nowMs) {
  s_filteredAdcMillivolts = trimmedMeanMillivolts();
  const float adcVoltage = s_filteredAdcMillivolts / 1000.0f;
  const float packVoltage =
      adcVoltage * dividerRatio() * AppConfig::kBatteryCalibrationGain +
      AppConfig::kBatteryCalibrationOffsetV;

  const bool batchValid = isfinite(packVoltage) &&
                          s_filteredAdcMillivolts > 0U &&
                          s_filteredAdcMillivolts <=
                              AppConfig::kBatteryAdcMaxMillivolts &&
                          packVoltage >= AppConfig::kBatteryValidMinV &&
                          packVoltage <= AppConfig::kBatteryValidMaxV;
  if (!batchValid) {
    // Do not let a disconnected divider or saturated ADC poison the EMA.
    s_emaVoltage = packVoltage;
    s_emaInitialized = false;
  } else if (!s_emaInitialized) {
    s_emaVoltage = packVoltage;
    s_emaInitialized = true;
  } else {
    s_emaVoltage +=
        AppConfig::kBatteryEmaAlpha * (packVoltage - s_emaVoltage);
  }

  s_batchReady = true;
  if (!s_hasPublished ||
      static_cast<uint32_t>(nowMs - s_lastPublishAt) >=
          AppConfig::kBatteryPublishIntervalMs) {
    publishReading(nowMs);
    s_hasPublished = true;
    s_batchReady = false;
  }
}

}  // namespace

bool battery_monitor_begin() {
  analogReadResolution(AppConfig::kBatteryAdcResolutionBits);
  analogSetPinAttenuation(AppConfig::kBatteryAdcPin,
                          AppConfig::kBatteryAdcAttenuation);

  s_sampleCount = 0;
  s_lastSampleAt = millis() - AppConfig::kBatterySampleIntervalMs;
  s_lastPublishAt = millis();
  s_lastLogAt = 0;
  s_filteredAdcMillivolts = 0;
  s_emaVoltage = 0.0f;
  s_publishedVoltage = 0.0f;
  s_percent = 0;
  s_emaInitialized = false;
  s_hasPublished = false;
  s_valid = false;
  s_low = false;
  s_newSample = false;
  s_batchReady = false;
  s_logInitialized = false;

  Serial.print("BATTERY: ADC GPIO=");
  Serial.print(AppConfig::kBatteryAdcPin);
  Serial.print(" divider=");
  Serial.print(AppConfig::kBatteryDividerHighOhm);
  Serial.print('/');
  Serial.println(AppConfig::kBatteryDividerLowOhm);
  Serial.print("BATTERY: ADC resolution=");
  Serial.print(AppConfig::kBatteryAdcResolutionBits);
  Serial.println(" attenuation=ADC_11db");
  Serial.print("BATTERY: calibration gain=");
  Serial.print(AppConfig::kBatteryCalibrationGain, 6);
  Serial.print(" offset=");
  Serial.print(AppConfig::kBatteryCalibrationOffsetV, 3);
  Serial.println(" V");
  return true;
}

void battery_monitor_update(uint32_t now_ms) {
  if (static_cast<uint32_t>(now_ms - s_lastSampleAt) >=
      AppConfig::kBatterySampleIntervalMs) {
    s_lastSampleAt = now_ms;
    s_samples[s_sampleCount++] =
        analogReadMilliVolts(AppConfig::kBatteryAdcPin);
    if (s_sampleCount == AppConfig::kBatterySampleCount) {
      s_sampleCount = 0;
      finishBatch(now_ms);
    }
  }

  if (s_batchReady &&
      static_cast<uint32_t>(now_ms - s_lastPublishAt) >=
          AppConfig::kBatteryPublishIntervalMs) {
    publishReading(now_ms);
    s_hasPublished = true;
    s_batchReady = false;
  }
}

bool battery_monitor_has_new_sample() {
  const bool result = s_newSample;
  s_newSample = false;
  return result;
}

float battery_monitor_voltage() { return s_publishedVoltage; }

uint8_t battery_monitor_percent() { return s_percent; }

bool battery_monitor_is_low() { return s_valid && s_low; }

bool battery_monitor_is_valid() { return s_valid; }

uint32_t battery_monitor_adc_millivolts() {
  return s_filteredAdcMillivolts;
}
