#include "hal/hardware.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef ENABLE_HARDWARE
#include <fcntl.h>
#include <gpiod.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>
#ifdef GPIOD_V2
#include <glob.h>
#endif
typedef struct {
    struct gpiod_chip *chip;
#ifdef GPIOD_V2
    struct gpiod_line_request *request;
    unsigned offset;
#else
    struct gpiod_line *line;
    bool requested;
#endif
} Gpio_pin;
#endif

struct Hardware {
    Hardware_config config;
    bool has_adc, has_gpio, drive;
    double sim_time, sim_voltage, sim_updated;
    unsigned sample_index;
#ifdef ENABLE_HARDWARE
    int spi_fd;
    Gpio_pin pins[2];
#endif
};

#ifdef ENABLE_HARDWARE
enum { SPI_BITS = 8, SPI_SPEED = 250000, SPI_BYTES = 3 };

#ifdef GPIOD_V2
static bool findPin(Gpio_pin *pin, const char *name)
{
    glob_t paths = {0};
    int found = glob("/dev/gpiochip*", 0, NULL, &paths);
    if (found != 0) {
        globfree(&paths);
        errno = found == GLOB_NOSPACE ? ENOMEM : ENOENT;
        return false;
    }
    int saved_error = ENOENT;
    for (size_t i = 0; i < paths.gl_pathc; i++) {
        struct gpiod_chip *chip = gpiod_chip_open(paths.gl_pathv[i]);
        if (!chip) { saved_error = errno; continue; }
        int offset = gpiod_chip_get_line_offset_from_name(chip, name);
        if (offset >= 0) {
            pin->chip = chip;
            pin->offset = (unsigned)offset;
            globfree(&paths);
            return true;
        }
        gpiod_chip_close(chip);
    }
    globfree(&paths);
    errno = saved_error;
    return false;
}
static bool requestPin(Gpio_pin *pin, const char *name, bool output)
{
    if (!findPin(pin, name)) return false;
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    struct gpiod_line_config *lines = gpiod_line_config_new();
    struct gpiod_request_config *request = gpiod_request_config_new();
    bool ok = settings && lines && request;
    if (ok) {
        ok = gpiod_line_settings_set_direction(settings, output
            ? GPIOD_LINE_DIRECTION_OUTPUT : GPIOD_LINE_DIRECTION_INPUT) == 0;
    }
    if (ok && output) {
        ok = gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE) == 0;
    }
    if (ok) {
        gpiod_request_config_set_consumer(request, "board_diagnostics");
        ok = gpiod_line_config_add_line_settings(lines, &pin->offset, 1, settings) == 0;
    }
    if (ok) pin->request = gpiod_chip_request_lines(pin->chip, request, lines);
    int saved_error = errno;
    if (request) gpiod_request_config_free(request);
    if (lines) gpiod_line_config_free(lines);
    if (settings) gpiod_line_settings_free(settings);
    errno = saved_error;
    return ok && pin->request;
}
static bool writePin(Gpio_pin *pin, bool value)
{
    return gpiod_line_request_set_value(pin->request, pin->offset,
        value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE) == 0;
}
static int readPin(Gpio_pin *pin)
{
    return gpiod_line_request_get_value(pin->request, pin->offset);
}
static bool pinRequested(const Gpio_pin *pin) { return pin->request != NULL; }
static void releasePin(Gpio_pin *pin)
{
    if (pin->request) gpiod_line_request_release(pin->request);
    if (pin->chip) gpiod_chip_close(pin->chip);
}
#else
static bool requestPin(Gpio_pin *pin, const char *name, bool output)
{
    pin->line = gpiod_line_find(name);
    if (!pin->line) return false;
    pin->chip = gpiod_line_get_chip(pin->line);
    int result = output
        ? gpiod_line_request_output(pin->line, "board_diagnostics", 0)
        : gpiod_line_request_input(pin->line, "board_diagnostics");
    pin->requested = result == 0;
    return pin->requested;
}
static bool writePin(Gpio_pin *pin, bool value)
{
    return gpiod_line_set_value(pin->line, value ? 1 : 0) == 0;
}
static int readPin(Gpio_pin *pin) { return gpiod_line_get_value(pin->line); }
static bool pinRequested(const Gpio_pin *pin) { return pin->requested; }
static void releasePin(Gpio_pin *pin)
{
    if (pin->requested) gpiod_line_release(pin->line);
    if (pin->chip) gpiod_chip_close(pin->chip);
}
#endif

//configure the existing linux spi device for the adc
static bool openSpi(Hardware *hardware)
{
    hardware->spi_fd = open(hardware->config.spi_device, O_RDWR | O_CLOEXEC);
    if (hardware->spi_fd < 0) return false;
    uint8_t mode = SPI_MODE_0, bits = SPI_BITS;
    uint32_t speed = SPI_SPEED;
    return ioctl(hardware->spi_fd, SPI_IOC_WR_MODE, &mode) == 0
        && ioctl(hardware->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) == 0
        && ioctl(hardware->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) == 0;
}
//send a single ended channel command and decode the twelve bit result
static bool readSpi(Hardware *hardware, unsigned channel, uint16_t *raw)
{
    uint8_t tx[SPI_BYTES] = {
        (uint8_t)(0x06 | ((channel & 0x04) >> 2)),
        (uint8_t)((channel & 0x03) << 6), 0
    };
    uint8_t rx[SPI_BYTES] = {0};
    struct spi_ioc_transfer transfer = {
        .tx_buf = (uintptr_t)tx, .rx_buf = (uintptr_t)rx, .len = sizeof(tx),
        .speed_hz = SPI_SPEED, .bits_per_word = SPI_BITS, .cs_change = 0
    };
    int count;
    do { count = ioctl(hardware->spi_fd, SPI_IOC_MESSAGE(1), &transfer); }
    while (count < 0 && errno == EINTR);
    if (count < 0) return false;
    if (count != SPI_BYTES) { errno = EIO; return false; }
    *raw = (uint16_t)(((rx[1] & 0x0F) << 8) | rx[2]);
    return true;
}
#endif

//evolve a fixed physical rc model independently of expected component values
static void updateModel(Hardware *hardware)
{
    double tau = hardware->config.fault == FAULT_RC_FAST ? 0.1
               : hardware->config.fault == FAULT_RC_SLOW ? 2.0 : 0.5;
    double target = hardware->drive ? 3.3 : 0.0;
    hardware->sim_voltage = target + (hardware->sim_voltage - target)
        * exp(-(hardware->sim_time - hardware->sim_updated) / tau);
    hardware->sim_updated = hardware->sim_time;
}
bool Hardware_open(Hardware **out, const Hardware_config *config,
                   bool need_adc, bool need_gpio)
{
    *out = NULL;
    Hardware *hardware = calloc(1, sizeof(*hardware));
    if (!hardware) return false;
    hardware->config = *config;
    hardware->has_adc = need_adc;
    hardware->has_gpio = need_gpio;
#ifdef ENABLE_HARDWARE
    hardware->spi_fd = -1;
    if (!config->simulate) {
        bool ok = !need_adc || openSpi(hardware);
        //reserve the sense input before driving an output in loopback mode
        if (ok && need_gpio && config->need_loopback)
            ok = requestPin(&hardware->pins[1], config->gpio_sense, false);
        if (ok && need_gpio)
            ok = requestPin(&hardware->pins[0], config->gpio_drive, true);
        if (!ok) {
            int saved_error = errno;
            (void)Hardware_close(hardware);
            errno = saved_error;
            return false;
        }
    }
#else
    if (!config->simulate) { free(hardware); errno = ENOSYS; return false; }
#endif
    *out = hardware;
    return true;
}
bool Hardware_readAdc(Hardware *hardware, unsigned channel, uint16_t *raw)
{
    if (!hardware->has_adc || channel > 7) { errno = EINVAL; return false; }
    if (hardware->config.simulate) {
        if (hardware->config.fault == FAULT_ADC_IO) { errno = EIO; return false; }
        double volts;
        if (channel == hardware->config.rc_channel) {
            updateModel(hardware);
            volts = hardware->sim_voltage;
            if (hardware->config.fault == FAULT_RC_LOW) volts = 0;
            if (hardware->config.fault == FAULT_RC_HIGH) volts = 3.3;
        } else {
            //fixed physical 10k/10k, 3.3v divider, independent of expectations.
            volts = 1.65 + ((int)(hardware->sample_index++ % 5) - 2) * 0.0005;
            if (hardware->config.fault == FAULT_DIVIDER_LOW) volts -= 0.48;
        }
        //quantize the simulated voltage with the same adc reference as hardware
        double code = volts / hardware->config.reference_volts * 4096.0;
        if (code < 0) code = 0;
        if (code > 4095) code = 4095;
        *raw = (uint16_t)code;
        return true;
    }
#ifdef ENABLE_HARDWARE
    return readSpi(hardware, channel, raw);
#else
    errno = ENOSYS;
    return false;
#endif
}
bool Hardware_setDrive(Hardware *hardware, bool high)
{
    if (!hardware->has_gpio) { errno = EINVAL; return false; }
    if (hardware->config.simulate) {
        if (hardware->config.fault == FAULT_GPIO_IO) { errno = EIO; return false; }
        updateModel(hardware);
        hardware->drive = high;
        return true;
    }
#ifdef ENABLE_HARDWARE
    return writePin(&hardware->pins[0], high);
#else
    errno = ENOSYS;
    return false;
#endif
}
bool Hardware_readOutput(Hardware *hardware, bool *high)
{
    if (!hardware->has_gpio || !hardware->config.need_loopback) {
        errno = EINVAL; return false;
    }
    if (hardware->config.simulate) {
        if (hardware->config.fault == FAULT_GPIO_IO) { errno = EIO; return false; }
        *high = hardware->drive;
        return true;
    }
#ifdef ENABLE_HARDWARE
    int value = readPin(&hardware->pins[1]);
    if (value < 0) return false;
    *high = value != 0;
    return true;
#else
    errno = ENOSYS;
    return false;
#endif
}
//use virtual time only in simulation and monotonic time on the board
bool Hardware_now(Hardware *hardware, double *seconds)
{
    if (hardware->config.simulate) { *seconds = hardware->sim_time; return true; }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return false;
    *seconds = now.tv_sec + now.tv_nsec * 1e-9;
    return true;
}
//simulation advances immediately while real hardware waits between samples
bool Hardware_wait(Hardware *hardware, double seconds,
                   const volatile sig_atomic_t *stop)
{
    if (*stop) { errno = EINTR; return false; }
    if (hardware->config.simulate) {
        hardware->sim_time += seconds;
        return true;
    }
    struct timespec delay = { .tv_sec = (time_t)seconds,
        .tv_nsec = (long)((seconds - floor(seconds)) * 1e9) };
    while (nanosleep(&delay, &delay) < 0) {
        if (errno != EINTR || *stop) return false;
    }
    if (*stop) { errno = EINTR; return false; }
    return true;
}
//attempt output reset before releasing pins and descriptors
bool Hardware_close(Hardware *hardware)
{
    if (!hardware) return true;
    bool ok = true;
    int saved_error = 0;
#ifdef ENABLE_HARDWARE
    if (!hardware->config.simulate) {
        if (pinRequested(&hardware->pins[0]) && !writePin(&hardware->pins[0], false)) {
            ok = false; saved_error = errno;
        }
        for (unsigned i = 0; i < 2; i++) releasePin(&hardware->pins[i]);
        if (hardware->spi_fd >= 0 && close(hardware->spi_fd) < 0) {
            ok = false; saved_error = errno;
        }
    }
#endif
    free(hardware);
    if (!ok) errno = saved_error;
    return ok;
}
