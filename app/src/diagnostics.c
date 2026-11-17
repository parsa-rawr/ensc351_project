#include "diagnostics.h"
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

static int ioError(const char *operation)
{
    perror(operation);
    puts("RESULT ERROR: measurements are incomplete; no PASS result.");
    return DIAGNOSTIC_ERROR;
}
//convert the twelve bit code using the configured reference voltage
static bool readVoltage(Hardware *hw, const Test_config *c, unsigned channel,
                        uint16_t *raw, double *volts)
{
    if (!Hardware_readAdc(hw, channel, raw)) return false;
    *volts = *raw / 4096.0 * c->reference_volts;
    return true;
}
static int runAnalog(Hardware *hw, const Test_config *c,
                     const volatile sig_atomic_t *stop)
{
    double sum = 0, minimum = 0, maximum = 0;
    for (unsigned i = 0; i < c->samples; i++) {
        if (*stop) { errno = EINTR; return ioError("ADC interrupted"); }
        uint16_t raw;
        double volts;
        if (!readVoltage(hw, c, c->adc_channel, &raw, &volts)) return ioError("ADC transfer");
        if (!i || volts < minimum) minimum = volts;
        if (!i || volts > maximum) maximum = volts;
        sum += volts;
        if (c->kind == TEST_ADC)
            printf("ADC sample=%u raw=%u volts=%.4f\n", i, (unsigned)raw, volts);
        if (i + 1 < c->samples && !Hardware_wait(hw, 0.001, stop)) return ioError("ADC interval");
    }
    double average = sum / c->samples;
    printf("ADC SUMMARY samples=%u min=%.4fV max=%.4fV mean=%.4fV\n",
           c->samples, minimum, maximum, average);
    if (c->kind == TEST_ADC) {
        puts("RESULT MEASURED: compare against a multimeter; no circuit limits applied.");
        return DIAGNOSTIC_PASS;
    }
    double expected = c->supply_volts * c->bottom_ohms / (c->top_ohms + c->bottom_ohms);
    //check every sample so opposite errors cannot cancel in the average
    bool passed = fabs(average - expected) <= c->tolerance_volts
               && minimum >= expected - c->tolerance_volts
               && maximum <= expected + c->tolerance_volts;
    printf("DIVIDER expected=%.4fV measured=%.4fV error=%+.4fV tolerance=+/-%.4fV\n",
           expected, average, average - expected, c->tolerance_volts);
    printf("RESULT %s: voltage divider\n", passed ? "PASS" : "FAIL");
    return passed ? DIAGNOSTIC_PASS : DIAGNOSTIC_FAIL;
}
//check both digital levels without requiring an external logic chip
static int runLoopback(Hardware *hw, const volatile sig_atomic_t *stop)
{
    bool passed = true;
    puts("GPIO LOOPBACK: drive wired to sense; disconnect RC drive first.");
    for (unsigned row = 0; row < 2; row++) {
        if (!Hardware_setDrive(hw, row != 0) || !Hardware_wait(hw, 0.005, stop))
            return ioError("GPIO stimulus");
        unsigned matching = 0;
        for (unsigned i = 0; i < 5; i++) {
            bool measured;
            if (!Hardware_readOutput(hw, &measured)) return ioError("GPIO observation");
            if (measured == (row != 0)) matching++;
            if (!Hardware_wait(hw, 0.001, stop)) return ioError("GPIO interval");
        }
        printf("drive=%u matching_reads=%u/5 %s\n", row, matching, matching == 5 ? "PASS" : "FAIL");
        passed = passed && matching == 5;
    }
    if (!Hardware_setDrive(hw, false)) return ioError("GPIO idle reset");
    printf("RESULT %s: GPIO loopback\n", passed ? "PASS" : "FAIL");
    return passed ? DIAGNOSTIC_PASS : DIAGNOSTIC_FAIL;
}
//sample a complete phase, using actual monotonic timestamps, not sample counts.
//a threshold bracket is linearly interpolated. excessive sampling gaps are an
//acquisition error, rather than a circuit fail or a guessed time constant.
static bool samplePhase(Hardware *hw, const Test_config *c,
    const volatile sig_atomic_t *stop, FILE *csv, const char *phase,
    double start, bool rising, double threshold, double *crossing, double *last)
{
    double previous_t = 0, previous_v = 0;
    bool first = true;
    *crossing = NAN;
    for (;;) {
        if (*stop) { errno = EINTR; return false; }
        double before, after, volts;
        uint16_t raw;
        if (!Hardware_now(hw, &before) || !readVoltage(hw, c, c->rc_channel, &raw, &volts)
            || !Hardware_now(hw, &after)) return false;
        double t = (before + after) * 0.5 - start;
        double expected_tau = c->resistance_ohms * c->capacitance_uf * 1e-6;
        //large timing gaps make the acquisition unreliable
        if ((first && t > expected_tau * 0.2) || (after - before) > expected_tau * 0.2
            || (!first && t - previous_t > expected_tau * 0.2)) {
            errno = ETIMEDOUT; return false;
        }
        if (csv && fprintf(csv, "%s,%.9f,%u,%.7f\n", phase, t, (unsigned)raw, volts) < 0)
            return false;
        //estimate the threshold time between the two surrounding samples
        if (!first && isnan(*crossing) && isfinite(threshold)) {
            bool bracket = rising ? previous_v < threshold && volts >= threshold
                                  : previous_v > threshold && volts <= threshold;
            if (bracket) *crossing = previous_t + (threshold - previous_v)
                / (volts - previous_v) * (t - previous_t);
        }
        *last = volts;
        previous_t = t;
        previous_v = volts;
        first = false;
        //avoid a final tiny delay getting stuck on floating point rounding
        if (t + 1e-9 >= c->window_s) break;
        //target the next deadline without assuming nanosleep is exact.
        double delay = fmin(c->interval_s, c->window_s - t);
        if (!Hardware_wait(hw, delay, stop)) return false;
    }
    return true;
}
static int measureRC(Hardware *hw, const Test_config *c,
                     const volatile sig_atomic_t *stop, FILE *csv)
{
    //convert microfarads to farads before calculating the expected time constant
    double expected = c->resistance_ohms * c->capacitance_uf * 1e-6;
    printf("RC expected_tau=%.6fs R=%.0fohm C=%.3fuF tolerance=+/-%.1f%% interval=%.4fs window=%.3fs\n",
           expected, c->resistance_ohms, c->capacitance_uf,
           c->tau_tolerance * 100, c->interval_s, c->window_s);
    double start, unused, low, high, final_low, charging, discharging;
    //start from a discharged capacitor before measuring the rising curve
    if (!Hardware_setDrive(hw, false) || !Hardware_now(hw, &start)
        || !samplePhase(hw, c, stop, csv, "prepare", start, false, NAN, &unused, &low))
        return ioError("RC initial discharge");
    if (low > c->tolerance_volts) {
        printf("RESULT FAIL: RC initial voltage %.4fV is not discharged\n", low);
        return DIAGNOSTIC_FAIL;
    }
    //timestamp before the gpio change so command latency is included.
    //one charging time constant reaches about sixty three percent of the swing
    double threshold = low + (c->supply_volts - low) * (1.0 - exp(-1.0));
    if (!Hardware_now(hw, &start) || !Hardware_setDrive(hw, true)
        || !samplePhase(hw, c, stop, csv, "charge", start, true, threshold, &charging, &high))
        return ioError("RC charging acquisition");
    //one discharging time constant leaves about thirty seven percent of the voltage
    if (!Hardware_now(hw, &start) || !Hardware_setDrive(hw, false)
        || !samplePhase(hw, c, stop, csv, "discharge", start, false, high * exp(-1.0),
                        &discharging, &final_low)) return ioError("RC discharging acquisition");
    //both curves and their final voltages must agree with the expected circuit
    bool endpoints = fabs(high - c->supply_volts) <= c->tolerance_volts
                  && final_low <= c->tolerance_volts;
    bool passed = endpoints && isfinite(charging) && isfinite(discharging)
        && fabs(charging - expected) <= expected * c->tau_tolerance
        && fabs(discharging - expected) <= expected * c->tau_tolerance;
    printf("RC endpoints initial=%.4fV charged=%.4fV discharged=%.4fV\n", low, high, final_low);
    if (isfinite(charging)) printf("RC charge_tau=%.6fs\n", charging);
    else puts("RC charge threshold not crossed within acquisition window");
    if (isfinite(discharging)) printf("RC discharge_tau=%.6fs\n", discharging);
    else puts("RC discharge threshold not crossed within acquisition window");
    printf("RESULT %s: RC charge/discharge\n", passed ? "PASS" : "FAIL");
    return passed ? DIAGNOSTIC_PASS : DIAGNOSTIC_FAIL;
}
static int runRC(Hardware *hw, const Test_config *c,
                 const volatile sig_atomic_t *stop)
{
    FILE *csv = NULL;
    if (c->csv_path) {
        //exclusive create avoids overwriting an earlier measurement capture.
        csv = fopen(c->csv_path, "wx");
        if (!csv) return ioError("RC CSV open (choose a new filename)");
        if (fprintf(csv, "phase,time_s,raw,volts\n") < 0) {
            int saved = errno;
            (void)fclose(csv); errno = saved;
            return ioError("RC CSV header");
        }
    }
    //reset the drive and close the capture even if acquisition fails
    int result = measureRC(hw, c, stop, csv);
    if (!Hardware_setDrive(hw, false)) result = ioError("RC drive reset");
    if (csv && fclose(csv)) result = ioError("RC CSV close");
    if (csv) printf("RC CSV capture: %s (partial if interrupted or errored)\n", c->csv_path);
    return result;
}
int Diagnostics_run(Hardware *hw, const Test_config *c,
                    const volatile sig_atomic_t *stop)
{
    if (c->kind == TEST_ADC || c->kind == TEST_DIVIDER) return runAnalog(hw, c, stop);
    if (c->kind == TEST_LOOPBACK) return runLoopback(hw, stop);
    if (c->kind == TEST_RC) return runRC(hw, c, stop);
    //combined mode uses separate adc channels for the two wired circuits
    int analog = runAnalog(hw, c, stop);
    if (analog == DIAGNOSTIC_ERROR) return analog;
    int rc = runRC(hw, c, stop);
    if (rc == DIAGNOSTIC_ERROR) return rc;
    bool passed = analog == DIAGNOSTIC_PASS && rc == DIAGNOSTIC_PASS;
    printf("OVERALL %s\n", passed ? "PASS" : "FAIL");
    return passed ? DIAGNOSTIC_PASS : DIAGNOSTIC_FAIL;
}
