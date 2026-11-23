#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#define main status_client_main
#include "../status.c"
#undef main

#include <assert.h>
#include <sys/wait.h>
#include <sys/un.h>

static unsigned checks = 0;
static unsigned failures = 0;
static int probe_dns_mode = 0;
static uint16_t probe_port = 0;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        ++failures; \
    } \
} while (0)

static int suppress_stderr(void)
{
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    int sink = open("/dev/null", O_WRONLY);
    if (saved < 0 || sink < 0) {
        if (saved >= 0) close(saved);
        if (sink >= 0) close(sink);
        return -1;
    }
    if (dup2(sink, STDERR_FILENO) < 0) {
        close(saved);
        close(sink);
        return -1;
    }
    close(sink);
    return saved;
}

static void restore_stderr(int saved)
{
    if (saved < 0)
        return;
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
}

int __real_getaddrinfo(const char *node, const char *service,
                       const struct addrinfo *hints, struct addrinfo **result);

int __wrap_getaddrinfo(const char *node, const char *service,
                       const struct addrinfo *hints, struct addrinfo **result)
{
    bool is_probe = strcmp(node, "ipv4.google.com") == 0 ||
                    strcmp(node, "ipv6.google.com") == 0;
    if (probe_dns_mode == 1 && is_probe)
        return EAI_NONAME;
    if (probe_dns_mode == 2 && is_probe) {
        const char *loopback = hints && hints->ai_family == AF_INET6 ? "::1" : "127.0.0.1";
        int error = __real_getaddrinfo(loopback, "80", hints, result);
        if (error != 0)
            return error;
        for (struct addrinfo *entry = *result; entry; entry = entry->ai_next) {
            if (entry->ai_family == AF_INET)
                ((struct sockaddr_in *)entry->ai_addr)->sin_port = htons(probe_port);
            else if (entry->ai_family == AF_INET6)
                ((struct sockaddr_in6 *)entry->ai_addr)->sin6_port = htons(probe_port);
        }
        return 0;
    }
    return __real_getaddrinfo(node, service, hints, result);
}

static int write_all_test(int fd, const void *buffer, size_t length)
{
    const char *data = buffer;
    while (length) {
        ssize_t n = write(fd, data, length);
        if (n > 0) {
            data += n;
            length -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static char *temporary_file(const char *contents)
{
    char *path = strdup("/tmp/status-client-test-XXXXXX");
    if (!path)
        return NULL;
    int fd = mkstemp(path);
    if (fd < 0 || write_all_test(fd, contents, strlen(contents)) < 0) {
        if (fd >= 0) close(fd);
        unlink(path);
        free(path);
        return NULL;
    }
    close(fd);
    return path;
}

static int make_listener(int family, uint16_t *port)
{
    int fd = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    if (family == AF_INET) {
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) { close(fd); return -1; }
        socklen_t length = sizeof(address);
        if (getsockname(fd, (struct sockaddr *)&address, &length) < 0) { close(fd); return -1; }
        *port = ntohs(address.sin_port);
    } else {
        struct sockaddr_in6 address = {.sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT};
        if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) { close(fd); return -1; }
        socklen_t length = sizeof(address);
        if (getsockname(fd, (struct sockaddr *)&address, &length) < 0) { close(fd); return -1; }
        *port = ntohs(address.sin6_port);
    }
    if (listen(fd, 8) < 0) { close(fd); return -1; }
    return fd;
}

static void test_utilities(void)
{
    char text[] = " \t hello world \r\n";
    CHECK(strcmp(trim(text), "hello world") == 0);

    bool previous_quiet = quiet;
    quiet = true;
    log_message("suppressed test message");
    quiet = previous_quiet;

    char *usage_text = NULL;
    size_t usage_size = 0;
    FILE *usage_stream = open_memstream(&usage_text, &usage_size);
    CHECK(usage_stream != NULL);
    if (usage_stream) {
        usage(usage_stream, "status-client");
        fclose(usage_stream);
        CHECK(strstr(usage_text, "--config FILE") != NULL);
        CHECK(strstr(usage_text, "legacy mode") != NULL);
        free(usage_text);
    }

    stopping = 0;
    handle_signal(SIGTERM);
    CHECK(stopping == 1);
    stopping = 0;
}

static void test_credentials(void)
{
    char *path = temporary_file("# sample\n\naccount secret\nignored second line\n");
    Credentials credentials = {0};
    CHECK(path != NULL);
    if (path) {
        CHECK(load_credentials(path, &credentials) == 0);
        CHECK(strcmp(credentials.username, "account") == 0);
        CHECK(strcmp(credentials.password, "secret") == 0);
        unlink(path);
        free(path);
    }
    path = temporary_file("account:secret\n");
    CHECK(path != NULL);
    if (path) {
        CHECK(load_credentials(path, &credentials) == 0);
        CHECK(strcmp(credentials.username, "account") == 0);
        CHECK(strcmp(credentials.password, "secret") == 0);
        unlink(path);
        free(path);
    }
    path = temporary_file("missing-password\n");
    CHECK(path != NULL);
    if (path) {
        int saved = suppress_stderr();
        int result = load_credentials(path, &credentials);
        restore_stderr(saved);
        CHECK(result < 0);
        unlink(path);
        free(path);
    }
}

static void test_json_helpers(void)
{
    const char *whitespace = " \t\n x";
    JsonParser parser = {.text = whitespace, .length = strlen(whitespace)};
    json_space(&parser);
    CHECK(parser.position == 4);

    parser = (JsonParser){.text = "0041", .length = 4};
    uint32_t hex = 0;
    CHECK(json_hex4(&parser, &hex) == 0 && hex == 0x41);
    parser = (JsonParser){.text = "00xz", .length = 4};
    CHECK(json_hex4(&parser, &hex) < 0);

    char utf8[16];
    size_t used = 0;
    CHECK(json_append_codepoint(utf8, sizeof(utf8), &used, 0x1f642) == 0);
    utf8[used] = '\0';
    CHECK(strcmp(utf8, "🙂") == 0);
    CHECK(json_append_codepoint(utf8, sizeof(utf8), &used, 0) < 0);

    char decoded[64];
    const char *escaped_json = "\"a\\n\\u0042\\uD83D\\uDE42\"";
    parser = (JsonParser){.text = escaped_json, .length = strlen(escaped_json)};
    CHECK(json_string(&parser, decoded, sizeof(decoded)) == 0);
    CHECK(strcmp(decoded, "a\nB🙂") == 0);
    parser = (JsonParser){.text = "\"unfinished", .length = 11};
    CHECK(json_string(&parser, decoded, sizeof(decoded)) < 0);
    const char *plain_json = "\"plain\\ttext\"";
    parser = (JsonParser){.text = plain_json, .length = strlen(plain_json)};
    CHECK(json_skip_string(&parser) == 0);

    double number = 0;
    parser = (JsonParser){.text = "-12.5e1,", .length = 8};
    CHECK(json_number(&parser, &number) == 0 && number == -125.0);
    parser = (JsonParser){.text = "01", .length = 2};
    CHECK(json_number(&parser, &number) < 0);
    parser = (JsonParser){.text = "1.", .length = 2};
    CHECK(json_number(&parser, &number) < 0);

    parser = (JsonParser){.text = "false", .length = 5};
    bool boolean = true;
    CHECK(json_boolean(&parser, &boolean) == 0 && !boolean);
    parser = (JsonParser){.text = "true", .length = 4};
    CHECK(json_boolean(&parser, &boolean) == 0 && boolean);
    parser = (JsonParser){.text = "null", .length = 4};
    CHECK(json_boolean(&parser, &boolean) < 0);

    const char *nested_json = "{\"future\":[1,true,{\"nested\":null}]}";
    parser = (JsonParser){.text = nested_json, .length = strlen(nested_json)};
    CHECK(json_skip_value(&parser) == 0);
    parser = (JsonParser){.text = "[1,]", .length = 4};
    CHECK(json_skip_value(&parser) < 0);
}

static void test_json_config(void)
{
    const char *json = "{\"host\":\"localhost\",\"port\":35601,\"username\":\"u\","
                       "\"password\":\"p\\u0031\",\"interval\":0.5,\"mountpoint\":\"/tmp\","
                       "\"quiet\":true,\"future\":[1,2]}\n";
    char *path = temporary_file(json);
    ProgramConfig config = {.interval = DEFAULT_INTERVAL};
    CHECK(path != NULL);
    if (path) {
        CHECK(read_config_json(path, &config) == 0);
        CHECK(strcmp(config.host, "localhost") == 0);
        CHECK(strcmp(config.port, "35601") == 0);
        CHECK(strcmp(config.credentials.username, "u") == 0);
        CHECK(strcmp(config.credentials.password, "p1") == 0);
        CHECK(config.interval == 0.5 && strcmp(config.mountpoint, "/tmp") == 0 && config.quiet);
        unlink(path);
        free(path);
    }

    path = temporary_file("{\"host\":\"x\",\"host\":\"y\"}");
    CHECK(path != NULL);
    if (path) {
        int saved = suppress_stderr();
        int result = read_config_json(path, &config);
        restore_stderr(saved);
        CHECK(result < 0);
        unlink(path);
        free(path);
    }
}

static void test_wait_and_sockets(void)
{
    int pipe_fds[2];
    CHECK(pipe(pipe_fds) == 0);
    CHECK(wait_fd(pipe_fds[0], POLLIN, 5) < 0 && errno == ETIMEDOUT);
    CHECK(write(pipe_fds[1], "x", 1) == 1);
    CHECK(wait_fd(pipe_fds[0], POLLIN, 100) == 0);
    close(pipe_fds[0]); close(pipe_fds[1]);

    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(send_all(pair[0], "partial-write", 13) == 0);
    char received[128] = {0};
    CHECK(read(pair[1], received, sizeof(received)) == 13);
    CHECK(strcmp(received, "partial-write") == 0);
    CHECK(send_line(pair[0], "alpha") == 0);
    CHECK(send_all(pair[0], "beta\r\n", 6) == 0);
    LineReader reader = {0};
    char line[64];
    CHECK(read_line(pair[1], &reader, line, sizeof(line)) == 0 && strcmp(line, "alpha") == 0);
    CHECK(read_line(pair[1], &reader, line, sizeof(line)) == 0 && strcmp(line, "beta") == 0);
    close(pair[0]); close(pair[1]);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    char oversized[MAX_LINE];
    memset(oversized, 'z', sizeof(oversized));
    CHECK(write_all_test(pair[0], oversized, sizeof(oversized)) == 0);
    reader = (LineReader){0};
    CHECK(read_line(pair[1], &reader, line, sizeof(line)) < 0 && errno == EMSGSIZE);
    close(pair[0]); close(pair[1]);
}

static void test_connect_functions(void)
{
    uint16_t port;
    int listener = make_listener(AF_INET, &port);
    CHECK(listener >= 0);
    if (listener < 0)
        return;
    char service[8];
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *addresses = NULL;
    CHECK(getaddrinfo("127.0.0.1", service, &hints, &addresses) == 0);
    if (addresses) {
        int fd = connect_one(addresses, 1000);
        CHECK(fd >= 0);
        int accepted = fd >= 0 ? accept(listener, NULL, NULL) : -1;
        CHECK(accepted >= 0);
        if (fd >= 0) close(fd);
        if (accepted >= 0) close(accepted);
        freeaddrinfo(addresses);
    }

    int fd = connect_to_server("127.0.0.1", service);
    CHECK(fd >= 0);
    int accepted = fd >= 0 ? accept(listener, NULL, NULL) : -1;
    CHECK(accepted >= 0);
    if (fd >= 0) close(fd);
    if (accepted >= 0) close(accepted);

    close(listener);
    fd = connect_to_server("127.0.0.1", service);
    CHECK(fd < 0);
}

static void test_authenticate(void)
{
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(pair[0]);
        char login[128];
        FILE *stream = fdopen(pair[1], "r+");
        if (!stream) _exit(10);
        const char *required = "Authentication required\n";
        if (write_all_test(pair[1], required, strlen(required)) < 0) _exit(11);
        if (!fgets(login, sizeof(login), stream) || strcmp(login, "demo:secret\n") != 0) _exit(12);
        const char *success = "Authentication successful\nIPv6\n";
        if (write_all_test(pair[1], success, strlen(success)) < 0) _exit(13);
        fclose(stream);
        _exit(0);
    }
    close(pair[1]);
    Credentials credentials = {.username = "demo", .password = "secret"};
    int family = AF_UNSPEC;
    CHECK(authenticate(pair[0], &credentials, &family) == 0);
    CHECK(family == AF_INET);
    close(pair[0]);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    CHECK(write_all_test(pair[1], "no authentication\n", 18) == 0);
    int saved = suppress_stderr();
    int auth_result = authenticate(pair[0], &credentials, &family);
    restore_stderr(saved);
    CHECK(auth_result < 0);
    close(pair[0]); close(pair[1]);
}

static void test_metric_functions(void)
{
    CpuCounters previous = {.total = 100, .idle = 60, .valid = true};
    CpuCounters current = {.total = 200, .idle = 100, .valid = true};
    CHECK(cpu_percent(previous, current) == 60.0);
    CHECK(cpu_percent(previous, previous) == 0.0);
    CHECK(cpu_percent((CpuCounters){0}, current) == 0.0);
    CHECK(counter_rate(100, 300, 2.0) == 100);
    CHECK(counter_rate(300, 100, 2.0) == 0);
    CHECK(counter_rate(1, 2, 0.0) == 0);

    CpuCounters cpu = read_cpu_counters();
    CHECK(cpu.valid && cpu.total > 0);
    NetworkCounters network = read_network_counters();
    CHECK(network.valid);

    uint64_t memory_total, memory_used, swap_total, swap_used;
    CHECK(read_memory(&memory_total, &memory_used, &swap_total, &swap_used) == 0);
    CHECK(memory_total > 0 && memory_used <= memory_total && swap_used <= swap_total);

    uint64_t disk_total, disk_used;
    CHECK(read_disk("/", &disk_total, &disk_used) == 0);
    CHECK(disk_total > 0 && disk_used <= disk_total);
    CHECK(read_disk("/path/that/does/not/exist", &disk_total, &disk_used) < 0);

    Metrics metrics = {0};
    CpuCounters cpu_after;
    NetworkCounters network_after;
    CHECK(collect_metrics("/", cpu, network, 1.0, &metrics, &cpu_after, &network_after) == 0);
    CHECK(metrics.uptime > 0 && metrics.memory_total > 0 && metrics.disk_total > 0);
    CHECK(metrics.load >= 0.0);
}

static void test_probe(void)
{
    probe_dns_mode = 1;
    CHECK(probe_connect("ipv4.google.com", AF_INET) == 0);
    uint16_t port;
    int listener = make_listener(AF_INET, &port);
    CHECK(listener >= 0);
    if (listener >= 0) {
        probe_port = port;
        probe_dns_mode = 2;
        int probe_result = probe_connect("ipv4.google.com", AF_INET);
        CHECK(probe_result == 1);
        int accepted = probe_result == 1 ? accept(listener, NULL, NULL) : -1;
        CHECK(accepted >= 0);
        if (accepted >= 0) close(accepted);
        close(listener);
    }
    probe_dns_mode = 0;
    CHECK(monotonic_seconds() > 0.0);
    double start = monotonic_seconds();
    sleep_interval(0.002);
    CHECK(monotonic_seconds() >= start);
}

static void test_send_updates(void)
{
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    uint16_t port;
    int listener = make_listener(AF_INET, &port);
    CHECK(listener >= 0);
    if (listener < 0) { close(pair[0]); close(pair[1]); return; }

    probe_port = port;
    probe_dns_mode = 2;
    stopping = 0;
    struct sigaction old_action, alarm_action = {0};
    alarm_action.sa_handler = handle_signal;
    sigemptyset(&alarm_action.sa_mask);
    sigaction(SIGALRM, &alarm_action, &old_action);
    alarm(1);
    CHECK(send_updates(pair[0], "/", AF_INET, 0.1) == 0);
    alarm(0);
    sigaction(SIGALRM, &old_action, NULL);
    probe_dns_mode = 0;
    close(listener);
    shutdown(pair[0], SHUT_WR);
    char buffer[8192] = {0};
    ssize_t length = read(pair[1], buffer, sizeof(buffer) - 1);
    CHECK(length > 0);
    CHECK(strstr(buffer, "update {\"online4\":") != NULL);
    close(pair[0]); close(pair[1]);
    stopping = 0;
}

static void test_main_entry(void)
{
    char *help_argv[] = {"status-client", "--help", NULL};
    optind = 1;
    FILE *capture = tmpfile();
    CHECK(capture != NULL);
    if (capture) {
        fflush(stdout);
        int saved_stdout = dup(STDOUT_FILENO);
        dup2(fileno(capture), STDOUT_FILENO);
        CHECK(status_client_main(2, help_argv) == EXIT_SUCCESS);
        fflush(stdout);
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
        CHECK(fseek(capture, 0, SEEK_SET) == 0);
        char output[1024] = {0};
        CHECK(fread(output, 1, sizeof(output) - 1, capture) > 0);
        CHECK(strstr(output, "--config FILE") != NULL);
        fclose(capture);
    }

    char *bad_port_argv[] = {"status-client", "--host", "localhost", "--port", "70000",
                             "--username", "u", "--password", "p", NULL};
    optind = 1;
    int saved = suppress_stderr();
    int result = status_client_main(9, bad_port_argv);
    restore_stderr(saved);
    CHECK(result == EXIT_FAILURE);
}

int main(void)
{
    test_utilities();
    test_credentials();
    test_json_helpers();
    test_json_config();
    test_wait_and_sockets();
    test_connect_functions();
    test_authenticate();
    test_metric_functions();
    test_probe();
    test_send_updates();
    test_main_entry();
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
