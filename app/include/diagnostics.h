#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H
#include "hal/hardware.h"
#include <signal.h>
typedef enum { TEST_ALL, TEST_ADC, TEST_LOOPBACK, TEST_DIVIDER, TEST_RC } Test_kind;
typedef struct {
    Test_kind kind;
    double top_ohms, bottom_ohms, supply_volts, reference_volts, tolerance_volts;
    unsigned samples, adc_channel, rc_channel;
    double resistance_ohms, capacitance_uf, tau_tolerance, interval_s, window_s;
    const char *csv_path;
} Test_config;
enum { DIAGNOSTIC_PASS = 0, DIAGNOSTIC_FAIL = 1, DIAGNOSTIC_ERROR = 2 };
int Diagnostics_run(Hardware *hardware, const Test_config *config,
                    const volatile sig_atomic_t *stop);
#endif
