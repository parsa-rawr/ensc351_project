#include "hal/hardware.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

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
    bool has_adc, has_gpio, a, b;
    unsigned sample_index;
#ifdef ENABLE_HARDWARE
    int spi_fd;
    Gpio_pin pins[3];
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
static bool readSpi(Hardware *hardware, uint16_t *raw)
{
    unsigned channel = hardware->config.adc_channel;
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
        //reserve y first; do not drive a/b if the observation line is busy.
        if (ok && need_gpio) {
            ok = requestPin(&hardware->pins[2], config->gpio_y, false)
              && requestPin(&hardware->pins[0], config->gpio_a, true)
              && requestPin(&hardware->pins[1], config->gpio_b, true);
        }
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

bool Hardware_readAdc(Hardware *hardware, uint16_t *raw)
{
    if (!hardware->has_adc) { errno = EINVAL; return false; }
    if (hardware->config.simulate) {
        if (hardware->config.fault == FAULT_ADC_IO) { errno = EIO; return false; }
        //model an independent physical 10k/10k divider on 3.3v, not the
        //configured expected value. wrong resistor expectations can fail.
        int value = 2048 + (int)(hardware->sample_index++ % 5) - 2;
        if (hardware->config.fault == FAULT_DIVIDER_LOW) value -= 600;
        *raw = (uint16_t)value;
        return true;
    }
#ifdef ENABLE_HARDWARE
    return readSpi(hardware, raw);
#else
    errno = ENOSYS;
    return false;
#endif
}
bool Hardware_setInputs(Hardware *hardware, bool a, bool b)
{
    if (!hardware->has_gpio) { errno = EINVAL; return false; }
    if (hardware->config.simulate) {
        if (hardware->config.fault == FAULT_GPIO_IO) { errno = EIO; return false; }
        hardware->a = a;
        hardware->b = b;
        return true;
    }
#ifdef ENABLE_HARDWARE
    return writePin(&hardware->pins[0], a) && writePin(&hardware->pins[1], b);
#else
    errno = ENOSYS;
    return false;
#endif
}
bool Hardware_readOutput(Hardware *hardware, bool *y)
{
    if (!hardware->has_gpio) { errno = EINVAL; return false; }
    if (hardware->config.simulate) {
        if (hardware->config.fault == FAULT_GPIO_IO) { errno = EIO; return false; }
        *y = hardware->config.simulate_loopback ? hardware->a
                                               : !(hardware->a && hardware->b);
        if (hardware->config.fault == FAULT_LOGIC_LOW) *y = false;
        return true;
    }
#ifdef ENABLE_HARDWARE
    int value = readPin(&hardware->pins[2]);
    if (value < 0) return false;
    *y = value != 0;
    return true;
#else
    errno = ENOSYS;
    return false;
#endif
}
bool Hardware_close(Hardware *hardware)
{
    if (!hardware) return true;
    bool ok = true;
    int saved_error = 0;
#ifdef ENABLE_HARDWARE
    if (!hardware->config.simulate) {
        for (unsigned i = 0; i < 2; i++) {
            if (pinRequested(&hardware->pins[i]) && !writePin(&hardware->pins[i], false)) {
                ok = false;
                saved_error = errno;
            }
        }
        for (unsigned i = 0; i < 3; i++) releasePin(&hardware->pins[i]);
        if (hardware->spi_fd >= 0 && close(hardware->spi_fd) < 0) {
            ok = false;
            saved_error = errno;
        }
    }
#endif
    free(hardware);
    if (!ok) errno = saved_error;
    return ok;
}
