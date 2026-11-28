#ifndef CIRCUIT_CONTROLLER_H
#define CIRCUIT_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>
#include <signal.h>

typedef struct {
    const char *bind_ip;
    const char *board2_ip;
    const char *board3_ip;
    unsigned controller_port;
    unsigned dashboard_port;
    unsigned board2_port;
    unsigned board3_port;
    unsigned command_timeout_ms;
    unsigned measurement_timeout_ms;
    unsigned cleanup_timeout_ms;
    unsigned max_retries;
} Controller_config;

int controller_run(const Controller_config *config,
                   const volatile sig_atomic_t *stop);

#endif
