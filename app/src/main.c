//main application: command-line configuration, signals and resource lifecycle.
#include "diagnostics.h"
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stopped;
typedef struct { Hardware_config hardware; Test_config test; } Options;
static const char usage[] =
    "Usage: board_diagnostics [--simulate] [--test divider|logic|adc|loopback|all]\n"
    "  --spi PATH         SPI device (default /dev/spidev0.0)\n"
    "  --channel N        MCP3208 input 0..7 (default 3)\n"
    "  --rtop OHMS        Divider resistor from supply to junction (default 10000)\n"
    "  --rbottom OHMS     Divider resistor from junction to GND (default 10000)\n"
    "  --supply VOLTS     Actual divider supply (default 3.3)\n"
    "  --vref VOLTS       Actual ADC reference (default 3.3)\n"
    "  --tolerance VOLTS  Absolute allowed error for every sample (default 0.10)\n"
    "  --samples N        ADC readings per check, 1..10000 (default 32)\n"
    "  --a NAME --b NAME --y NAME   GPIO line names (GPIO22, GPIO23, GPIO24)\n"
    "  --fault divider-low|logic-stuck-low|adc-error|gpio-error (simulation only)\n"
    "  --help             Show this message\n"
    "Exit: 0=pass/measurement completed, 1=circuit fail, 2=configuration/I/O error,\n"
    "      130=interrupted. Simulation always prints a SIMULATION banner.\n";

static void onSignal(int signal_number) { (void)signal_number; stopped = 1; }
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
    static const char *const names[] = {"all", "adc", "loopback", "divider", "logic"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(value, names[i])) { *kind = (Test_kind)i; return true; }
    }
    return false;
}
static bool selectFault(const char *value, Hardware_fault *fault)
{
    static const char *const names[] = {
        "none", "divider-low", "logic-stuck-low", "adc-error", "gpio-error"
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(value, names[i])) { *fault = (Hardware_fault)i; return true; }
    }
    return false;
}
static bool parsePair(Options *options, const char *name, const char *value)
{
    if (!strcmp(name, "--test")) return selectTest(value, &options->test.kind);
    if (!strcmp(name, "--fault")) return selectFault(value, &options->hardware.fault);
    if (!strcmp(name, "--channel")) return parseUnsigned(value, 0, 7, &options->hardware.adc_channel);
    if (!strcmp(name, "--samples")) return parseUnsigned(value, 1, 10000, &options->test.samples);
    if (!strcmp(name, "--rtop")) return parseNumber(value, 1, 1e9, &options->test.top_ohms);
    if (!strcmp(name, "--rbottom")) return parseNumber(value, 1, 1e9, &options->test.bottom_ohms);
    if (!strcmp(name, "--supply")) return parseNumber(value, 0.01, 3.6, &options->test.supply_volts);
    if (!strcmp(name, "--vref")) return parseNumber(value, 0.01, 3.6, &options->test.reference_volts);
    if (!strcmp(name, "--tolerance")) return parseNumber(value, 0, 3.6, &options->test.tolerance_volts);
    if (!*value) return false;
    if (!strcmp(name, "--spi")) options->hardware.spi_device = value;
    else if (!strcmp(name, "--a")) options->hardware.gpio_a = value;
    else if (!strcmp(name, "--b")) options->hardware.gpio_b = value;
    else if (!strcmp(name, "--y")) options->hardware.gpio_y = value;
    else return false;
    return true;
}
static bool parseOptions(int argc, char **argv, Options *options)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--simulate")) { options->hardware.simulate = true; continue; }
        if (i + 1 >= argc || !parsePair(options, argv[i], argv[i + 1])) {
            fprintf(stderr, "Invalid option/value near '%s'. Use --help.\n", argv[i]);
            return false;
        }
        i++;
    }
    if (!options->hardware.simulate && options->hardware.fault != FAULT_NONE) {
        fprintf(stderr, "--fault requires --simulate.\n");
        return false;
    }
    if (!strcmp(options->hardware.gpio_a, options->hardware.gpio_b)
        || !strcmp(options->hardware.gpio_a, options->hardware.gpio_y)
        || !strcmp(options->hardware.gpio_b, options->hardware.gpio_y)) {
        fprintf(stderr, "GPIO A, B and Y must have different line names.\n");
        return false;
    }
    double expected = options->test.supply_volts * options->test.bottom_ohms
                    / (options->test.top_ohms + options->test.bottom_ohms);
    if (expected > options->test.reference_volts
        || options->test.tolerance_volts > options->test.reference_volts) {
        fprintf(stderr, "Divider expectation/tolerance must fit the ADC reference range.\n");
        return false;
    }
    options->hardware.simulate_loopback = options->test.kind == TEST_LOOPBACK;
    return true;
}
int main(int argc, char **argv)
{
    Options options = {
        .hardware = { .spi_device = "/dev/spidev0.0", .adc_channel = 3,
                      .gpio_a = "GPIO22", .gpio_b = "GPIO23", .gpio_y = "GPIO24" },
        .test = { .kind = TEST_DIVIDER, .top_ohms = 10000, .bottom_ohms = 10000,
                  .supply_volts = 3.3, .reference_volts = 3.3,
                  .tolerance_volts = 0.10, .samples = 32 }
    };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) { fputs(usage, stdout); return 0; }
    }
    if (!parseOptions(argc, argv, &options)) return DIAGNOSTIC_ERROR;
    struct sigaction action = { .sa_handler = onSignal };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
        perror("Signal setup"); return DIAGNOSTIC_ERROR;
    }
    puts(options.hardware.simulate
        ? "SIMULATION: synthetic circuits; results are not hardware measurements."
        : "HARDWARE: real SPI/GPIO measurements.");
    bool adc = options.test.kind == TEST_ALL || options.test.kind == TEST_ADC
            || options.test.kind == TEST_DIVIDER;
    bool gpio = options.test.kind == TEST_ALL || options.test.kind == TEST_LOOPBACK
             || options.test.kind == TEST_LOGIC;
    if (adc) printf("ADC device=%s channel=%u reference=%.4fV\n",
                    options.hardware.spi_device, options.hardware.adc_channel,
                    options.test.reference_volts);
    if (gpio) printf("GPIO A=%s B=%s Y=%s (74HC00 gate 1 for logic test)\n",
                     options.hardware.gpio_a, options.hardware.gpio_b, options.hardware.gpio_y);
    Hardware *hardware;
    if (!Hardware_open(&hardware, &options.hardware, adc, gpio)) {
        perror("Hardware setup (SPI device/GPIO line lookup or request)");
        return stopped ? 130 : DIAGNOSTIC_ERROR;
    }
    int result = Diagnostics_run(hardware, &options.test, &stopped);
    if (!Hardware_close(hardware)) { perror("Hardware cleanup"); result = DIAGNOSTIC_ERROR; }
    return stopped ? 130 : result;
}
