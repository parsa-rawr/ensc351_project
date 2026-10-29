//automated divider and two-input truth-table tests, plus adc/loopback
//bring-up checks. reports to stdout; no persistent logging or dashboard.
#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H
#include "hal/hardware.h"
#include <signal.h>
typedef enum { TEST_ALL, TEST_ADC, TEST_LOOPBACK, TEST_DIVIDER, TEST_LOGIC } Test_kind;
typedef struct {
    Test_kind kind;
    double top_ohms, bottom_ohms, supply_volts, reference_volts, tolerance_volts;
    unsigned samples;
} Test_config;
enum { DIAGNOSTIC_PASS = 0, DIAGNOSTIC_FAIL = 1, DIAGNOSTIC_ERROR = 2 };
//stop is set by main's signal handler. any i/o error is error, not pass/fail.
int Diagnostics_run(Hardware *hardware, const Test_config *config,
                    const volatile sig_atomic_t *stop);
#endif
