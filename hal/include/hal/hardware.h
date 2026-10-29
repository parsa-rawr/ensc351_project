//hal: linux spi/gpio access and a deterministic software model for vm tests.
//handles and gpio library versions stay private. call close even after a
//diagnostic fails; failed open calls clean up their own partial resources.
#ifndef HARDWARE_H
#define HARDWARE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct Hardware Hardware;
typedef enum { FAULT_NONE, FAULT_DIVIDER_LOW, FAULT_LOGIC_LOW,
               FAULT_ADC_IO, FAULT_GPIO_IO } Hardware_fault;
typedef struct {
    bool simulate;
    bool simulate_loopback;
    Hardware_fault fault;
    const char *spi_device;
    unsigned adc_channel;
    const char *gpio_a, *gpio_b, *gpio_y;
} Hardware_config;
//gpio a/b are outputs, y is an input. resources for unselected tests are
//not opened. returns false with errno set on an i/o/setup error.
bool Hardware_open(Hardware **out, const Hardware_config *config,
                   bool need_adc, bool need_gpio);
bool Hardware_readAdc(Hardware *hardware, uint16_t *raw);
bool Hardware_setInputs(Hardware *hardware, bool a, bool b);
bool Hardware_readOutput(Hardware *hardware, bool *y);
//attempts to drive a/b low, releases all requests, closes spi, frees handle.
//a false result means an output reset or descriptor close failed.
bool Hardware_close(Hardware *hardware);
#endif
