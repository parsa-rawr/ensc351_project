//command-line configuration, signal handling, and resource lifecycle.
#include "diagnostics.h"
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//the signal handler only sets a flag so normal code can clean up
static volatile sig_atomic_t stopped;
typedef struct { Hardware_config hardware; Test_config test; } Options;
static const char usage[] =
    "Usage: board_diagnostics [--simulate] [--test divider|rc|adc|loopback|all]\n"
    "  --spi PATH          MCP3208 SPI device (default /dev/spidev0.0)\n"
    "  --channel N         Divider/raw ADC channel 0..7 (default 0)\n"
    "  --rc-channel N      RC ADC channel 0..7 (default 2)\n"
    "  --drive NAME        GPIO charging output (default GPIO22)\n"
    "  --sense NAME        GPIO loopback input (default GPIO24)\n"
    "  --rtop OHMS --rbottom OHMS  Divider resistors (default 10000 each)\n"
    "  --supply VOLTS      Measured supply/GPIO HIGH voltage (default 3.3)\n"
    "  --vref VOLTS        Measured ADC reference voltage (default 3.3)\n"
    "  --tolerance VOLTS   Divider/RC endpoint voltage tolerance (default 0.10)\n"
    "  --samples N         ADC readings per divider/raw check (default 32)\n"
    "  --resistance OHMS   Expected RC resistance (default 50000)\n"
    "  --capacitance-uf UF Expected capacitance in microfarads (default 10)\n"
    "  --tau-tolerance F   Fractional RC tolerance (default 0.25 = 25%)\n"
    "  --interval-ms MS    RC sampling interval (default 5)\n"
    "  --window SECONDS    Each RC phase duration (default 7*expected tau)\n"
    "  --csv PATH          Create a NEW RC sample CSV; refuses overwrite\n"
    "  --fault divider-low|rc-fast|rc-slow|rc-stuck-low|rc-stuck-high|adc-error|gpio-error\n"
    "                      Simulation only; fixed physical model is 50k/10uF\n"
    "  --help              Show this message\n"
    "Exit: 0=pass/measurement completed, 1=circuit fail, 2=configuration/I/O error,\n"
    "      130=interrupted. Simulation uses virtual time, not hardware timing.\n";
static void onSignal(int signal_number) { (void)signal_number; stopped = 1; }
//reject incomplete numbers, nonfinite values and out of range settings
static bool parseNumber(const char *value, double minimum, double maximum, double *out)
{
    char *end;
    errno = 0;
    double number = strtod(value, &end);
    if (errno || end == value || *end || !isfinite(number)
        || number < minimum || number > maximum) return false;
    *out = number;
    return true;
}
//validate integer options before opening any hardware
static bool parseUnsigned(const char *value, unsigned minimum, unsigned maximum, unsigned *out)
{
    char *end;
    errno = 0;
    unsigned long number = strtoul(value, &end, 10);
    if (errno || *value == '-' || end == value || *end
        || number < minimum || number > maximum) return false;
    *out = (unsigned)number;
    return true;
}
static bool selectTest(const char *value, Test_kind *kind)
{
    static const char *const names[] = {"all", "adc", "loopback", "divider", "rc"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(value, names[i])) { *kind = (Test_kind)i; return true; }
    return false;
}
static bool selectFault(const char *value, Hardware_fault *fault)
{
    static const char *const names[] = {"none", "divider-low", "rc-fast", "rc-slow",
        "rc-stuck-low", "rc-stuck-high", "adc-error", "gpio-error"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(value, names[i])) { *fault = (Hardware_fault)i; return true; }
    return false;
}
static bool parsePair(Options *o, const char *name, const char *value)
{
    if (!strcmp(name, "--test")) return selectTest(value, &o->test.kind);
    if (!strcmp(name, "--fault")) return selectFault(value, &o->hardware.fault);
    if (!strcmp(name, "--channel")) return parseUnsigned(value, 0, 7, &o->test.adc_channel);
    if (!strcmp(name, "--rc-channel")) return parseUnsigned(value, 0, 7, &o->test.rc_channel);
    if (!strcmp(name, "--samples")) return parseUnsigned(value, 1, 10000, &o->test.samples);
    if (!strcmp(name, "--rtop")) return parseNumber(value, 1, 1e9, &o->test.top_ohms);
    if (!strcmp(name, "--rbottom")) return parseNumber(value, 1, 1e9, &o->test.bottom_ohms);
    if (!strcmp(name, "--supply")) return parseNumber(value, 0.1, 3.6, &o->test.supply_volts);
    if (!strcmp(name, "--vref")) return parseNumber(value, 0.1, 3.6, &o->test.reference_volts);
    if (!strcmp(name, "--tolerance")) return parseNumber(value, 0, 3.6, &o->test.tolerance_volts);
    if (!strcmp(name, "--resistance")) return parseNumber(value, 1000, 1e6, &o->test.resistance_ohms);
    if (!strcmp(name, "--capacitance-uf")) return parseNumber(value, 0.1, 1000, &o->test.capacitance_uf);
    if (!strcmp(name, "--tau-tolerance")) return parseNumber(value, 0.001, 0.9, &o->test.tau_tolerance);
    if (!strcmp(name, "--window")) return parseNumber(value, 0.1, 120, &o->test.window_s);
    if (!strcmp(name, "--interval-ms")) {
        double ms;
        if (!parseNumber(value, 1, 1000, &ms)) return false;
        o->test.interval_s = ms / 1000;
        return true;
    }
    if (!*value) return false;
    if (!strcmp(name, "--spi")) o->hardware.spi_device = value;
    else if (!strcmp(name, "--drive")) o->hardware.gpio_drive = value;
    else if (!strcmp(name, "--sense")) o->hardware.gpio_sense = value;
    else if (!strcmp(name, "--csv")) o->test.csv_path = value;
    else return false;
    return true;
}
//check that the selected channels and sampling settings work together
static bool parseOptions(int argc, char **argv, Options *o)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--simulate")) { o->hardware.simulate = true; continue; }
        if (i + 1 >= argc || !parsePair(o, argv[i], argv[i + 1])) {
            fprintf(stderr, "Invalid option/value near '%s'. Use --help.\n", argv[i]);
            return false;
        }
        i++;
    }
    if (!o->hardware.simulate && o->hardware.fault != FAULT_NONE) {
        fprintf(stderr, "--fault requires --simulate.\n"); return false;
    }
    bool rc = o->test.kind == TEST_RC || o->test.kind == TEST_ALL;
    if (o->test.kind == TEST_LOOPBACK && !strcmp(o->hardware.gpio_drive, o->hardware.gpio_sense)) {
        fprintf(stderr, "Drive and sense must be different GPIO lines.\n"); return false;
    }
    double expected = o->test.supply_volts * o->test.bottom_ohms
                    / (o->test.top_ohms + o->test.bottom_ohms);
    if (expected > o->test.reference_volts || o->test.tolerance_volts > o->test.reference_volts) {
        fprintf(stderr, "Divider expectation/tolerance must fit ADC range.\n"); return false;
    }
    if (o->test.adc_channel == o->test.rc_channel) {
        fprintf(stderr, "Divider/raw and RC channels must differ; use --rc-channel.\n"); return false;
    }
    if (o->test.csv_path && !rc) {
        fprintf(stderr, "--csv requires --test rc or all.\n"); return false;
    }
    //bound acquisition time and require enough samples per time constant
    if (rc) {
        double tau = o->test.resistance_ohms * o->test.capacitance_uf * 1e-6;
        if (!o->test.window_s) o->test.window_s = 7 * tau;
        if (tau < 0.02 || tau > 10 || o->test.window_s < 5 * tau
            || o->test.window_s > 120 || o->test.interval_s > tau / 10
            || o->test.window_s / o->test.interval_s > 100000
            || o->test.supply_volts > o->test.reference_volts
            || o->test.tolerance_volts >= o->test.supply_volts * 0.25) {
            fprintf(stderr, "RC requires tau 0.02..10s, window 5*tau..120s, interval <= tau/10,\n"
                "<=100000 intervals, supply <= vref, voltage tolerance < supply/4.\n");
            return false;
        }
    }
    o->hardware.need_loopback = o->test.kind == TEST_LOOPBACK;
    o->hardware.rc_channel = o->test.rc_channel;
    o->hardware.reference_volts = o->test.reference_volts;
    return true;
}
int main(int argc, char **argv)
{
    Options o = {
        .hardware = { .spi_device = "/dev/spidev0.0", .gpio_drive = "GPIO22", .gpio_sense = "GPIO24" },
        .test = { .kind = TEST_DIVIDER, .top_ohms = 10000, .bottom_ohms = 10000,
            .supply_volts = 3.3, .reference_volts = 3.3, .tolerance_volts = 0.10,
            .samples = 32, .adc_channel = 0, .rc_channel = 2, .resistance_ohms = 50000,
            .capacitance_uf = 10, .tau_tolerance = 0.25, .interval_s = 0.005 }
    };
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--help")) { fputs(usage, stdout); return 0; }
    if (!parseOptions(argc, argv, &o)) return DIAGNOSTIC_ERROR;
    struct sigaction action = { .sa_handler = onSignal };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
        perror("Signal setup"); return DIAGNOSTIC_ERROR;
    }
    puts(o.hardware.simulate
        ? "SIMULATION: synthetic circuits and virtual time; not hardware measurements or timing."
        : "HARDWARE: real MCP3208 SPI/GPIO measurements.");
    //only request the interfaces needed by the selected test
    bool adc = o.test.kind != TEST_LOOPBACK;
    bool gpio = o.test.kind == TEST_ALL || o.test.kind == TEST_RC || o.test.kind == TEST_LOOPBACK;
    if (adc) printf("ADC device=%s divider_channel=%u rc_channel=%u reference=%.4fV\n",
        o.hardware.spi_device, o.test.adc_channel, o.test.rc_channel, o.test.reference_volts);
    if (gpio) printf("GPIO drive=%s%s%s\n", o.hardware.gpio_drive,
        o.hardware.need_loopback ? " sense=" : "", o.hardware.need_loopback ? o.hardware.gpio_sense : "");
    Hardware *hw;
    if (!Hardware_open(&hw, &o.hardware, adc, gpio)) {
        perror("Hardware setup"); return stopped ? 130 : DIAGNOSTIC_ERROR;
    }
    //always release resources after a pass, failure or interruption
    int result = Diagnostics_run(hw, &o.test, &stopped);
    if (!Hardware_close(hw)) { perror("Hardware cleanup"); result = DIAGNOSTIC_ERROR; }
    return stopped ? 130 : result;
}
