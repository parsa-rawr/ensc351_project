#ifndef MEASUREMENT_H
#define MEASUREMENT_H

#include "hal/hardware.h"
#include <stddef.h>

typedef enum { MEASURE_DIVIDER, MEASURE_RC, MEASURE_PWM } Measurement_kind;

typedef struct {
    double sum, minimum, maximum;
    unsigned count;
} Measurement_stats;

typedef struct {
    Measurement_kind kind;
    double start, previous_time, previous_v;
    double first_signal, peak, charge_10, charge_60, discharge_85, discharge_35;
    double fall_started, last_edge;
    bool pwm_high;
    Measurement_stats volts, sample_ms, period_ms;
} Measurement;

typedef struct {
    double divider_v, rc_tau_s, supply_v, pwm_hz, pwm_threshold_v;
} Measurement_expectations;

const char *Measurement_name(Measurement_kind kind);
bool Measurement_kindFromName(const char *name, Measurement_kind *kind);
void Measurement_begin(Measurement *m, Measurement_kind kind, double now);
double Measurement_interval(Measurement_kind kind);
bool Measurement_sample(Measurement *m, const Measurement_expectations *e,
                        double time, double volts);
bool Measurement_json(const Measurement *m, const Measurement_expectations *e,
                      bool simulated, char *json, size_t size);

#endif
