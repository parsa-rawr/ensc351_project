//linux mcp3208 spi/gpio access and an independent virtual-time circuit model.
#ifndef HARDWARE_H
#define HARDWARE_H
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
typedef struct Hardware Hardware;
typedef enum { FAULT_NONE, FAULT_DIVIDER_LOW, FAULT_RC_FAST, FAULT_RC_SLOW,
    FAULT_RC_LOW, FAULT_RC_HIGH, FAULT_ADC_IO, FAULT_GPIO_IO } Hardware_fault;
typedef struct {
    bool simulate, need_loopback;
    Hardware_fault fault;
    const char *spi_device, *gpio_drive, *gpio_sense;
    unsigned rc_channel;
    double reference_volts;
} Hardware_config;
bool Hardware_open(Hardware **out, const Hardware_config *config,
                   bool need_adc, bool need_gpio);
bool Hardware_readAdc(Hardware *hardware, unsigned channel, uint16_t *raw);
bool Hardware_setDrive(Hardware *hardware, bool high);
bool Hardware_readOutput(Hardware *hardware, bool *high);
//real monotonic time on hardware; deterministic virtual time in simulation.
bool Hardware_now(Hardware *hardware, double *seconds);
bool Hardware_wait(Hardware *hardware, double seconds,
                   const volatile sig_atomic_t *stop);
//reset drive low, release requested pins, close spi, and free handle.
bool Hardware_close(Hardware *hardware);
#endif
