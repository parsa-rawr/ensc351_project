#define _POSIX_C_SOURCE 200809L
#include "controller.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { BOARD2 = 0, BOARD3 = 1, BOARD_COUNT = 2, MAX_HISTORY = 64,
       PACKET_CAP = 8192, JSON_CAP = 4096, ID_CAP = 80 };
static const uint64_t HEARTBEAT_NS = UINT64_C(1000000000);
static const uint64_t OFFLINE_NS = UINT64_C(3000000000);

typedef enum { IDLE, PREPARING, ARMING, RUNNING, COMPLETED, FAILED, ABORTED } State;
typedef enum { WAIT_NONE, WAIT_PREPARE, WAIT_ARM, WAIT_START } Wait_kind;
typedef enum { FINAL_COMPLETED, FINAL_FAILED, FINAL_ABORTED } Final_state;

typedef struct {
    struct sockaddr_in endpoint;
    unsigned number;
    const char *name;
    bool online;
    uint64_t last_pong_ns;
    uint64_t nonce;
    uint64_t ping_sent_ns;
    bool have_latency;
    double latency_last_ms;
    double latency_avg_ms;
    double latency_min_ms;
    double latency_max_ms;
    uint64_t latency_count;
} Board;

typedef struct {
    char id[ID_CAP];
    char request_id[ID_CAP];
    char type[16];
    char fault[24];
    State state;
    char progress[32];
    char result[JSON_CAP];
    bool has_result;
    bool diagnostic_pass;
    char error[256];
    bool start_sent;
    Wait_kind waiting;
    unsigned attempts;
    uint64_t command_deadline_ns;
    uint64_t measurement_deadline_ns;
    Final_state final_state;
    bool cleanup_started;
    bool cleanup_acked[BOARD_COUNT];
    bool cleanup_unconfirmed[BOARD_COUNT];
    unsigned cleanup_attempts[BOARD_COUNT];
    uint64_t cleanup_deadline_ns[BOARD_COUNT];
    bool cleanup_failure;
} Test_record;

typedef struct {
    const Controller_config *config;
    int board_fd;
    int dashboard_fd;
    Board boards[BOARD_COUNT];
    Test_record history[MAX_HISTORY];
    size_t history_count;
    int current;
    uint64_t next_heartbeat_ns;
    uint64_t nonce_seed;
    uint64_t test_counter;
    bool shutting_down;
} Controller;

static uint64_t monotonicNs(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static const char *stateName(State state)
{
    switch (state) {
    case IDLE: return "IDLE";
    case PREPARING: return "PREPARING";
    case ARMING: return "ARMING";
    case RUNNING: return "RUNNING";
    case COMPLETED: return "COMPLETED";
    case FAILED: return "FAILED";
    case ABORTED: return "ABORTED";
    }
    return "FAILED";
}

static int openUdp(const char *ip, unsigned port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int enabled = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, ip, &address.sin_addr) != 1) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static bool setEndpoint(Board *board, const char *ip, unsigned port,
                        unsigned number, const char *name)
{
    memset(&board->endpoint, 0, sizeof(board->endpoint));
    board->endpoint.sin_family = AF_INET;
    board->endpoint.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &board->endpoint.sin_addr) != 1) return false;
    board->number = number;
    board->name = name;
    return true;
}

static bool sameEndpoint(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_family == AF_INET && a->sin_port == b->sin_port
        && a->sin_addr.s_addr == b->sin_addr.s_addr;
}

static bool sendTo(int fd, const struct sockaddr_in *to, const char *message)
{
    size_t length = strlen(message);
    ssize_t sent = sendto(fd, message, length, 0, (const struct sockaddr *)to, sizeof(*to));
    return sent == (ssize_t)length;
}

static bool sendBoard(Controller *c, Board *board, const char *message)
{
    return sendTo(c->board_fd, &board->endpoint, message);
}

static bool validToken(const char *value, size_t max_length)
{
    size_t length = strlen(value);
    if (!length || length >= max_length) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
              || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'))
            return false;
    }
    return true;
}

static bool parseUint64(const char *value, uint64_t *out)
{
    if (!*value) return false;
    uint64_t result = 0;
    for (const char *p = value; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        unsigned digit = (unsigned)(*p - '0');
        if (result > (UINT64_MAX - digit) / 10) return false;
        result = result * 10 + digit;
    }
    *out = result;
    return true;
}

/* A small strict JSON recognizer keeps RESULT payloads intact without a dependency. */
typedef struct { const char *p; const char *end; unsigned depth; } Json_parser;

static void jsonSpace(Json_parser *p)
{
    while (p->p < p->end && (*p->p == ' ' || *p->p == '\t' || *p->p == '\r' || *p->p == '\n'))
        p->p++;
}

static bool jsonString(Json_parser *p, const char **start, const char **finish)
{
    if (p->p >= p->end || *p->p != '"') return false;
    p->p++;
    const char *begin = p->p;
    while (p->p < p->end) {
        unsigned char ch = (unsigned char)*p->p++;
        if (ch == '"') {
            if (start) *start = begin;
            if (finish) *finish = p->p - 1;
            return true;
        }
        if (ch < 0x20) return false;
        if (ch == '\\') {
            if (p->p >= p->end) return false;
            char escaped = *p->p++;
            if (escaped == '"' || escaped == '\\' || escaped == '/' || escaped == 'b'
                || escaped == 'f' || escaped == 'n' || escaped == 'r' || escaped == 't') continue;
            if (escaped != 'u' || p->end - p->p < 4) return false;
            for (unsigned i = 0; i < 4; i++) {
                char h = *p->p++;
                if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F')))
                    return false;
            }
        }
    }
    return false;
}

static bool jsonValue(Json_parser *p);

static bool jsonArray(Json_parser *p)
{
    p->p++;
    jsonSpace(p);
    if (p->p < p->end && *p->p == ']') { p->p++; return true; }
    for (;;) {
        if (!jsonValue(p)) return false;
        jsonSpace(p);
        if (p->p >= p->end) return false;
        if (*p->p == ']') { p->p++; return true; }
        if (*p->p++ != ',') return false;
        jsonSpace(p);
    }
}

static bool jsonObject(Json_parser *p)
{
    p->p++;
    jsonSpace(p);
    if (p->p < p->end && *p->p == '}') { p->p++; return true; }
    for (;;) {
        if (!jsonString(p, NULL, NULL)) return false;
        jsonSpace(p);
        if (p->p >= p->end || *p->p++ != ':') return false;
        jsonSpace(p);
        if (!jsonValue(p)) return false;
        jsonSpace(p);
        if (p->p >= p->end) return false;
        if (*p->p == '}') { p->p++; return true; }
        if (*p->p++ != ',') return false;
        jsonSpace(p);
    }
}

static bool jsonNumber(Json_parser *p)
{
    const char *start = p->p;
    if (p->p < p->end && *p->p == '-') p->p++;
    if (p->p >= p->end) return false;
    if (*p->p == '0') p->p++;
    else {
        if (*p->p < '1' || *p->p > '9') return false;
        while (p->p < p->end && *p->p >= '0' && *p->p <= '9') p->p++;
    }
    if (p->p < p->end && *p->p == '.') {
        p->p++;
        if (p->p >= p->end || *p->p < '0' || *p->p > '9') return false;
        while (p->p < p->end && *p->p >= '0' && *p->p <= '9') p->p++;
    }
    if (p->p < p->end && (*p->p == 'e' || *p->p == 'E')) {
        p->p++;
        if (p->p < p->end && (*p->p == '+' || *p->p == '-')) p->p++;
        if (p->p >= p->end || *p->p < '0' || *p->p > '9') return false;
        while (p->p < p->end && *p->p >= '0' && *p->p <= '9') p->p++;
    }
    return p->p > start;
}

static bool jsonValue(Json_parser *p)
{
    if (++p->depth > 32) return false;
    jsonSpace(p);
    bool ok = false;
    if (p->p < p->end) {
        char ch = *p->p;
        if (ch == '"') ok = jsonString(p, NULL, NULL);
        else if (ch == '{') ok = jsonObject(p);
        else if (ch == '[') ok = jsonArray(p);
        else if (ch == '-' || (ch >= '0' && ch <= '9')) ok = jsonNumber(p);
        else if (p->end - p->p >= 4 && !memcmp(p->p, "true", 4)) { p->p += 4; ok = true; }
        else if (p->end - p->p >= 5 && !memcmp(p->p, "false", 5)) { p->p += 5; ok = true; }
        else if (p->end - p->p >= 4 && !memcmp(p->p, "null", 4)) { p->p += 4; ok = true; }
    }
    p->depth--;
    return ok;
}

static bool resultStatus(const char *json, size_t length, bool *passed)
{
    Json_parser p = { .p = json, .end = json + length };
    jsonSpace(&p);
    if (p.p >= p.end || *p.p++ != '{') return false;
    jsonSpace(&p);
    bool found = false;
    if (p.p < p.end && *p.p == '}') return false;
    for (;;) {
        const char *key_start, *key_end;
        if (!jsonString(&p, &key_start, &key_end)) return false;
        bool status_key = (size_t)(key_end - key_start) == 6
                       && memcmp(key_start, "status", 6) == 0;
        jsonSpace(&p);
        if (p.p >= p.end || *p.p++ != ':') return false;
        jsonSpace(&p);
        if (status_key) {
            const char *value_start, *value_end;
            if (found || !jsonString(&p, &value_start, &value_end)) return false;
            size_t n = (size_t)(value_end - value_start);
            if (n == 4 && !memcmp(value_start, "PASS", 4)) *passed = true;
            else if (n == 4 && !memcmp(value_start, "FAIL", 4)) *passed = false;
            else return false;
            found = true;
        } else if (!jsonValue(&p)) return false;
        jsonSpace(&p);
        if (p.p >= p.end) return false;
        if (*p.p == '}') { p.p++; break; }
        if (*p.p++ != ',') return false;
        jsonSpace(&p);
    }
    jsonSpace(&p);
    return found && p.p == p.end;
}

static bool appendf(char *buffer, size_t capacity, size_t *used, const char *format, ...)
{
    if (*used >= capacity) return false;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buffer + *used, capacity - *used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= capacity - *used) return false;
    *used += (size_t)n;
    return true;
}

static bool appendQuoted(char *buffer, size_t capacity, size_t *used, const char *value)
{
    if (!appendf(buffer, capacity, used, "\"")) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        char escaped[7];
        const char *part = escaped;
        size_t length = 1;
        switch (*p) {
        case '"': part = "\\\""; length = 2; break;
        case '\\': part = "\\\\"; length = 2; break;
        case '\b': part = "\\b"; length = 2; break;
        case '\f': part = "\\f"; length = 2; break;
        case '\n': part = "\\n"; length = 2; break;
        case '\r': part = "\\r"; length = 2; break;
        case '\t': part = "\\t"; length = 2; break;
        default:
            if (*p < 0x20) {
                int n = snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
                if (n != 6) return false;
                length = 6;
            } else escaped[0] = (char)*p;
        }
        if (*used + length >= capacity) return false;
        memcpy(buffer + *used, part, length);
        *used += length;
        buffer[*used] = '\0';
    }
    return appendf(buffer, capacity, used, "\"");
}

static void sendDashboard(Controller *c, const struct sockaddr_in *to,
                          const Test_record *record, const char *request_id,
                          const char *test_id, bool duplicate, const char *error)
{
    char json[PACKET_CAP];
    size_t used = 0;
    const char *state = record ? stateName(record->state) : "IDLE";
    const char *progress = record ? record->progress : "idle";
    const char *type = record && record->type[0] ? record->type : NULL;
    const char *id = record && record->id[0] ? record->id : NULL;
    const char *result = record && record->has_result ? record->result : NULL;
    const char *message = error ? error : (record && record->error[0] ? record->error : NULL);
    const char *board2_cleanup = !record || !record->cleanup_started ? "not_started"
        : record->cleanup_acked[BOARD2] ? "confirmed"
        : record->cleanup_unconfirmed[BOARD2] ? "unconfirmed" : "pending";
    const char *board3_cleanup = !record || !record->cleanup_started ? "not_started"
        : record->cleanup_acked[BOARD3] ? "confirmed"
        : record->cleanup_unconfirmed[BOARD3] ? "unconfirmed" : "pending";
    if (!appendf(json, sizeof(json), &used, "{\"state\":\"%s\",\"board2_connected\":%s,"
            "\"board3_connected\":%s,\"current_test_id\":", state,
            c->boards[BOARD2].online ? "true" : "false",
            c->boards[BOARD3].online ? "true" : "false")) return;
    if (id) { if (!appendQuoted(json, sizeof(json), &used, id)) return; }
    else if (!appendf(json, sizeof(json), &used, "null")) return;
    if (!appendf(json, sizeof(json), &used, ",\"test_type\":")) return;
    if (type) { if (!appendQuoted(json, sizeof(json), &used, type)) return; }
    else if (!appendf(json, sizeof(json), &used, "null")) return;
    if (!appendf(json, sizeof(json), &used, ",\"test_progress\":")) return;
    if (!appendQuoted(json, sizeof(json), &used, progress)) return;
    if (!appendf(json, sizeof(json), &used, ",\"measurement_result\":")) return;
    if (result) { if (!appendf(json, sizeof(json), &used, "%s", result)) return; }
    else if (!appendf(json, sizeof(json), &used, "null")) return;
    if (!appendf(json, sizeof(json), &used, ",\"error_message\":")) return;
    if (message) { if (!appendQuoted(json, sizeof(json), &used, message)) return; }
    else if (!appendf(json, sizeof(json), &used, "null")) return;
    if (!appendf(json, sizeof(json), &used, ",\"cleanup_acknowledgements\":{\"board2\":\"%s\",\"board3\":\"%s\"}",
                 board2_cleanup, board3_cleanup)) return;
    if (!appendf(json, sizeof(json), &used, ",\"network_latency_ms\":{\"board2\":{")) return;
    if (c->boards[BOARD2].have_latency) {
        if (!appendf(json, sizeof(json), &used,
            "\"last\":%.3f,\"average\":%.3f,\"minimum\":%.3f,\"maximum\":%.3f,\"samples\":%" PRIu64,
            c->boards[BOARD2].latency_last_ms, c->boards[BOARD2].latency_avg_ms,
            c->boards[BOARD2].latency_min_ms, c->boards[BOARD2].latency_max_ms,
            c->boards[BOARD2].latency_count)) return;
    } else if (!appendf(json, sizeof(json), &used, "\"last\":null,\"average\":null,\"minimum\":null,\"maximum\":null,\"samples\":0")) return;
    if (!appendf(json, sizeof(json), &used, "},\"board3\":{")) return;
    if (c->boards[BOARD3].have_latency) {
        if (!appendf(json, sizeof(json), &used,
            "\"last\":%.3f,\"average\":%.3f,\"minimum\":%.3f,\"maximum\":%.3f,\"samples\":%" PRIu64,
            c->boards[BOARD3].latency_last_ms, c->boards[BOARD3].latency_avg_ms,
            c->boards[BOARD3].latency_min_ms, c->boards[BOARD3].latency_max_ms,
            c->boards[BOARD3].latency_count)) return;
    } else if (!appendf(json, sizeof(json), &used, "\"last\":null,\"average\":null,\"minimum\":null,\"maximum\":null,\"samples\":0")) return;
    if (!appendf(json, sizeof(json), &used, "}}")) return;
    if (request_id) {
        if (!appendf(json, sizeof(json), &used, ",\"request_id\":")) return;
        if (!appendQuoted(json, sizeof(json), &used, request_id)) return;
    }
    if (test_id && (!appendf(json, sizeof(json), &used, ",\"test_id\":")
                    || !appendQuoted(json, sizeof(json), &used, test_id))) return;
    if (duplicate && !appendf(json, sizeof(json), &used, ",\"duplicate_request\":true")) return;
    if (!appendf(json, sizeof(json), &used, "}")) return;
    (void)sendTo(c->dashboard_fd, to, json);
}

static Test_record *findTest(Controller *c, const char *id)
{
    for (size_t i = 0; i < c->history_count; i++)
        if (!strcmp(c->history[i].id, id)) return &c->history[i];
    return NULL;
}

static Test_record *findRequest(Controller *c, const char *request_id)
{
    for (size_t i = 0; i < c->history_count; i++)
        if (!strcmp(c->history[i].request_id, request_id)) return &c->history[i];
    return NULL;
}

static Test_record *currentTest(Controller *c)
{
    return c->current >= 0 ? &c->history[c->current] : NULL;
}

static Test_record *visibleTest(Controller *c)
{
    if (c->current >= 0) return currentTest(c);
    if (c->history_count) return &c->history[c->history_count - 1];
    return NULL;
}

static bool validTypeFault(const char *type, const char *fault)
{
    bool type_ok = !strcmp(type, "DIVIDER") || !strcmp(type, "RC") || !strcmp(type, "PWM");
    bool fault_ok = !strcmp(fault, "NONE") || !strcmp(fault, "WRONG_VALUE")
                 || !strcmp(fault, "WRONG_FREQ") || !strcmp(fault, "NO_SIGNAL");
    if (!type_ok || !fault_ok) return false;
    if (!strcmp(fault, "NONE") || !strcmp(fault, "NO_SIGNAL")) return true;
    if (!strcmp(fault, "WRONG_VALUE")) return !strcmp(type, "DIVIDER") || !strcmp(type, "RC");
    return !strcmp(type, "PWM");
}

static void finalize(Controller *c, State state, const char *progress)
{
    Test_record *test = currentTest(c);
    if (!test) return;
    test->state = state;
    snprintf(test->progress, sizeof(test->progress), "%s", progress);
    test->waiting = WAIT_NONE;
    test->command_deadline_ns = 0;
    test->measurement_deadline_ns = 0;
    printf("TEST %s %s: %s", test->id, stateName(state), test->progress);
    if (test->error[0]) printf(" error=%s", test->error);
    putchar('\n');
    fflush(stdout);
    c->current = -1;
}

static bool sendCommand(Controller *c, Test_record *test, Wait_kind command, bool retry)
{
    char message[256];
    int n;
    Board *board;
    switch (command) {
    case WAIT_PREPARE:
        board = &c->boards[BOARD3];
        n = snprintf(message, sizeof(message), "PREPARE,%s,%s,%s", test->id, test->type, test->fault);
        break;
    case WAIT_ARM:
        board = &c->boards[BOARD2];
        n = snprintf(message, sizeof(message), "ARM,%s,%s", test->id, test->type);
        break;
    case WAIT_START:
        board = &c->boards[BOARD3];
        n = snprintf(message, sizeof(message), "START,%s,%s", test->id, test->type);
        test->start_sent = true;
        break;
    default:
        errno = EINVAL;
        return false;
    }
    if (n < 0 || (size_t)n >= sizeof(message) || !sendBoard(c, board, message)) return false;
    if (!retry) test->attempts = 1;
    else test->attempts++;
    test->waiting = command;
    test->command_deadline_ns = monotonicNs()
        + (uint64_t)c->config->command_timeout_ms * UINT64_C(1000000);
    return true;
}

static bool sendCleanupAbort(Controller *c, Test_record *test, unsigned board_index)
{
    char message[ID_CAP + 16];
    int n = snprintf(message, sizeof(message), "ABORT,%s", test->id);
    bool sent = n > 0 && (size_t)n < sizeof(message)
             && sendBoard(c, &c->boards[board_index], message);
    test->cleanup_attempts[board_index]++;
    test->cleanup_deadline_ns[board_index] = monotonicNs()
        + (uint64_t)c->config->cleanup_timeout_ms * UINT64_C(1000000);
    printf("ABORT %s board=%u attempt=%u %s\n", test->id,
           c->boards[board_index].number, test->cleanup_attempts[board_index],
           sent ? "sent" : "send_failed");
    fflush(stdout);
    return sent;
}

static void maybeFinishCleanup(Controller *c)
{
    Test_record *test = currentTest(c);
    if (!test || !test->cleanup_started) return;
    for (unsigned i = 0; i < BOARD_COUNT; i++)
        if (!test->cleanup_acked[i] && !test->cleanup_unconfirmed[i]) return;
    if (test->cleanup_unconfirmed[BOARD2] || test->cleanup_unconfirmed[BOARD3]) {
        char detail[128];
        if (test->cleanup_unconfirmed[BOARD2] && test->cleanup_unconfirmed[BOARD3])
            snprintf(detail, sizeof(detail), "ABORT unconfirmed by Board 2 and Board 3");
        else
            snprintf(detail, sizeof(detail), "ABORT unconfirmed by Board %u",
                test->cleanup_unconfirmed[BOARD2] ? 2u : 3u);
        if (test->error[0]) {
            size_t used = strlen(test->error);
            if (used > sizeof(test->error) - 1) used = sizeof(test->error) - 1;
            if (used < sizeof(test->error) - 1) test->error[used++] = ';';
            if (used < sizeof(test->error) - 1) test->error[used++] = ' ';
            size_t length = strlen(detail);
            size_t available = sizeof(test->error) - used - 1;
            if (length > available) length = available;
            memcpy(test->error + used, detail, length);
            test->error[used + length] = '\0';
        } else {
            snprintf(test->error, sizeof(test->error), "%s", detail);
        }
        test->cleanup_failure = true;
        finalize(c, FAILED, "cleanup_failed");
        return;
    }
    State final = test->final_state == FINAL_COMPLETED ? COMPLETED
                : test->final_state == FINAL_ABORTED ? ABORTED : FAILED;
    const char *progress = final == COMPLETED ? "complete"
                         : final == ABORTED ? "aborted" : "failed";
    finalize(c, final, progress);
}

static void beginFinish(Controller *c, Final_state desired, const char *error)
{
    Test_record *test = currentTest(c);
    if (!test) return;
    if (test->cleanup_started) return;
    if (error && *error) snprintf(test->error, sizeof(test->error), "%s", error);
    test->final_state = desired;
    test->cleanup_started = true;
    test->waiting = WAIT_NONE;
    test->command_deadline_ns = 0;
    test->measurement_deadline_ns = 0;
    snprintf(test->progress, sizeof(test->progress), "aborting_stimulus");
    /* Always attempt both sends; a failed Board 2 send must not skip Board 3. */
    (void)sendCleanupAbort(c, test, BOARD3);
    (void)sendCleanupAbort(c, test, BOARD2);
}

static void failTest(Controller *c, const char *reason)
{
    beginFinish(c, FINAL_FAILED, reason);
}

static bool sendResultAck(Controller *c, const char *id)
{
    char message[ID_CAP + 32];
    int n = snprintf(message, sizeof(message), "ACK,%s,RESULT", id);
    return n > 0 && (size_t)n < sizeof(message)
        && sendBoard(c, &c->boards[BOARD2], message);
}

static void startTest(Controller *c, const char *request_id,
                      const char *type, const char *fault,
                      const struct sockaddr_in *dashboard_peer)
{
    Test_record *same = findRequest(c, request_id);
    if (same) {
        sendDashboard(c, dashboard_peer, same, request_id, same->id, true, NULL);
        return;
    }
    if (c->current >= 0) {
        sendDashboard(c, dashboard_peer, currentTest(c), request_id, NULL, false,
                      "another test is active");
        return;
    }
    if (!validTypeFault(type, fault)) {
        sendDashboard(c, dashboard_peer, visibleTest(c), request_id, NULL, false,
                      "unsupported test type or fault combination");
        return;
    }
    if (!c->boards[BOARD2].online || !c->boards[BOARD3].online) {
        sendDashboard(c, dashboard_peer, visibleTest(c), request_id, NULL, false,
                      "both remote boards must be online");
        return;
    }
    if (c->history_count == MAX_HISTORY) {
        memmove(&c->history[0], &c->history[1], (MAX_HISTORY - 1) * sizeof(c->history[0]));
        c->history_count--;
    }
    size_t index = c->history_count++;
    Test_record *test = &c->history[index];
    memset(test, 0, sizeof(*test));
    uint64_t now = monotonicNs();
    int n = snprintf(test->id, sizeof(test->id), "%ld-%" PRIx64 "-%" PRIu64,
                     (long)getpid(), now, ++c->test_counter);
    if (n < 0 || (size_t)n >= sizeof(test->id)) {
        c->history_count--;
        sendDashboard(c, dashboard_peer, visibleTest(c), request_id, NULL, false,
                      "could not create test ID");
        return;
    }
    snprintf(test->request_id, sizeof(test->request_id), "%s", request_id);
    snprintf(test->type, sizeof(test->type), "%s", type);
    snprintf(test->fault, sizeof(test->fault), "%s", fault);
    test->state = PREPARING;
    snprintf(test->progress, sizeof(test->progress), "preparing_stimulus");
    c->current = (int)index;
    if (!sendCommand(c, test, WAIT_PREPARE, false)) {
        failTest(c, "failed to send PREPARE to Board 3");
    } else {
        printf("TEST %s PREPARING %s %s\n", test->id, test->type, test->fault);
        fflush(stdout);
    }
    sendDashboard(c, dashboard_peer, test, request_id, test->id, false, NULL);
}

static bool sendDashboardError(Controller *c, const struct sockaddr_in *peer,
                              const char *error)
{
    sendDashboard(c, peer, visibleTest(c), NULL, NULL, false, error);
    return true;
}

static void handleDashboardPacket(Controller *c, char *packet,
                                  const struct sockaddr_in *peer)
{
    if ((ntohl(peer->sin_addr.s_addr) & UINT32_C(0xff000000)) != UINT32_C(0x7f000000)) return;
    if (!strcmp(packet, "STATUS")) {
        sendDashboard(c, peer, visibleTest(c), NULL, NULL, false, NULL);
        return;
    }
    if (!strncmp(packet, "RUN,", 4)) {
        char *request_id = packet + 4;
        char *comma1 = strchr(request_id, ',');
        if (!comma1) { sendDashboardError(c, peer, "RUN requires request_id,test_type,fault"); return; }
        *comma1++ = '\0';
        char *comma2 = strchr(comma1, ',');
        if (!comma2 || strchr(comma2 + 1, ',')) {
            sendDashboardError(c, peer, "RUN requires exactly three fields after RUN"); return;
        }
        *comma2++ = '\0';
        if (!validToken(request_id, ID_CAP)) { sendDashboardError(c, peer, "invalid request_id"); return; }
        startTest(c, request_id, comma1, comma2, peer);
        return;
    }
    if (!strncmp(packet, "ABORT,", 6)) {
        char *id = packet + 6;
        if (!validToken(id, ID_CAP)) { sendDashboardError(c, peer, "invalid test_id"); return; }
        Test_record *test = findTest(c, id);
        if (!test) { sendDashboardError(c, peer, "unknown test_id"); return; }
        if (c->current >= 0 && currentTest(c) == test) {
            beginFinish(c, FINAL_ABORTED, "abort requested by dashboard");
        }
        sendDashboard(c, peer, test, NULL, id, false, NULL);
        return;
    }
    if (!strncmp(packet, "GET_RESULT,", 11)) {
        char *id = packet + 11;
        if (!validToken(id, ID_CAP)) { sendDashboardError(c, peer, "invalid test_id"); return; }
        Test_record *test = findTest(c, id);
        if (!test) sendDashboardError(c, peer, "unknown test_id");
        else sendDashboard(c, peer, test, NULL, id, false, NULL);
        return;
    }
    sendDashboardError(c, peer, "invalid dashboard request");
}

static void updateBoardLatency(Board *board, uint64_t now)
{
    if (!board->ping_sent_ns || now < board->ping_sent_ns) return;
    double latency = (double)(now - board->ping_sent_ns) / 1000000.0;
    board->latency_last_ms = latency;
    if (!board->have_latency) {
        board->latency_min_ms = latency;
        board->latency_max_ms = latency;
        board->latency_avg_ms = latency;
        board->have_latency = true;
    } else {
        if (latency < board->latency_min_ms) board->latency_min_ms = latency;
        if (latency > board->latency_max_ms) board->latency_max_ms = latency;
        board->latency_avg_ms += (latency - board->latency_avg_ms)
                               / (double)(board->latency_count + 1);
    }
    board->latency_count++;
}

static void handleAck(Controller *c, Board *source, const char *id, const char *command)
{
    Test_record *test = currentTest(c);
    if (!test || strcmp(test->id, id)) return;
    if (!strcmp(command, "RESULT")) return;
    if (!strcmp(command, "ABORT") && test->cleanup_started) {
        unsigned board_index = source == &c->boards[BOARD2] ? BOARD2
                             : source == &c->boards[BOARD3] ? BOARD3 : BOARD_COUNT;
        if (board_index < BOARD_COUNT && !test->cleanup_acked[board_index]
            && !test->cleanup_unconfirmed[board_index]) {
            test->cleanup_acked[board_index] = true;
            test->cleanup_deadline_ns[board_index] = 0;
            printf("ABORT %s board=%u confirmed\n", test->id, source->number);
            fflush(stdout);
            maybeFinishCleanup(c);
        }
        return;
    }
    if (test->cleanup_started) return;
    if (test->waiting == WAIT_PREPARE && !strcmp(command, "PREPARE")
        && source == &c->boards[BOARD3]) {
        test->waiting = WAIT_NONE;
        test->state = ARMING;
        snprintf(test->progress, sizeof(test->progress), "arming_measurement");
        if (!sendCommand(c, test, WAIT_ARM, false)) failTest(c, "failed to send ARM to Board 2");
        return;
    }
    if (test->waiting == WAIT_ARM && !strcmp(command, "ARM")
        && source == &c->boards[BOARD2]) {
        test->waiting = WAIT_NONE;
        test->state = RUNNING;
        snprintf(test->progress, sizeof(test->progress), "starting_stimulus");
        if (!sendCommand(c, test, WAIT_START, false)) failTest(c, "failed to send START to Board 3");
        return;
    }
    if (test->waiting == WAIT_START && !strcmp(command, "START")
        && source == &c->boards[BOARD3]) {
        test->waiting = WAIT_NONE;
        snprintf(test->progress, sizeof(test->progress), "measuring");
        test->measurement_deadline_ns = monotonicNs()
            + (uint64_t)c->config->measurement_timeout_ms * UINT64_C(1000000);
    }
}

static void handleBoardPacket(Controller *c, char *packet,
                              const struct sockaddr_in *peer)
{
    Board *source = NULL;
    for (unsigned i = 0; i < BOARD_COUNT; i++)
        if (sameEndpoint(peer, &c->boards[i].endpoint)) source = &c->boards[i];
    if (!source) return;

    if (!strncmp(packet, "PING,", 5)) {
        uint64_t nonce;
        if (!parseUint64(packet + 5, &nonce)) return;
        char response[96];
        (void)snprintf(response, sizeof(response), "PONG,1,%" PRIu64, nonce);
        (void)sendTo(c->board_fd, peer, response);
        return;
    }
    if (!strncmp(packet, "PONG,", 5)) {
        char *number_text = packet + 5;
        char *comma = strchr(number_text, ',');
        if (!comma || strchr(comma + 1, ',')) return;
        *comma++ = '\0';
        uint64_t number, nonce;
        if (!parseUint64(number_text, &number) || number != source->number
            || !parseUint64(comma, &nonce) || nonce != source->nonce) return;
        uint64_t now = monotonicNs();
        bool was_online = source->online;
        source->last_pong_ns = now;
        updateBoardLatency(source, now);
        source->online = true;
        if (!was_online) {
            printf("BOARD %u ONLINE\n", source->number);
            fflush(stdout);
        }
        return;
    }
    if (!strncmp(packet, "ACK,", 4)) {
        char *id = packet + 4;
        char *comma = strchr(id, ',');
        if (!comma || strchr(comma + 1, ',')) return;
        *comma++ = '\0';
        if (!validToken(id, ID_CAP) || (strcmp(comma, "PREPARE") && strcmp(comma, "ARM")
            && strcmp(comma, "START") && strcmp(comma, "ABORT") && strcmp(comma, "RESULT"))) return;
        handleAck(c, source, id, comma);
        return;
    }
    if (!strncmp(packet, "RESULT,", 7)) {
        char *id = packet + 7;
        char *comma = strchr(id, ',');
        if (!comma) return;
        *comma++ = '\0';
        if (source != &c->boards[BOARD2] || !validToken(id, ID_CAP)
            || strlen(comma) >= JSON_CAP) return;
        bool passed;
        if (!resultStatus(comma, strlen(comma), &passed)) return;
        Test_record *known = findTest(c, id);
        if (known && known->has_result) {
            (void)sendResultAck(c, id);
            return;
        }
        Test_record *test = currentTest(c);
        if (!test || strcmp(test->id, id) || !test->start_sent || test->state != RUNNING) return;
        memcpy(test->result, comma, strlen(comma) + 1);
        test->has_result = true;
        test->diagnostic_pass = passed;
        (void)sendResultAck(c, id);
        beginFinish(c, FINAL_COMPLETED, NULL);
        return;
    }
    if (!strncmp(packet, "ERROR,", 6)) {
        char *id = packet + 6;
        char *comma = strchr(id, ',');
        if (!comma) return;
        *comma++ = '\0';
        if (!validToken(id, ID_CAP) || !*comma || strlen(comma) >= 200) return;
        Test_record *test = currentTest(c);
        if (test && !strcmp(test->id, id)) {
            char error[256];
            snprintf(error, sizeof(error), "%s: %s", source->name, comma);
            failTest(c, error);
        }
        return;
    }
    if (!strncmp(packet, "ABORT,", 6)) {
        char *id = packet + 6;
        if (validToken(id, ID_CAP)) {
            Test_record *test = currentTest(c);
            if (test && !strcmp(test->id, id)) failTest(c, "remote board requested abort");
        }
    }
}

static void receivePackets(Controller *c, int fd, bool dashboard)
{
    for (;;) {
        char packet[PACKET_CAP];
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        ssize_t length = recvfrom(fd, packet, sizeof(packet) - 1, MSG_TRUNC,
                                  (struct sockaddr *)&peer, &peer_length);
        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            return;
        }
        if ((size_t)length >= sizeof(packet) || peer_length != sizeof(peer)) continue;
        packet[length] = '\0';
        if (memchr(packet, '\0', (size_t)length)) continue;
        if (dashboard) handleDashboardPacket(c, packet, &peer);
        else handleBoardPacket(c, packet, &peer);
    }
}

static void sendHeartbeats(Controller *c, uint64_t now)
{
    for (unsigned i = 0; i < BOARD_COUNT; i++) {
        Board *board = &c->boards[i];
        board->nonce = ++c->nonce_seed;
        board->ping_sent_ns = now;
        char ping[96];
        (void)snprintf(ping, sizeof(ping), "PING,%" PRIu64, board->nonce);
        (void)sendBoard(c, board, ping);
    }
    c->next_heartbeat_ns = now + HEARTBEAT_NS;
}

static void updateConnectivity(Controller *c, uint64_t now)
{
    for (unsigned i = 0; i < BOARD_COUNT; i++) {
        Board *board = &c->boards[i];
        if (board->online && now - board->last_pong_ns >= OFFLINE_NS) {
            board->online = false;
            printf("BOARD %u OFFLINE\n", board->number);
            fflush(stdout);
            if (c->current >= 0) {
                char reason[128];
                snprintf(reason, sizeof(reason), "Board %u heartbeat lost", board->number);
                failTest(c, reason);
            }
        }
    }
}

static void serviceTestTimers(Controller *c, uint64_t now)
{
    Test_record *test = currentTest(c);
    if (!test) return;
    if (test->cleanup_started) {
        for (unsigned i = 0; i < BOARD_COUNT; i++) {
            if (test->cleanup_acked[i] || test->cleanup_unconfirmed[i]
                || now < test->cleanup_deadline_ns[i]) continue;
            if (test->cleanup_attempts[i] <= c->config->max_retries) {
                (void)sendCleanupAbort(c, test, i);
            } else {
                test->cleanup_unconfirmed[i] = true;
                printf("ABORT %s board=%u unconfirmed\n", test->id,
                       c->boards[i].number);
                fflush(stdout);
            }
        }
        maybeFinishCleanup(c);
        return;
    }
    if (test->waiting != WAIT_NONE && now >= test->command_deadline_ns) {
        if (test->attempts <= c->config->max_retries) {
            Wait_kind command = test->waiting;
            if (!sendCommand(c, test, command, true)) {
                failTest(c, "failed to retry command");
            } else {
                printf("RETRY %s attempt=%u\n", test->id, test->attempts);
                fflush(stdout);
            }
        } else {
            char reason[128];
            snprintf(reason, sizeof(reason), "timeout waiting for %s acknowledgement",
                     test->waiting == WAIT_PREPARE ? "PREPARE" :
                     test->waiting == WAIT_ARM ? "ARM" : "START");
            failTest(c, reason);
        }
        return;
    }
    if (test->waiting == WAIT_NONE && test->measurement_deadline_ns
        && now >= test->measurement_deadline_ns) {
        failTest(c, "measurement timeout");
    }
}

static int pollTimeout(const Controller *c, uint64_t now)
{
    uint64_t next = c->next_heartbeat_ns;
    const Test_record *test = c->current >= 0 ? &c->history[c->current] : NULL;
    if (test && test->waiting != WAIT_NONE && test->command_deadline_ns < next)
        next = test->command_deadline_ns;
    if (test && test->cleanup_started) {
        for (unsigned i = 0; i < BOARD_COUNT; i++) {
            if (!test->cleanup_acked[i] && !test->cleanup_unconfirmed[i]
                && test->cleanup_deadline_ns[i] < next)
                next = test->cleanup_deadline_ns[i];
        }
    }
    if (test && test->waiting == WAIT_NONE && test->measurement_deadline_ns
        && test->measurement_deadline_ns < next) next = test->measurement_deadline_ns;
    if (next <= now) return 0;
    uint64_t milliseconds = (next - now + UINT64_C(999999)) / UINT64_C(1000000);
    if (milliseconds > 250) milliseconds = 250;
    return (int)milliseconds;
}

static bool setup(Controller *c)
{
    const Controller_config *config = c->config;
    c->board_fd = openUdp(config->bind_ip, config->controller_port);
    if (c->board_fd < 0) return false;
    c->dashboard_fd = openUdp("127.0.0.1", config->dashboard_port);
    if (c->dashboard_fd < 0) return false;
    if (!setEndpoint(&c->boards[BOARD2], config->board2_ip, config->board2_port, 2, "Board 2")
        || !setEndpoint(&c->boards[BOARD3], config->board3_ip, config->board3_port, 3, "Board 3")) {
        errno = EINVAL;
        return false;
    }
    uint64_t now = monotonicNs();
    c->nonce_seed = now ^ (uint64_t)(unsigned long)getpid();
    c->next_heartbeat_ns = now;
    c->current = -1;
    return true;
}

int controller_run(const Controller_config *config, const volatile sig_atomic_t *stop)
{
    Controller c;
    memset(&c, 0, sizeof(c));
    c.config = config;
    c.board_fd = -1;
    c.dashboard_fd = -1;
    c.current = -1;
    if (!setup(&c)) {
        perror("controller socket setup");
        if (c.dashboard_fd >= 0) close(c.dashboard_fd);
        if (c.board_fd >= 0) close(c.board_fd);
        return 1;
    }
    printf("circuit_controller listening controller=%u dashboard=127.0.0.1:%u board2=%s:%u board3=%s:%u\n",
           config->controller_port, config->dashboard_port, config->board2_ip,
           config->board2_port, config->board3_ip, config->board3_port);
    fflush(stdout);
    bool finished = false;
    while (!finished) {
        uint64_t now = monotonicNs();
        updateConnectivity(&c, now);
        if (now >= c.next_heartbeat_ns) sendHeartbeats(&c, now);
        serviceTestTimers(&c, now);
        if (*stop && !c.shutting_down) {
            c.shutting_down = true;
            if (c.current >= 0) beginFinish(&c, FINAL_ABORTED, "controller shutdown requested");
        }
        if (c.shutting_down && c.current < 0) break;

        struct pollfd descriptors[2] = {
            { .fd = c.board_fd, .events = POLLIN },
            { .fd = c.dashboard_fd, .events = POLLIN }
        };
        int result = poll(descriptors, 2, pollTimeout(&c, monotonicNs()));
        if (result < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            if (c.current >= 0) failTest(&c, "poll event loop failed");
            break;
        }
        if (descriptors[0].revents & POLLIN) receivePackets(&c, c.board_fd, false);
        if (descriptors[1].revents & POLLIN) receivePackets(&c, c.dashboard_fd, true);
        if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)
            || descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (c.current >= 0) failTest(&c, "UDP socket failure");
            break;
        }
    }
    close(c.dashboard_fd);
    close(c.board_fd);
    return 0;
}
