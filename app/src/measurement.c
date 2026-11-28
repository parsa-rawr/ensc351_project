// Analyze timestamped ADC readings from stimulus produced by Board 3.
// PWM uses the Assignment 2 sampler's hysteresis and monotonic interval statistics.
#include "measurement.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const names[] = { "DIVIDER", "RC", "PWM" };
static const double endpoint_tolerance = 0.10;
static const double signal_floor = 0.08;
static const double start_timeout = 3.0;
static const double rc_phase_time_constants = 7.0;

const char *Measurement_name(Measurement_kind kind)
{
    return names[kind];
}

bool Measurement_kindFromName(const char *name, Measurement_kind *kind)
{
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(name, names[i])) {
            *kind = (Measurement_kind)i;
            return true;
        }
    }
    return false;
}

void Measurement_begin(Measurement *m, Measurement_kind kind, double now)
{
    *m = (Measurement){
        .kind = kind, .start = now, .first_signal = NAN, .last_edge = NAN, .fall_started = NAN,
        .charge_10 = NAN, .charge_60 = NAN, .discharge_85 = NAN, .discharge_35 = NAN,
    };
}

double Measurement_interval(Measurement_kind kind)
{
    return kind == MEASURE_RC ? 0.005 : 0.001;
}

static void add(Measurement_stats *stats, double value)
{
    if (!stats->count || value < stats->minimum) stats->minimum = value;
    if (!stats->count || value > stats->maximum) stats->maximum = value;
    stats->sum += value;
    stats->count++;
}

static double crossing(const Measurement *m, double time, double volts, double level)
{
    return m->previous_time + (time - m->previous_time)
           * (level - m->previous_v) / (volts - m->previous_v);
}

static void recordCrossing(const Measurement *m, double time, double volts,
                           double level, bool rising, double *out)
{
    if (!isnan(*out) || !m->volts.count) return;
    bool bracket = rising ? m->previous_v < level && volts >= level
                          : m->previous_v > level && volts <= level;
    if (bracket) *out = crossing(m, time, volts, level);
}

static bool sampleDivider(Measurement *m, double time, double volts)
{
    if (isnan(m->first_signal) && volts >= signal_floor) m->first_signal = time;
    if (isnan(m->first_signal)) {
        m->previous_v = volts;
        return time - m->start >= start_timeout;
    }
    if (time - m->first_signal >= 0.10) add(&m->volts, volts);
    return m->volts.count >= 32;
}

static bool sampleRc(Measurement *m, const Measurement_expectations *e,
                     double time, double volts)
{
    if (isnan(m->first_signal) && volts >= signal_floor) m->first_signal = time;
    if (isnan(m->fall_started)) {
        m->peak = fmax(m->peak, volts);
        // Allow ADC noise while looking for the start of Board 3's falling phase.
        if (m->peak >= 0.20 && volts < m->peak - 0.06) m->fall_started = time;
    }
    recordCrossing(m, time, volts, e->supply_v * 0.10, true, &m->charge_10);
    recordCrossing(m, time, volts, e->supply_v * 0.60, true, &m->charge_60);
    if (!isnan(m->fall_started)) {
        recordCrossing(m, time, volts, m->peak * 0.85, false, &m->discharge_85);
        recordCrossing(m, time, volts, m->peak * 0.35, false, &m->discharge_35);
    }
    add(&m->volts, volts);
    if (isnan(m->first_signal)) return time - m->start >= start_timeout;
    // RESULT triggers the controller's ABORT. Give Board 3 its full low phase first.
    if (!isnan(m->fall_started))
        return time - m->fall_started >= rc_phase_time_constants * e->rc_tau_s;
    return time - m->first_signal >= 15 * e->rc_tau_s;
}

static bool samplePwm(Measurement *m, const Measurement_expectations *e,
                      double time, double volts)
{
    // Arm after observing LOW. Interpolate the upper threshold on the next rise.
    const double high = e->pwm_threshold_v + 0.05;
    const double low = e->pwm_threshold_v - 0.05;
    if (!m->volts.count) m->pwm_high = volts > low;
    if (volts <= low) m->pwm_high = false;
    if (!m->pwm_high && volts >= high) {
        double edge = crossing(m, time, volts, high);
        if (isnan(m->first_signal)) m->first_signal = edge;
        if (!isnan(m->last_edge)) add(&m->period_ms, (edge - m->last_edge) * 1000);
        m->last_edge = edge;
        m->pwm_high = true;
    }
    add(&m->volts, volts);
    if (isnan(m->first_signal)) return time - m->start >= start_timeout;
    return time - m->first_signal >= fmax(1.0, 4.0 / e->pwm_hz);
}

bool Measurement_sample(Measurement *m, const Measurement_expectations *e,
                        double time, double volts)
{
    if (m->previous_time != 0) add(&m->sample_ms, (time - m->previous_time) * 1000);
    bool complete = false;
    switch (m->kind) {
    case MEASURE_DIVIDER: complete = sampleDivider(m, time, volts); break;
    case MEASURE_RC: complete = sampleRc(m, e, time, volts); break;
    case MEASURE_PWM: complete = samplePwm(m, e, time, volts); break;
    }
    m->previous_v = volts;
    m->previous_time = time;
    return complete;
}

static const char *jsonNumber(double number, char buffer[32])
{
    if (!isfinite(number)) return "null";
    snprintf(buffer, 32, "%.6f", number);
    return buffer;
}

static const char *dividerFields(const Measurement *m, const Measurement_expectations *e,
                                 char *fields, size_t size)
{
    double mean = m->volts.count ? m->volts.sum / m->volts.count : m->previous_v;
    double minimum = m->volts.count ? m->volts.minimum : mean;
    double maximum = m->volts.count ? m->volts.maximum : mean;
    snprintf(fields, size,
             "\"measured_v\":%.6f,\"expected_v\":%.6f,\"min_v\":%.6f,\"max_v\":%.6f",
             mean, e->divider_v, minimum, maximum);
    if (isnan(m->first_signal)) return "NO_SIGNAL";
    if (minimum < e->divider_v - endpoint_tolerance ||
        maximum > e->divider_v + endpoint_tolerance) return "WRONG_VALUE";
    return "NONE";
}

static const char *rcFields(const Measurement *m, const Measurement_expectations *e,
                            char *fields, size_t size)
{
    // Solve the exponential over two thresholds. No shared clock or START timestamp.
    double charge = (m->charge_60 - m->charge_10) / log(0.90 / 0.40);
    double discharge = (m->discharge_35 - m->discharge_85) / log(0.85 / 0.35);
    char charge_buffer[32], discharge_buffer[32];
    snprintf(fields, size,
             "\"charge_tau_s\":%s,\"discharge_tau_s\":%s,\"expected_tau_s\":%.6f,"
             "\"peak_v\":%.6f,\"final_v\":%.6f",
             jsonNumber(charge, charge_buffer), jsonNumber(discharge, discharge_buffer),
             e->rc_tau_s, m->peak, m->previous_v);
    if (isnan(m->first_signal)) return "NO_SIGNAL";
    double tolerance = e->rc_tau_s * 0.25;
    if (!isfinite(charge) || !isfinite(discharge) ||
        fabs(charge - e->rc_tau_s) > tolerance || fabs(discharge - e->rc_tau_s) > tolerance ||
        fabs(m->peak - e->supply_v) > endpoint_tolerance ||
        m->previous_v > endpoint_tolerance) return "WRONG_VALUE";
    return "NONE";
}

static const char *pwmFields(const Measurement *m, const Measurement_expectations *e,
                             char *fields, size_t size)
{
    double frequency = m->period_ms.count ? 1000 * m->period_ms.count / m->period_ms.sum : NAN;
    char frequency_buffer[32];
    snprintf(fields, size,
             "\"frequency_hz\":%s,\"expected_hz\":%.6f,\"cycles\":%u,"
             "\"min_v\":%.6f,\"max_v\":%.6f",
             jsonNumber(frequency, frequency_buffer), e->pwm_hz, m->period_ms.count,
             m->volts.minimum, m->volts.maximum);
    if (!isfinite(frequency)) return "NO_SIGNAL";
    if (m->period_ms.count < 2 ||
        m->period_ms.minimum < 1000 / (1.10 * e->pwm_hz) ||
        m->period_ms.maximum > 1000 / (0.90 * e->pwm_hz)) return "WRONG_FREQ";
    return "NONE";
}

bool Measurement_json(const Measurement *m, const Measurement_expectations *e,
                      bool simulated, char *json, size_t size)
{
    char fields[320];
    const char *fault;
    switch (m->kind) {
    case MEASURE_DIVIDER: fault = dividerFields(m, e, fields, sizeof(fields)); break;
    case MEASURE_RC: fault = rcFields(m, e, fields, sizeof(fields)); break;
    case MEASURE_PWM: fault = pwmFields(m, e, fields, sizeof(fields)); break;
    default: return false;
    }
    const Measurement_stats *interval = &m->sample_ms;
    double mean = interval->count ? interval->sum / interval->count : 0;
    int length = snprintf(json, size,
        "{\"test_type\":\"%s\",\"status\":\"%s\",\"fault\":\"%s\",\"simulated\":%s,%s,"
        "\"sample_interval_ms\":{\"min\":%.6f,\"mean\":%.6f,\"max\":%.6f,"
        "\"jitter_pp\":%.6f,\"count\":%u}}",
        Measurement_name(m->kind), !strcmp(fault, "NONE") ? "PASS" : "FAIL", fault,
        simulated ? "true" : "false", fields, interval->minimum, mean, interval->maximum,
        interval->maximum - interval->minimum, interval->count);
    return length >= 0 && (size_t)length < size;
}
