#include "controller.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stopped;

static void onSignal(int number)
{
    (void)number;
    stopped = 1;
}

static bool parseUnsigned(const char *text, unsigned minimum, unsigned maximum,
                          unsigned *value)
{
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || end == text || *end || parsed < minimum || parsed > maximum)
        return false;
    *value = (unsigned)parsed;
    return true;
}

static void usage(FILE *stream)
{
    fputs("Usage: circuit_controller [options]\n"
          "  --bind-ip IPv4                Board-facing bind address (default 0.0.0.0)\n"
          "  --board2-ip IPv4              Measurement board (default 127.0.0.1)\n"
          "  --board3-ip IPv4              Stimulus board (default 127.0.0.1)\n"
          "  --command-timeout-ms N        Command ACK timeout (default 500)\n"
          "  --measurement-timeout-ms N    Measurement deadline (default 15000)\n"
          "  --cleanup-timeout-ms N        ABORT confirmation deadline (default 1000)\n"
          "  --retries N                   Retries after first send (default 2)\n"
          "  --help                        Show this message\n"
          "Ports: dashboard 35100 (localhost), controller 35101, board 2 35102, board 3 35103.\n",
          stream);
}

int main(int argc, char **argv)
{
    Controller_config config = {
        .bind_ip = "0.0.0.0",
        .board2_ip = "127.0.0.1",
        .board3_ip = "127.0.0.1",
        .controller_port = 35101,
        .dashboard_port = 35100,
        .board2_port = 35102,
        .board3_port = 35103,
        .command_timeout_ms = 500,
        .measurement_timeout_ms = 15000,
        .cleanup_timeout_ms = 1000,
        .max_retries = 2
    };
    const char *invalid_name = NULL;
    const char *invalid_value = NULL;
    const char *name = NULL;
    const char *value = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", argv[i]);
            usage(stderr);
            return 2;
        }
        name = argv[i];
        value = argv[++i];
        if (!strcmp(name, "--bind-ip")) config.bind_ip = value;
        else if (!strcmp(name, "--board2-ip")) config.board2_ip = value;
        else if (!strcmp(name, "--board3-ip")) config.board3_ip = value;
        else if (!strcmp(name, "--command-timeout-ms")) {
            if (!parseUnsigned(value, 50, 60000, &config.command_timeout_ms)) goto invalid;
        } else if (!strcmp(name, "--measurement-timeout-ms")) {
            if (!parseUnsigned(value, 100, 300000, &config.measurement_timeout_ms)) goto invalid;
        } else if (!strcmp(name, "--cleanup-timeout-ms")) {
            if (!parseUnsigned(value, 50, 60000, &config.cleanup_timeout_ms)) goto invalid;
        } else if (!strcmp(name, "--retries")) {
            if (!parseUnsigned(value, 0, 10, &config.max_retries)) goto invalid;
        } else {
            fprintf(stderr, "Unknown option: %s\n", name);
            usage(stderr);
            return 2;
        }
    }
    {
        struct sigaction action = { .sa_handler = onSignal };
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
            perror("sigaction");
            return 2;
        }
    }
    return controller_run(&config, &stopped);

invalid:
    invalid_name = name;
    invalid_value = value;
    fprintf(stderr, "Invalid value for %s: %s\n", invalid_name, invalid_value);
    usage(stderr);
    return 2;
}
