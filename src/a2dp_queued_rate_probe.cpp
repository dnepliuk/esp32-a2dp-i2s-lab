#include "a2dp_rate_probe_common.h"

namespace {

constexpr a2dp_rate_probe::ProbeVariant kVariant = {
    "queued-rate-probe",
    "Faital A2DP Rate Probe",
    false,
};

}  // namespace

void setup() { a2dp_rate_probe::setupProbe(kVariant); }

void loop() { a2dp_rate_probe::loopProbe(); }
