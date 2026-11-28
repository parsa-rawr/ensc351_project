#include "measurement.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

enum { BOARD_PORT = 35102, MAX_TOKEN = 128, MAX_PACKET = 1024, MAX_TESTS = 4096 };
typedef enum { RUNNING, RESULT_PENDING, COMPLETE, ABORTED, FAILED } Test_state;
typedef enum {
    SIM_NORMAL, SIM_DIVIDER_WRONG, SIM_RC_FAST, SIM_PWM_WRONG, SIM_NO_SIGNAL, SIM_ADC_ERROR
} Sim_case;

typedef struct {
    Hardware_config hardware;
    Measurement_expectations expected;
    unsigned channels[3];
    Sim_case scenario;
} Options;

typedef struct {
    struct sockaddr_in owner;
    char id[MAX_TOKEN + 1];
    Measurement_kind kind;
    Test_state state;
    char *result;
    const char *error;
    double retry_at;
} Test_record;

typedef struct {
    int socket_fd;
    Test_record records[MAX_TESTS];
    size_t count;
    Test_record *active;
    Hardware *hardware;
    Measurement measurement;
    double sample_at, sim_time;
    bool sim_high;
} Station;

static volatile sig_atomic_t stopped;

static void onSignal(int signo)
{
    (void)signo;
    stopped = 1;
}

static double monotonic(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        perror("CLOCK_MONOTONIC");
        exit(EXIT_FAILURE);
    }
    return now.tv_sec + now.tv_nsec / 1e9;
}

static bool parseNumber(const char *value, double low, double high, double *result)
{
    char *end;
    errno = 0;
    double number = strtod(value, &end);
    if (errno || end == value || *end || !isfinite(number) || number < low || number > high)
        return false;
    *result = number;
    return true;
}

static bool parsePair(Options *o, const char *key, const char *value)
{
    if (!strcmp(key, "--spi")) {
        o->hardware.spi_device = value;
        return *value != '\0';
    }
    if (!strcmp(key, "--scenario")) {
        const char *const names[] = {
            "normal", "divider-wrong", "rc-fast", "pwm-wrong", "no-signal", "adc-error"
        };
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (!strcmp(value, names[i])) {
                o->scenario = (Sim_case)i;
                return true;
            }
        }
        return false;
    }
    const char *const channel_options[] = { "--divider-channel", "--rc-channel", "--pwm-channel" };
    for (unsigned i = 0; i < 3; i++) {
        if (!strcmp(key, channel_options[i])) {
            if (value[0] < '0' || value[0] > '7' || value[1] != '\0') return false;
            o->channels[i] = (unsigned)(value[0] - '0');
            return true;
        }
    }
    if (!strcmp(key, "--vref")) return parseNumber(value, 1.0, 3.6, &o->hardware.reference_volts);
    if (!strcmp(key, "--supply")) return parseNumber(value, 1.0, 3.6, &o->expected.supply_v);
    if (!strcmp(key, "--divider-v")) return parseNumber(value, 0.20, 3.6, &o->expected.divider_v);
    if (!strcmp(key, "--rc-tau")) return parseNumber(value, 0.10, 5, &o->expected.rc_tau_s);
    if (!strcmp(key, "--pwm-hz")) return parseNumber(value, 1, 100, &o->expected.pwm_hz);
    if (!strcmp(key, "--pwm-threshold"))
        return parseNumber(value, 0.10, 3.50, &o->expected.pwm_threshold_v);
    return false;
}

static bool parseOptions(Options *o, int argc, char **argv)
{
    *o = (Options){
        .hardware = { .spi_device = "/dev/spidev0.0", .reference_volts = 3.3 },
        .expected = { .divider_v = 1.65, .rc_tau_s = 0.5, .supply_v = 3.3,
                      .pwm_hz = 20, .pwm_threshold_v = 1.65 },
        .channels = { 0, 2, 1 }
    };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--simulate")) {
            o->hardware.simulate = true;
        } else if (i + 1 < argc && parsePair(o, argv[i], argv[i + 1])) {
            i++;
        } else {
            return false;
        }
    }
    o->hardware.rc_channel = o->channels[MEASURE_RC];
    return (o->hardware.simulate || o->scenario == SIM_NORMAL)
        && o->expected.supply_v <= o->hardware.reference_volts + 0.10
        && o->expected.divider_v <= o->hardware.reference_volts
        && o->expected.pwm_threshold_v + 0.05 < o->hardware.reference_volts
        && o->channels[0] != o->channels[1] && o->channels[0] != o->channels[2]
        && o->channels[1] != o->channels[2];
}

static void reply(int fd, const struct sockaddr_in *to, const char *verb,
                  const char *id, const char *value)
{
    char packet[MAX_PACKET];
    int size = snprintf(packet, sizeof(packet), "%s,%s,%s", verb, id, value);
    if (size < 0 || (size_t)size >= sizeof(packet)) return;
    if (sendto(fd, packet, (size_t)size, 0, (const struct sockaddr *)to, sizeof(*to)) < 0
        && errno != EAGAIN && errno != EINTR) perror("UDP send");
}

static bool samePeer(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static Test_record *findTest(Station *s, const char *id)
{
    for (size_t i = 0; i < s->count; i++) {
        if (!strcmp(s->records[i].id, id)) return &s->records[i];
    }
    return NULL;
}

static Test_record *newTest(Station *s, const char *id, const struct sockaddr_in *peer)
{
    if (s->count == MAX_TESTS) {
        reply(s->socket_fd, peer, "ERROR", id, "TEST_HISTORY_FULL");
        return NULL;
    }
    Test_record *test = &s->records[s->count++];
    strcpy(test->id, id);
    test->owner = *peer;
    return test;
}

static void closeAcquisition(Station *s)
{
    if (s->hardware && !Hardware_close(s->hardware)) perror("ADC close");
    s->hardware = NULL;
    s->active = NULL;
}

static void failAcquisition(Station *s, const char *reason)
{
    Test_record *test = s->active;
    closeAcquisition(s);
    test->state = FAILED;
    test->error = reason;
    reply(s->socket_fd, &test->owner, "ERROR", test->id, reason);
}

static void sendResult(Station *s, Test_record *test, double now)
{
    reply(s->socket_fd, &test->owner, "RESULT", test->id, test->result);
    test->retry_at = now + 0.5;
}

static bool readSample(Station *s, const Options *o, double now, double *volts)
{
    Measurement_kind kind = s->active->kind;
    uint16_t raw;
    if (!o->hardware.simulate)
        return Hardware_readVoltage(s->hardware, o->channels[kind], &raw, volts);

    double elapsed = now - s->measurement.start;
    if (o->scenario == SIM_NO_SIGNAL || elapsed < 0.2) {
        *volts = 0;
        return true;
    }
    if (kind == MEASURE_PWM) {
        double frequency = o->scenario == SIM_PWM_WRONG ? 11 : 20;
        *volts = fmod((elapsed - 0.2) * frequency, 1.0) < 0.5 ? 2.8 : 0.4;
        return true;
    }
    if (kind == MEASURE_RC) {
        if (!Hardware_wait(s->hardware, elapsed - s->sim_time, &stopped)) return false;
        s->sim_time = elapsed;
        bool high = elapsed < 3.7;
        if (s->sim_high != high) {
            if (!Hardware_setDrive(s->hardware, high)) return false;
            s->sim_high = high;
        }
    }
    return Hardware_readVoltage(s->hardware, o->channels[kind], &raw, volts);
}

static void acquire(Station *s, const Options *o)
{
    double before = monotonic(), volts;
    if (!readSample(s, o, before, &volts)) {
        failAcquisition(s, "ADC_READ");
        return;
    }
    double after = monotonic();
    double timestamp = (before + after) * 0.5;
    const Measurement *m = &s->measurement;
    const double interval = Measurement_interval(s->active->kind);
    double gap_limit = s->active->kind == MEASURE_PWM
        ? fmin(0.010, 0.30 / o->expected.pwm_hz) : fmin(0.10, o->expected.rc_tau_s * 0.20);
    if (!o->hardware.simulate && s->active->kind != MEASURE_DIVIDER &&
        (after - before > gap_limit ||
         (m->previous_time != 0 && timestamp - m->previous_time > gap_limit))) {
        failAcquisition(s, "SAMPLE_GAP");
        return;
    }
    s->sample_at += interval;
    if (s->sample_at <= after) s->sample_at = after + interval;
    if (!Measurement_sample(&s->measurement, &o->expected, timestamp, volts)) return;

    char json[MAX_PACKET - MAX_TOKEN - 16];
    if (!Measurement_json(&s->measurement, &o->expected, o->hardware.simulate, json, sizeof(json))
        || !(s->active->result = strdup(json))) {
        failAcquisition(s, "RESULT_ENCODING");
        return;
    }
    Test_record *test = s->active;
    closeAcquisition(s);
    test->state = RESULT_PENDING;
    sendResult(s, test, after);
}

static void arm(Station *s, const Options *o, Test_record *test, Measurement_kind kind)
{
    Hardware_config hardware = o->hardware;
    if (o->scenario == SIM_ADC_ERROR) hardware.fault = FAULT_ADC_IO;
    if (kind == MEASURE_RC && o->scenario == SIM_RC_FAST) hardware.fault = FAULT_RC_FAST;
    if (kind == MEASURE_DIVIDER && o->scenario == SIM_DIVIDER_WRONG)
        hardware.fault = FAULT_DIVIDER_LOW;
    s->active = test;
    test->state = RUNNING;
    test->kind = kind;
    if (!Hardware_open(&s->hardware, &hardware, true, hardware.simulate && kind == MEASURE_RC)) {
        failAcquisition(s, "ADC_OPEN");
        return;
    }
    // Probe the SPI channel before reporting ready, including when the stimulus is low.
    double volts;
    uint16_t raw;
    if (!Hardware_readVoltage(s->hardware, o->channels[kind], &raw, &volts)) {
        failAcquisition(s, "ADC_READ");
        return;
    }
    double now = monotonic();
    Measurement_begin(&s->measurement, kind, now);
    s->sim_high = false;
    s->sim_time = 0;
    s->sample_at = now + Measurement_interval(kind);
    Measurement_sample(&s->measurement, &o->expected, now, hardware.simulate ? 0 : volts);
    reply(s->socket_fd, &test->owner, "ACK", test->id, "ARM");
}

static void handleArm(Station *s, const Options *o, Test_record *test,
                      const struct sockaddr_in *peer, const char *id, Measurement_kind kind)
{
    if (test) {
        if (test->state == ABORTED) {
            reply(s->socket_fd, peer, "ERROR", id, "ABORTED");
        } else if (test->kind != kind) {
            reply(s->socket_fd, peer, "ERROR", id, "TEST_TYPE_MISMATCH");
        } else if (test->state == FAILED) {
            reply(s->socket_fd, peer, "ERROR", id, test->error);
        } else {
            reply(s->socket_fd, peer, "ACK", id, "ARM");
            if (test->state == RESULT_PENDING) sendResult(s, test, monotonic());
        }
    } else if (s->active) {
        reply(s->socket_fd, peer, "ERROR", id, "BUSY");
    } else {
        test = newTest(s, id, peer);
        if (test) arm(s, o, test, kind);
    }
}

static bool validToken(const char *token)
{
    size_t len = strlen(token);
    if (!len || len > MAX_TOKEN) return false;
    for (const unsigned char *p = (const unsigned char *)token; *p; p++) {
        if (*p < '!' || *p > '~' || *p == ',') return false;
    }
    return true;
}

static void handlePacket(Station *s, const Options *o, char *packet,
                         const struct sockaddr_in *peer)
{
    char *id = strchr(packet, ',');
    if (!id) return;
    *id++ = '\0';
    char *value = strchr(id, ',');
    if (value) *value++ = '\0';
    if (!validToken(id)) return;
    if (!strcmp(packet, "PING")) {
        if (!value) reply(s->socket_fd, peer, "PONG", "2", id);
        return;
    }

    Test_record *test = findTest(s, id);
    if (test && !samePeer(peer, &test->owner)) {
        reply(s->socket_fd, peer, "ERROR", id, "CONTROLLER_MISMATCH");
        return;
    }
    if (!strcmp(packet, "ACK")) {
        if (value && !strcmp(value, "RESULT") && test && test->state == RESULT_PENDING) {
            free(test->result);
            test->result = NULL;
            test->state = COMPLETE;
        }
        return;
    }
    if (!strcmp(packet, "ABORT") && !value) {
        if (!test && s->count < MAX_TESTS) {
            test = newTest(s, id, peer);
            test->state = ABORTED;
        } else if (test && test == s->active) {
            closeAcquisition(s);
            test->state = ABORTED;
        }
        reply(s->socket_fd, peer, "ACK", id, "ABORT");
        return;
    }
    Measurement_kind kind;
    if (!strcmp(packet, "ARM") && value && Measurement_kindFromName(value, &kind)) {
        handleArm(s, o, test, peer, id, kind);
        return;
    }
    reply(s->socket_fd, peer, "ERROR", id, "BAD_COMMAND");
}

static void receivePacket(Station *s, const Options *o)
{
    char packet[MAX_PACKET];
    struct sockaddr_in peer;
    socklen_t peer_size = sizeof(peer);
    ssize_t size = recvfrom(s->socket_fd, packet, sizeof(packet), MSG_TRUNC,
                            (struct sockaddr *)&peer, &peer_size);
    if (size < 0) {
        if (errno != EINTR && errno != EAGAIN) perror("UDP receive");
        return;
    }
    if (peer_size != sizeof(peer) || !size || (size_t)size >= sizeof(packet) ||
        memchr(packet, '\0', (size_t)size)) return;
    while (size && (packet[size - 1] == '\n' || packet[size - 1] == '\r')) size--;
    packet[size] = '\0';
    handlePacket(s, o, packet, &peer);
}

static bool run(Station *s, const Options *o)
{
    while (!stopped) {
        double now = monotonic();
        if (s->active && now >= s->sample_at) acquire(s, o);
        double wake = now + 0.5;
        if (s->active) wake = fmin(wake, s->sample_at);
        for (size_t i = 0; i < s->count; i++) {
            Test_record *test = &s->records[i];
            if (test->state != RESULT_PENDING) continue;
            if (now >= test->retry_at) sendResult(s, test, now);
            wake = fmin(wake, test->retry_at);
        }
        double delay = fmax(0, wake - monotonic());
        struct timespec timeout = { .tv_sec = (time_t)delay, .tv_nsec = (long)(fmod(delay, 1.0) * 1e9) };
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s->socket_fd, &readable);
        int ready = pselect(s->socket_fd + 1, &readable, NULL, NULL, &timeout, NULL);
        if (ready > 0 && !stopped) receivePacket(s, o);
        if (ready < 0 && errno != EINTR) {
            perror("UDP wait");
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    const char *usage = "Usage: board2_measurement [--simulate [--scenario CASE]] [--spi PATH]\n"
        "  [--divider-channel N] [--rc-channel N] [--pwm-channel N]\n"
        "  [--vref V] [--supply V] [--divider-v V] [--rc-tau SECONDS]\n"
        "  [--pwm-hz HZ] [--pwm-threshold V]\n";
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        fputs(usage, stdout);
        return 0;
    }
    Options options;
    if (!parseOptions(&options, argc, argv)) {
        fputs(usage, stderr);
        return 2;
    }
    struct sigaction action = { .sa_handler = onSignal };
    if (sigemptyset(&action.sa_mask) || sigaction(SIGINT, &action, NULL) ||
        sigaction(SIGTERM, &action, NULL)) {
        perror("Signal setup");
        return 1;
    }
    static Station station;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("UDP socket");
        return 1;
    }
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(BOARD_PORT),
                                   .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (fd >= FD_SETSIZE || fcntl(fd, F_SETFL, O_NONBLOCK) < 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        if (fd >= FD_SETSIZE) errno = EMFILE;
        perror("UDP setup");
        close(fd);
        return 1;
    }
    station.socket_fd = fd;
    printf("Board 2 listening on UDP %d (%s)\n", BOARD_PORT,
           options.hardware.simulate ? "simulation" : options.hardware.spi_device);
    fflush(stdout);
    bool ok = run(&station, &options);
    closeAcquisition(&station);
    for (size_t i = 0; i < station.count; i++) free(station.records[i].result);
    close(fd);
    return ok ? 0 : 1;
}
