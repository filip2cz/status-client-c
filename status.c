/*
 * Linux client for BotoX/ServerStatus. It collects host metrics and sends
 * periodic updates to a ServerStatus master over TCP. Configuration can be
 * provided through a JSON file, command-line options, or the legacy
 * host/port interface with credentials read from login.txt.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define DEFAULT_CREDENTIALS "login.txt"
#define DEFAULT_INTERVAL 1.0
#define MAX_LINE 4096
#define CONNECT_TIMEOUT_MS 10000
#define IO_TIMEOUT_MS 30000
#define PROBE_TIMEOUT_MS 2000
#define DISK_UNIT (1024ULL * 1024ULL)
#define MAX_CONFIG_SIZE (64U * 1024U)

typedef struct {
    char username[256];
    char password[256];
} Credentials;

typedef struct {
    char host[1024];
    char port[6];
    Credentials credentials;
    double interval;
    char mountpoint[4096];
    bool quiet;
} ProgramConfig;

typedef struct {
    const char *text;
    size_t length;
    size_t position;
    unsigned depth;
} JsonParser;

typedef struct {
    char data[MAX_LINE];
    size_t begin;
    size_t end;
} LineReader;

typedef struct {
    uint64_t total;
    uint64_t idle;
    bool valid;
} CpuCounters;

typedef struct {
    uint64_t rx;
    uint64_t tx;
    bool valid;
} NetworkCounters;

typedef struct {
    uint64_t uptime;
    double load;
    uint64_t memory_total;
    uint64_t memory_used;
    uint64_t swap_total;
    uint64_t swap_used;
    uint64_t disk_total;
    uint64_t disk_used;
    double cpu;
    uint64_t network_rx;
    uint64_t network_tx;
} Metrics;

static volatile sig_atomic_t stopping = 0;
static bool quiet = false;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static void log_message(const char *format, ...)
{
    if (quiet)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage modes:\n"
            "  %s --config FILE\n"
            "  %s --host HOST --port PORT --username USER --password PASS [options]\n"
            "  %s [options] host port   (legacy mode; reads %s)\n"
            "Options:\n"
            "  -c, --config FILE       JSON configuration (all settings)\n"
            "  -H, --host HOST         Server hostname or address\n"
            "  -p, --port PORT         Server TCP port\n"
            "  -u, --username USER     ServerStatus username\n"
            "      --password PASS     ServerStatus password\n"
            "  -C, --credentials FILE  Legacy credentials file\n"
            "  -i, --interval SECONDS  Update interval, 0.1 to 3600 (default: 1)\n"
            "  -m, --mountpoint PATH   Filesystem to report (default: /)\n"
            "  -q, --quiet             Suppress status messages\n"
            "  -h, --help              Show this help\n",
            program, program, program, DEFAULT_CREDENTIALS);
}

static char *trim(char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        ++text;
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                          text[length - 1] == '\r' || text[length - 1] == '\n'))
        text[--length] = '\0';
    return text;
}

static int load_credentials(const char *path, Credentials *credentials)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "Cannot open credentials file '%s': %s\n", path, strerror(errno));
        return -1;
    }

    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    int result = -1;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        (void)length;
        char *value = trim(line);
        if (*value == '\0' || *value == '#')
            continue;

        char *separator = value;
        while (*separator && *separator != ' ' && *separator != '\t' && *separator != ':')
            ++separator;
        if (*separator == '\0')
            break;
        char delimiter = *separator;
        *separator++ = '\0';
        char *password = trim(separator);
        if (delimiter != ':' && *password == ':')
            password = trim(password + 1);
        if (*value == '\0' || *password == '\0' || strchr(password, '\n') ||
            strlen(value) >= sizeof(credentials->username) ||
            strlen(password) >= sizeof(credentials->password))
            break;

        strcpy(credentials->username, value);
        strcpy(credentials->password, password);
        result = 0;
        break;
    }

    free(line);
    fclose(file);
    if (result != 0)
        fprintf(stderr, "Invalid credentials in '%s'; expected: username password\n", path);
    return result;
}

static void json_space(JsonParser *parser)
{
    while (parser->position < parser->length &&
           (parser->text[parser->position] == ' ' || parser->text[parser->position] == '\t' ||
            parser->text[parser->position] == '\r' || parser->text[parser->position] == '\n'))
        ++parser->position;
}

static int json_hex4(JsonParser *parser, uint32_t *value)
{
    if (parser->length - parser->position < 4)
        return -1;
    uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        char c = parser->text[parser->position++];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
        else return -1;
        result = result * 16 + digit;
    }
    *value = result;
    return 0;
}

static int json_append_codepoint(char *output, size_t capacity, size_t *used, uint32_t cp)
{
    unsigned char bytes[4];
    size_t count;
    if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return -1;
    if (cp <= 0x7f) {
        bytes[0] = (unsigned char)cp; count = 1;
    } else if (cp <= 0x7ff) {
        bytes[0] = (unsigned char)(0xc0 | (cp >> 6));
        bytes[1] = (unsigned char)(0x80 | (cp & 0x3f)); count = 2;
    } else if (cp <= 0xffff) {
        bytes[0] = (unsigned char)(0xe0 | (cp >> 12));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | (cp & 0x3f)); count = 3;
    } else {
        bytes[0] = (unsigned char)(0xf0 | (cp >> 18));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        bytes[3] = (unsigned char)(0x80 | (cp & 0x3f)); count = 4;
    }
    if (*used + count >= capacity)
        return -1;
    memcpy(output + *used, bytes, count);
    *used += count;
    return 0;
}

static int json_string(JsonParser *parser, char *output, size_t capacity)
{
    if (capacity == 0 || parser->position >= parser->length ||
        parser->text[parser->position++] != '"')
        return -1;
    size_t used = 0;
    while (parser->position < parser->length) {
        unsigned char c = (unsigned char)parser->text[parser->position++];
        if (c == '"') {
            output[used] = '\0';
            return 0;
        }
        if (c < 0x20)
            return -1;
        if (c == '\\') {
            if (parser->position >= parser->length)
                return -1;
            c = (unsigned char)parser->text[parser->position++];
            switch (c) {
            case '"': case '\\': case '/':
                if (used + 1 >= capacity) return -1;
                output[used++] = (char)c;
                break;
            case 'b': if (used + 1 >= capacity) return -1; output[used++] = '\b'; break;
            case 'f': if (used + 1 >= capacity) return -1; output[used++] = '\f'; break;
            case 'n': if (used + 1 >= capacity) return -1; output[used++] = '\n'; break;
            case 'r': if (used + 1 >= capacity) return -1; output[used++] = '\r'; break;
            case 't': if (used + 1 >= capacity) return -1; output[used++] = '\t'; break;
            case 'u': {
                uint32_t cp;
                if (json_hex4(parser, &cp) < 0)
                    return -1;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (parser->length - parser->position < 6 ||
                        parser->text[parser->position] != '\\' ||
                        parser->text[parser->position + 1] != 'u')
                        return -1;
                    parser->position += 2;
                    uint32_t low;
                    if (json_hex4(parser, &low) < 0 || low < 0xdc00 || low > 0xdfff)
                        return -1;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                }
                if (json_append_codepoint(output, capacity, &used, cp) < 0)
                    return -1;
                break;
            }
            default: return -1;
            }
        } else {
            if (used + 1 >= capacity)
                return -1;
            output[used++] = (char)c;
        }
    }
    return -1;
}

static int json_skip_string(JsonParser *parser)
{
    if (parser->position >= parser->length || parser->text[parser->position++] != '"')
        return -1;
    while (parser->position < parser->length) {
        unsigned char c = (unsigned char)parser->text[parser->position++];
        if (c == '"') return 0;
        if (c < 0x20) return -1;
        if (c == '\\') {
            if (parser->position >= parser->length) return -1;
            c = (unsigned char)parser->text[parser->position++];
            if (c == 'u') {
                uint32_t ignored;
                if (json_hex4(parser, &ignored) < 0) return -1;
            } else if (c != '"' && c != '\\' && c != '/' && c != 'b' && c != 'f' &&
                       c != 'n' && c != 'r' && c != 't') return -1;
        }
    }
    return -1;
}

static int json_number(JsonParser *parser, double *number)
{
    size_t start = parser->position;
    if (parser->position < parser->length && parser->text[parser->position] == '-')
        ++parser->position;
    if (parser->position >= parser->length) return -1;
    if (parser->text[parser->position] == '0') {
        ++parser->position;
        if (parser->position < parser->length && parser->text[parser->position] >= '0' &&
            parser->text[parser->position] <= '9') return -1;
    } else {
        if (parser->text[parser->position] < '1' || parser->text[parser->position] > '9') return -1;
        while (parser->position < parser->length && parser->text[parser->position] >= '0' &&
               parser->text[parser->position] <= '9') ++parser->position;
    }
    if (parser->position < parser->length && parser->text[parser->position] == '.') {
        ++parser->position;
        size_t fraction_start = parser->position;
        while (parser->position < parser->length && parser->text[parser->position] >= '0' &&
               parser->text[parser->position] <= '9') ++parser->position;
        if (fraction_start == parser->position) return -1;
    }
    if (parser->position < parser->length &&
        (parser->text[parser->position] == 'e' || parser->text[parser->position] == 'E')) {
        ++parser->position;
        if (parser->position < parser->length &&
            (parser->text[parser->position] == '+' || parser->text[parser->position] == '-'))
            ++parser->position;
        size_t exponent_start = parser->position;
        while (parser->position < parser->length && parser->text[parser->position] >= '0' &&
               parser->text[parser->position] <= '9') ++parser->position;
        if (exponent_start == parser->position) return -1;
    }
    size_t length = parser->position - start;
    if (length >= 128) return -1;
    char buffer[128];
    memcpy(buffer, parser->text + start, length);
    buffer[length] = '\0';
    char *end = NULL;
    *number = strtod(buffer, &end);
    return end == buffer + length && isfinite(*number) ? 0 : -1;
}

static int json_skip_value(JsonParser *parser)
{
    json_space(parser);
    if (parser->position >= parser->length || parser->depth >= 32)
        return -1;
    char c = parser->text[parser->position];
    if (c == '"') return json_skip_string(parser);
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        ++parser->position;
        ++parser->depth;
        json_space(parser);
        if (parser->position < parser->length && parser->text[parser->position] == close) {
            ++parser->position;
            --parser->depth;
            return 0;
        }
        for (;;) {
            if (c == '{') {
                if (json_skip_string(parser) < 0) { --parser->depth; return -1; }
                json_space(parser);
                if (parser->position >= parser->length || parser->text[parser->position++] != ':') {
                    --parser->depth; return -1;
                }
            }
            if (json_skip_value(parser) < 0) { --parser->depth; return -1; }
            json_space(parser);
            if (parser->position >= parser->length) { --parser->depth; return -1; }
            char separator = parser->text[parser->position++];
            if (separator == close) break;
            if (separator != ',') { --parser->depth; return -1; }
            json_space(parser);
        }
        --parser->depth;
        return 0;
    }
    double ignored;
    if (c == '-' || (c >= '0' && c <= '9'))
        return json_number(parser, &ignored);
    const char *literals[] = {"true", "false", "null"};
    for (size_t i = 0; i < sizeof(literals) / sizeof(literals[0]); ++i) {
        size_t n = strlen(literals[i]);
        if (parser->length - parser->position >= n &&
            memcmp(parser->text + parser->position, literals[i], n) == 0) {
            parser->position += n;
            return 0;
        }
    }
    return -1;
}

static int json_boolean(JsonParser *parser, bool *value)
{
    if (parser->length - parser->position >= 4 &&
        memcmp(parser->text + parser->position, "true", 4) == 0) {
        parser->position += 4; *value = true; return 0;
    }
    if (parser->length - parser->position >= 5 &&
        memcmp(parser->text + parser->position, "false", 5) == 0) {
        parser->position += 5; *value = false; return 0;
    }
    return -1;
}

static int read_config_json(const char *path, ProgramConfig *config)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Cannot open JSON configuration '%s': %s\n", path, strerror(errno));
        return -1;
    }
    char *text = malloc(MAX_CONFIG_SIZE + 1);
    if (!text) { fclose(file); return -1; }
    size_t length = fread(text, 1, MAX_CONFIG_SIZE + 1, file);
    bool too_large = length > MAX_CONFIG_SIZE;
    bool read_error = ferror(file);
    fclose(file);
    if (too_large || read_error) {
        fprintf(stderr, "Cannot read JSON configuration '%s' (maximum size is %u bytes)\n",
                path, MAX_CONFIG_SIZE);
        free(text);
        return -1;
    }
    text[length] = '\0';
    JsonParser parser = {.text = text, .length = length};
    unsigned seen = 0;
    json_space(&parser);
    if (parser.position >= parser.length || text[parser.position++] != '{')
        goto invalid_json;
    json_space(&parser);
    if (parser.position < parser.length && text[parser.position] == '}')
        goto invalid_json;
    for (;;) {
        char key[128];
        if (json_string(&parser, key, sizeof(key)) < 0) goto invalid_json;
        json_space(&parser);
        if (parser.position >= parser.length || text[parser.position++] != ':') goto invalid_json;
        json_space(&parser);
        unsigned bit = 0;
        if (strcmp(key, "host") == 0) {
            bit = 1U << 0;
            if (json_string(&parser, config->host, sizeof(config->host)) < 0) goto invalid_json;
        } else if (strcmp(key, "port") == 0) {
            double value;
            bit = 1U << 1;
            if (json_number(&parser, &value) < 0 || value < 1 || value > 65535 ||
                value != (double)(long)value) goto invalid_json;
            snprintf(config->port, sizeof(config->port), "%ld", (long)value);
        } else if (strcmp(key, "username") == 0) {
            bit = 1U << 2;
            if (json_string(&parser, config->credentials.username,
                            sizeof(config->credentials.username)) < 0) goto invalid_json;
        } else if (strcmp(key, "password") == 0) {
            bit = 1U << 3;
            if (json_string(&parser, config->credentials.password,
                            sizeof(config->credentials.password)) < 0) goto invalid_json;
        } else if (strcmp(key, "interval") == 0) {
            bit = 1U << 4;
            if (json_number(&parser, &config->interval) < 0) goto invalid_json;
        } else if (strcmp(key, "mountpoint") == 0) {
            bit = 1U << 5;
            if (json_string(&parser, config->mountpoint, sizeof(config->mountpoint)) < 0) goto invalid_json;
        } else if (strcmp(key, "quiet") == 0) {
            bit = 1U << 6;
            if (json_boolean(&parser, &config->quiet) < 0) goto invalid_json;
        } else if (json_skip_value(&parser) < 0) {
            goto invalid_json;
        }
        if (bit && (seen & bit)) goto invalid_json;
        seen |= bit;
        json_space(&parser);
        if (parser.position >= parser.length) goto invalid_json;
        char separator = text[parser.position++];
        if (separator == '}') break;
        if (separator != ',') goto invalid_json;
        json_space(&parser);
    }
    json_space(&parser);
    free(text);
    if (parser.position != parser.length || seen != 0x7f) {
        fprintf(stderr, "JSON config must contain host, port, username, password, interval, mountpoint, and quiet\n");
        return -1;
    }
    return 0;

invalid_json:
    free(text);
    fprintf(stderr, "Invalid JSON configuration '%s' near byte %zu\n", path, parser.position);
    return -1;
}

static int wait_fd(int fd, short events, int timeout_ms)
{
    struct pollfd descriptor = {.fd = fd, .events = events};
    for (;;) {
        int result = poll(&descriptor, 1, timeout_ms);
        if (result > 0) {
            if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                errno = ECONNRESET;
                return -1;
            }
            return (descriptor.revents & events) ? 0 : -1;
        }
        if (result == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (errno != EINTR)
            return -1;
        if (stopping) {
            errno = EINTR;
            return -1;
        }
    }
}

static int connect_one(const struct addrinfo *address, int timeout_ms)
{
    int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0)
        return -1;

    int old_flags = fcntl(fd, F_GETFL, 0);
    if (old_flags < 0 || fcntl(fd, F_SETFL, old_flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }

    int result = connect(fd, address->ai_addr, address->ai_addrlen);
    if (result < 0 && errno == EINPROGRESS) {
        if (wait_fd(fd, POLLOUT, timeout_ms) < 0) {
            close(fd);
            return -1;
        }
        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) < 0 || socket_error) {
            if (socket_error)
                errno = socket_error;
            close(fd);
            return -1;
        }
    } else if (result < 0) {
        close(fd);
        return -1;
    }

    if (fcntl(fd, F_SETFL, old_flags) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int connect_to_server(const char *host, const char *port)
{
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *addresses = NULL;
    int error = getaddrinfo(host, port, &hints, &addresses);
    if (error != 0) {
        fprintf(stderr, "Cannot resolve %s: %s\n", host, gai_strerror(error));
        return -1;
    }

    int fd = -1;
    int saved_errno = ECONNREFUSED;
    for (struct addrinfo *address = addresses; address && !stopping; address = address->ai_next) {
        fd = connect_one(address, CONNECT_TIMEOUT_MS);
        if (fd >= 0)
            break;
        saved_errno = errno;
    }
    freeaddrinfo(addresses);
    if (fd < 0)
        errno = saved_errno;
    return fd;
}

static int send_all(int fd, const char *data, size_t length)
{
    while (length > 0 && !stopping) {
        ssize_t sent = send(fd, data, length, MSG_NOSIGNAL);
        if (sent > 0) {
            data += sent;
            length -= (size_t)sent;
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(fd, POLLOUT, IO_TIMEOUT_MS) == 0)
                continue;
            return -1;
        } else {
            if (sent == 0)
                errno = EPIPE;
            return -1;
        }
    }
    return stopping ? -1 : 0;
}

static int send_line(int fd, const char *line)
{
    if (send_all(fd, line, strlen(line)) < 0)
        return -1;
    return send_all(fd, "\n", 1);
}

static int read_line(int fd, LineReader *reader, char *line, size_t line_size)
{
    for (;;) {
        for (size_t i = reader->begin; i < reader->end; ++i) {
            if (reader->data[i] == '\n') {
                size_t length = i - reader->begin;
                if (length > 0 && reader->data[reader->begin + length - 1] == '\r')
                    --length;
                if (length >= line_size) {
                    errno = EMSGSIZE;
                    return -1;
                }
                memcpy(line, reader->data + reader->begin, length);
                line[length] = '\0';
                reader->begin = i + 1;
                if (reader->begin == reader->end)
                    reader->begin = reader->end = 0;
                return 0;
            }
        }

        if (reader->begin > 0 && reader->begin < reader->end) {
            memmove(reader->data, reader->data + reader->begin, reader->end - reader->begin);
            reader->end -= reader->begin;
            reader->begin = 0;
        } else if (reader->begin == reader->end) {
            reader->begin = reader->end = 0;
        }
        if (reader->end == sizeof(reader->data)) {
            errno = EMSGSIZE;
            return -1;
        }
        if (wait_fd(fd, POLLIN, IO_TIMEOUT_MS) < 0)
            return -1;
        ssize_t received = recv(fd, reader->data + reader->end,
                                sizeof(reader->data) - reader->end, 0);
        if (received > 0)
            reader->end += (size_t)received;
        else if (received == 0) {
            errno = ECONNRESET;
            return -1;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
    }
}

static int authenticate(int fd, const Credentials *credentials, int *probe_family)
{
    LineReader reader = {0};
    char line[MAX_LINE];
    if (read_line(fd, &reader, line, sizeof(line)) < 0)
        return -1;
    if (!strstr(line, "Authentication required")) {
        fprintf(stderr, "Server rejected connection: %s\n", line);
        errno = EACCES;
        return -1;
    }

    char login[sizeof(credentials->username) + sizeof(credentials->password) + 2];
    int length = snprintf(login, sizeof(login), "%s:%s", credentials->username, credentials->password);
    if (length < 0 || (size_t)length >= sizeof(login)) {
        errno = EMSGSIZE;
        return -1;
    }
    if (send_line(fd, login) < 0) {
        memset(login, 0, sizeof(login));
        return -1;
    }
    memset(login, 0, sizeof(login));

    if (read_line(fd, &reader, line, sizeof(line)) < 0)
        return -1;
    if (!strstr(line, "Authentication successful")) {
        fprintf(stderr, "Authentication failed: %s\n", line);
        errno = EACCES;
        return -1;
    }
    if (read_line(fd, &reader, line, sizeof(line)) < 0)
        return -1;
    if (strstr(line, "IPv4"))
        *probe_family = AF_INET6;
    else if (strstr(line, "IPv6"))
        *probe_family = AF_INET;
    else {
        fprintf(stderr, "Unexpected server response: %s\n", line);
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static CpuCounters read_cpu_counters(void)
{
    CpuCounters counters = {0};
    FILE *file = fopen("/proc/stat", "r");
    if (!file)
        return counters;
    char *line = NULL;
    size_t capacity = 0;
    if (getline(&line, &capacity, file) < 0 || strncmp(line, "cpu ", 4) != 0) {
        free(line);
        fclose(file);
        return counters;
    }

    uint64_t values[10] = {0};
    char *cursor = line + 4;
    unsigned count = 0;
    while (count < 10) {
        while (*cursor == ' ' || *cursor == '\t')
            ++cursor;
        if (*cursor < '0' || *cursor > '9')
            break;
        errno = 0;
        char *end = NULL;
        unsigned long long value = strtoull(cursor, &end, 10);
        if (errno || end == cursor)
            break;
        values[count++] = (uint64_t)value;
        cursor = end;
    }
    if (count >= 4) {
        /* guest and guest_nice are already included in user and nice. */
        counters.total = values[0] + values[1] + values[2] + values[3];
        if (count > 4) counters.total += values[4];
        if (count > 5) counters.total += values[5];
        if (count > 6) counters.total += values[6];
        if (count > 7) counters.total += values[7];
        counters.idle = values[3] + (count > 4 ? values[4] : 0);
        counters.valid = true;
    }
    free(line);
    fclose(file);
    return counters;
}

static NetworkCounters read_network_counters(void)
{
    NetworkCounters counters = {0};
    FILE *file = fopen("/proc/net/dev", "r");
    if (!file)
        return counters;

    char *line = NULL;
    size_t capacity = 0;
    unsigned line_number = 0;
    bool valid = true;
    while (getline(&line, &capacity, file) >= 0) {
        if (line_number++ < 2)
            continue;
        char *colon = strrchr(line, ':');
        if (!colon) {
            valid = false;
            break;
        }
        *colon = '\0';
        char *name = trim(line);
        if (strcmp(name, "lo") == 0 || strncmp(name, "tun", 3) == 0)
            continue;
        unsigned long long rx = 0, tx = 0;
        unsigned long long fields[16];
        int parsed = sscanf(colon + 1,
                            "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                            &fields[0], &fields[1], &fields[2], &fields[3], &fields[4], &fields[5],
                            &fields[6], &fields[7], &fields[8], &fields[9], &fields[10], &fields[11],
                            &fields[12], &fields[13], &fields[14], &fields[15]);
        if (parsed < 9) {
            valid = false;
            break;
        }
        rx = fields[0];
        tx = fields[8];
        counters.rx += (uint64_t)rx;
        counters.tx += (uint64_t)tx;
    }
    free(line);
    fclose(file);
    counters.valid = valid;
    return counters;
}

static int read_memory(uint64_t *memory_total, uint64_t *memory_used,
                       uint64_t *swap_total, uint64_t *swap_used)
{
    FILE *file = fopen("/proc/meminfo", "r");
    if (!file)
        return -1;
    uint64_t total = 0, available = 0, free_memory = 0, buffers = 0, cached = 0;
    uint64_t swap_t = 0, swap_f = 0;
    bool have_available = false;
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, file) >= 0) {
        char key[64];
        unsigned long long value;
        if (sscanf(line, "%63[^:]: %llu", key, &value) != 2)
            continue;
        if (strcmp(key, "MemTotal") == 0) total = value;
        else if (strcmp(key, "MemAvailable") == 0) { available = value; have_available = true; }
        else if (strcmp(key, "MemFree") == 0) free_memory = value;
        else if (strcmp(key, "Buffers") == 0) buffers = value;
        else if (strcmp(key, "Cached") == 0) cached = value;
        else if (strcmp(key, "SwapTotal") == 0) swap_t = value;
        else if (strcmp(key, "SwapFree") == 0) swap_f = value;
    }
    free(line);
    fclose(file);
    if (total == 0) {
        errno = EIO;
        return -1;
    }
    uint64_t free_estimate = have_available ? available : free_memory + buffers + cached;
    *memory_total = total;
    *memory_used = total > free_estimate ? total - free_estimate : 0;
    *swap_total = swap_t;
    *swap_used = swap_t > swap_f ? swap_t - swap_f : 0;
    return 0;
}

static int read_disk(const char *mountpoint, uint64_t *total_mb, uint64_t *used_mb)
{
    struct statvfs stats;
    if (statvfs(mountpoint, &stats) < 0)
        return -1;
    uint64_t block_size = (uint64_t)stats.f_frsize;
    uint64_t total_bytes = (uint64_t)stats.f_blocks * block_size;
    uint64_t free_bytes = (uint64_t)stats.f_bfree * block_size;
    uint64_t used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;
    *total_mb = total_bytes / DISK_UNIT;
    *used_mb = used_bytes / DISK_UNIT;
    return 0;
}

static double cpu_percent(CpuCounters previous, CpuCounters current)
{
    if (!previous.valid || !current.valid || current.total <= previous.total)
        return 0.0;
    uint64_t total = current.total - previous.total;
    uint64_t idle = current.idle >= previous.idle ? current.idle - previous.idle : 0;
    if (idle > total)
        idle = total;
    return (double)(total - idle) * 100.0 / (double)total;
}

static uint64_t counter_rate(uint64_t previous, uint64_t current, double elapsed)
{
    if (elapsed <= 0.0 || current < previous)
        return 0;
    double rate = (double)(current - previous) / elapsed;
    return rate >= (double)UINT64_MAX ? UINT64_MAX : (uint64_t)rate;
}

static int collect_metrics(const char *mountpoint, CpuCounters previous_cpu,
                           NetworkCounters previous_network, double elapsed,
                           Metrics *metrics, CpuCounters *current_cpu,
                           NetworkCounters *current_network)
{
    struct sysinfo system;
    if (sysinfo(&system) < 0)
        return -1;
    metrics->uptime = system.uptime < 0 ? 0 : (uint64_t)system.uptime;

    double loads[3];
    if (getloadavg(loads, 3) != 3) {
        errno = EIO;
        return -1;
    }
    metrics->load = loads[0];
    if (read_memory(&metrics->memory_total, &metrics->memory_used,
                    &metrics->swap_total, &metrics->swap_used) < 0 ||
        read_disk(mountpoint, &metrics->disk_total, &metrics->disk_used) < 0)
        return -1;

    *current_cpu = read_cpu_counters();
    metrics->cpu = cpu_percent(previous_cpu, *current_cpu);
    *current_network = read_network_counters();
    metrics->network_rx = previous_network.valid && current_network->valid
        ? counter_rate(previous_network.rx, current_network->rx, elapsed) : 0;
    metrics->network_tx = previous_network.valid && current_network->valid
        ? counter_rate(previous_network.tx, current_network->tx, elapsed) : 0;
    return 0;
}

static int probe_connect(const char *host, int family)
{
    struct addrinfo hints = {0};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *addresses = NULL;
    int error = getaddrinfo(host, "80", &hints, &addresses);
    if (error != 0)
        return 0;
    bool online = false;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        int fd = connect_one(address, PROBE_TIMEOUT_MS);
        if (fd >= 0) {
            close(fd);
            online = true;
            break;
        }
    }
    freeaddrinfo(addresses);
    return online ? 1 : 0;
}

static double monotonic_seconds(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) < 0)
        return 0.0;
    return (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
}

static void sleep_interval(double seconds)
{
    struct timespec remaining = {
        .tv_sec = (time_t)seconds,
        .tv_nsec = (long)((seconds - (double)(time_t)seconds) * 1000000000.0)
    };
    while (!stopping && nanosleep(&remaining, &remaining) < 0 && errno == EINTR)
        ;
}

static int send_updates(int fd, const char *mountpoint, int probe_family, double interval)
{
    CpuCounters previous_cpu = read_cpu_counters();
    NetworkCounters previous_network = read_network_counters();
    double previous_time = monotonic_seconds();
    double next_probe = previous_time;
    int online = 0;
    bool has_online = false;
    const char *probe_host = probe_family == AF_INET6 ? "ipv6.google.com" : "ipv4.google.com";
    const char *online_field = probe_family == AF_INET6 ? "online6" : "online4";

    while (!stopping) {
        double now = monotonic_seconds();
        if (!has_online || now >= next_probe) {
            online = probe_connect(probe_host, probe_family);
            has_online = true;
            next_probe = monotonic_seconds() + 10.0;
        }

        Metrics metrics = {0};
        CpuCounters current_cpu;
        NetworkCounters current_network;
        double elapsed = now - previous_time;
        if (elapsed <= 0.0)
            elapsed = interval;
        if (collect_metrics(mountpoint, previous_cpu, previous_network, elapsed,
                            &metrics, &current_cpu, &current_network) < 0) {
            fprintf(stderr, "Cannot collect system metrics: %s\n", strerror(errno));
            return -1;
        }

        char update[1024];
        int length = snprintf(update, sizeof(update),
            "update {\"%s\": %s, \"uptime\": %" PRIu64
            ", \"load\": %.2f, \"memory_total\": %" PRIu64
            ", \"memory_used\": %" PRIu64 ", \"swap_total\": %" PRIu64
            ", \"swap_used\": %" PRIu64 ", \"hdd_total\": %" PRIu64
            ", \"hdd_used\": %" PRIu64 ", \"cpu\": %.1f, \"network_rx\": %" PRIu64
            ", \"network_tx\": %" PRIu64 "}",
            online_field, online ? "true" : "false", metrics.uptime, metrics.load,
            metrics.memory_total, metrics.memory_used, metrics.swap_total, metrics.swap_used,
            metrics.disk_total, metrics.disk_used, metrics.cpu,
            metrics.network_rx, metrics.network_tx);
        if (length < 0 || (size_t)length >= sizeof(update)) {
            errno = EMSGSIZE;
            return -1;
        }
        if (send_line(fd, update) < 0)
            return -1;

        previous_cpu = current_cpu;
        previous_network = current_network;
        previous_time = monotonic_seconds();
        sleep_interval(interval);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *credentials_path = DEFAULT_CREDENTIALS;
    const char *config_path = NULL;
    const char *host_argument = NULL;
    const char *port_argument = NULL;
    const char *username_argument = NULL;
    const char *password_argument = NULL;
    ProgramConfig config = {.interval = DEFAULT_INTERVAL};
    strcpy(config.mountpoint, "/");
    bool credentials_selected = false;
    bool host_selected = false, port_selected = false;
    bool username_selected = false, password_selected = false;
    bool interval_selected = false, mountpoint_selected = false, quiet_selected = false;
    static const struct option options[] = {
        {"config", required_argument, NULL, 'c'},
        {"credentials", required_argument, NULL, 'C'},
        {"host", required_argument, NULL, 'H'},
        {"port", required_argument, NULL, 'p'},
        {"username", required_argument, NULL, 'u'},
        {"password", required_argument, NULL, 'w'},
        {"interval", required_argument, NULL, 'i'},
        {"mountpoint", required_argument, NULL, 'm'},
        {"quiet", no_argument, NULL, 'q'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int option;
    while ((option = getopt_long(argc, argv, "c:C:H:p:u:w:i:m:qh", options, NULL)) != -1) {
        switch (option) {
        case 'c': config_path = optarg; break;
        case 'C': credentials_path = optarg; credentials_selected = true; break;
        case 'H': host_argument = optarg; host_selected = true; break;
        case 'p': port_argument = optarg; port_selected = true; break;
        case 'u': username_argument = optarg; username_selected = true; break;
        case 'w': password_argument = optarg; password_selected = true; break;
        case 'i': {
            char *end = NULL;
            errno = 0;
            config.interval = strtod(optarg, &end);
            if (errno || end == optarg || *end != '\0' || !isfinite(config.interval) ||
                config.interval < 0.1 || config.interval > 3600.0) {
                fprintf(stderr, "Invalid interval '%s' (expected 0.1..3600 seconds)\n", optarg);
                return EXIT_FAILURE;
            }
            interval_selected = true;
            break;
        }
        case 'm':
            if (strlen(optarg) >= sizeof(config.mountpoint)) {
                fprintf(stderr, "Mountpoint path is too long\n");
                return EXIT_FAILURE;
            }
            strcpy(config.mountpoint, optarg);
            mountpoint_selected = true;
            break;
        case 'q': config.quiet = true; quiet_selected = true; break;
        case 'h': usage(stdout, argv[0]); return EXIT_SUCCESS;
        default: usage(stderr, argv[0]); return EXIT_FAILURE;
        }
    }

    int positional_count = argc - optind;
    if (config_path) {
        if (positional_count != 0 || credentials_selected || host_selected || port_selected ||
            username_selected || password_selected || interval_selected || mountpoint_selected ||
            quiet_selected) {
            fprintf(stderr, "--config cannot be combined with other settings or positional arguments\n");
            return EXIT_FAILURE;
        }
        if (read_config_json(config_path, &config) < 0)
            return EXIT_FAILURE;
    } else if (host_selected || port_selected || username_selected || password_selected) {
        if (positional_count != 0 || credentials_selected || !host_selected || !port_selected ||
            !username_selected || !password_selected) {
            fprintf(stderr, "Argument mode requires --host, --port, --username, and --password only; do not add positional arguments\n");
            return EXIT_FAILURE;
        }
        if (strlen(host_argument) >= sizeof(config.host) ||
            strlen(port_argument) >= sizeof(config.port) ||
            strlen(username_argument) >= sizeof(config.credentials.username) ||
            strlen(password_argument) >= sizeof(config.credentials.password)) {
            fprintf(stderr, "A host name or credential is too long\n");
            return EXIT_FAILURE;
        }
        strcpy(config.host, host_argument);
        strcpy(config.port, port_argument);
        strcpy(config.credentials.username, username_argument);
        strcpy(config.credentials.password, password_argument);
    } else {
        if (positional_count != 2) {
            usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
        const char *legacy_host = argv[optind];
        const char *legacy_port = argv[optind + 1];
        if (strlen(legacy_host) >= sizeof(config.host)) {
            fprintf(stderr, "Host name is too long\n");
            return EXIT_FAILURE;
        }
        if (strlen(legacy_port) >= sizeof(config.port)) {
            fprintf(stderr, "TCP port argument is too long\n");
            return EXIT_FAILURE;
        }
        strcpy(config.host, legacy_host);
        strcpy(config.port, legacy_port);
        if (load_credentials(credentials_path, &config.credentials) < 0)
            return EXIT_FAILURE;
    }

    char *end = NULL;
    errno = 0;
    long port_number = strtol(config.port, &end, 10);
    if (errno || end == config.port || *end != '\0' || port_number < 1 || port_number > 65535) {
        fprintf(stderr, "Invalid TCP port '%s'\n", config.port);
        return EXIT_FAILURE;
    }
    if (config.host[0] == '\0' || config.mountpoint[0] == '\0' ||
        config.credentials.username[0] == '\0' || config.credentials.password[0] == '\0' ||
        strchr(config.credentials.username, ':') || strchr(config.credentials.username, '\n') ||
        strchr(config.credentials.username, '\r') || strchr(config.credentials.password, '\n') ||
        strchr(config.credentials.password, '\r')) {
        fprintf(stderr, "Host, mountpoint, username and password must be non-empty; credentials cannot contain line breaks or a colon in the username\n");
        return EXIT_FAILURE;
    }
    if (!isfinite(config.interval) || config.interval < 0.1 || config.interval > 3600.0) {
        fprintf(stderr, "Invalid update interval (expected 0.1..3600 seconds)\n");
        return EXIT_FAILURE;
    }

    quiet = config.quiet;

    struct sigaction action = {0};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    while (!stopping) {
        log_message("Connecting to %s:%s...", config.host, config.port);
        int fd = connect_to_server(config.host, config.port);
        if (fd < 0) {
            if (!stopping)
                log_message("Connection failed: %s", strerror(errno));
        } else {
            int probe_family = AF_UNSPEC;
            if (authenticate(fd, &config.credentials, &probe_family) < 0) {
                if (!stopping)
                    log_message("Handshake failed: %s", strerror(errno));
            } else {
                log_message("Connected; reporting system metrics every %.1f seconds", config.interval);
                if (send_updates(fd, config.mountpoint, probe_family, config.interval) < 0 && !stopping)
                    log_message("Disconnected: %s", strerror(errno));
            }
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        if (!stopping) {
            log_message("Retrying in 3 seconds...");
            sleep_interval(3.0);
        }
    }

    memset(&config.credentials, 0, sizeof(config.credentials));
    log_message("Stopped");
    return EXIT_SUCCESS;
}
