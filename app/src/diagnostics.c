#include "diagnostics.h"
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

enum { ADC_MAX_CODE = 4095, ADC_INTERVAL_NS = 1000000,
       GPIO_SETTLE_NS = 5000000, GPIO_READS_PER_ROW = 5 };

static bool pauseFor(long nanoseconds, const volatile sig_atomic_t *stop)
{
    struct timespec delay = { .tv_nsec = nanoseconds };
    while (!*stop && nanosleep(&delay, &delay) < 0) {
        if (errno != EINTR) return false;
    }
    if (*stop) { errno = EINTR; return false; }
    return true;
}
static int ioError(const char *operation)
{
    perror(operation);
    puts("RESULT ERROR: measurements are incomplete; no PASS result.");
    return DIAGNOSTIC_ERROR;
}
static int runAnalog(Hardware *hardware, const Test_config *config,
                     const volatile sig_atomic_t *stop)
{
    double sum = 0, minimum = 0, maximum = 0;
    for (unsigned i = 0; i < config->samples; i++) {
        if (*stop) { errno = EINTR; return ioError("ADC interrupted"); }
        uint16_t raw;
        if (!Hardware_readAdc(hardware, &raw)) return ioError("ADC transfer");
        double volts = raw / (double)ADC_MAX_CODE * config->reference_volts;
        if (!i || volts < minimum) minimum = volts;
        if (!i || volts > maximum) maximum = volts;
        sum += volts;
        if (config->kind == TEST_ADC) {
            printf("ADC sample=%u raw=%u volts=%.4f\n", i, (unsigned)raw, volts);
        }
        if (i + 1 < config->samples && !pauseFor(ADC_INTERVAL_NS, stop)) {
            return ioError("ADC interval");
        }
    }
    double average = sum / config->samples;
    printf("ADC SUMMARY samples=%u min=%.4fV max=%.4fV mean=%.4fV\n",
           config->samples, minimum, maximum, average);
    if (config->kind == TEST_ADC) {
        puts("RESULT MEASURED: compare against a multimeter; no circuit limits applied.");
        return DIAGNOSTIC_PASS;
    }
    double expected = config->supply_volts * config->bottom_ohms
                    / (config->top_ohms + config->bottom_ohms);
    //check each sample as well as the mean; opposite rail excursions must
    //not cancel into a falsely passing average.
    bool passed = fabs(average - expected) <= config->tolerance_volts
               && minimum >= expected - config->tolerance_volts
               && maximum <= expected + config->tolerance_volts;
    printf("DIVIDER expected=%.4fV measured=%.4fV error=%+.4fV tolerance=+/-%.4fV\n",
           expected, average, average - expected, config->tolerance_volts);
    printf("RESULT %s: voltage divider\n", passed ? "PASS" : "FAIL");
    return passed ? DIAGNOSTIC_PASS : DIAGNOSTIC_FAIL;
}
static int runDigital(Hardware *hardware, bool loopback,
                      const volatile sig_atomic_t *stop)
{
    //independent datasheet truth table: rows 00, 01, 10, 11.
    static const bool nand_expected[] = {true, true, true, false};
    bool all_passed = true;
    unsigned rows = loopback ? 2 : 4;
    puts(loopback ? "GPIO LOOPBACK (A wired directly to Y; IC disconnected)"
                  : "74HC00 GATE 1 NAND TRUTH TABLE");
    for (unsigned row = 0; row < rows; row++) {
        if (*stop) { errno = EINTR; return ioError("GPIO interrupted"); }
        bool a = loopback ? row != 0 : (row & 2) != 0;
        bool b = !loopback && (row & 1) != 0;
        bool expected = loopback ? a : nand_expected[row];
        if (!Hardware_setInputs(hardware, a, b)) return ioError("GPIO stimulus");
        if (!pauseFor(GPIO_SETTLE_NS, stop)) return ioError("GPIO settling");
        unsigned matching = 0, high_reads = 0;
        for (unsigned read = 0; read < GPIO_READS_PER_ROW; read++) {
            bool measured;
            if (!Hardware_readOutput(hardware, &measured)) return ioError("GPIO observation");
            if (measured) high_reads++;
            if (measured == expected) matching++;
            if (!pauseFor(ADC_INTERVAL_NS, stop)) return ioError("GPIO read interval");
        }
        bool passed = matching == GPIO_READS_PER_ROW;
        all_passed = all_passed && passed;
        printf("A=%d B=%d expected_Y=%d high_reads=%u low_reads=%u matching_reads=%u/%d %s\n",
               (int)a, (int)b, (int)expected, high_reads,
               GPIO_READS_PER_ROW - high_reads, matching, GPIO_READS_PER_ROW,
               passed ? "PASS" : "FAIL");
    }
    //restore idle between tests, as well as during final hardware cleanup.
    if (!Hardware_setInputs(hardware, false, false)) return ioError("GPIO idle reset");
    printf("RESULT %s: %s\n", all_passed ? "PASS" : "FAIL",
           loopback ? "GPIO loopback" : "74HC00 gate 1");
    return all_passed ? DIAGNOSTIC_PASS : DIAGNOSTIC_FAIL;
}
int Diagnostics_run(Hardware *hardware, const Test_config *config,
                    const volatile sig_atomic_t *stop)
{
    if (config->kind == TEST_ADC || config->kind == TEST_DIVIDER) {
        return runAnalog(hardware, config, stop);
    }
    if (config->kind == TEST_LOOPBACK || config->kind == TEST_LOGIC) {
        return runDigital(hardware, config->kind == TEST_LOOPBACK, stop);
    }
    int analog = runAnalog(hardware, config, stop);
    if (analog == DIAGNOSTIC_ERROR) return analog;
    int digital = runDigital(hardware, false, stop);
    if (digital == DIAGNOSTIC_ERROR) return digital;
    bool passed = analog == DIAGNOSTIC_PASS && digital == DIAGNOSTIC_PASS;
    printf("OVERALL %s\n", passed ? "PASS" : "FAIL");
    return passed ? DIAGNOSTIC_PASS : DIAGNOSTIC_FAIL;
}
