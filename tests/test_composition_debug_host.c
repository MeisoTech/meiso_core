#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int meiso_composition_debug_host_run(int argc, char **argv);

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void)fprintf(                                                     \
                stderr, "CHECK failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition                                \
            );                                                                 \
            return false;                                                      \
        }                                                                      \
    } while (0)

enum {
    CONFIG_BYTES = 4096,
    INPUT_BYTES = 65536,
    OUTPUT_BYTES = 131072,
    ERROR_BYTES = 4096
};

typedef struct {
    const char *bytes;
    size_t size;
    int fd;
    int error_number;
} Writer;

typedef struct {
    int exit_code;
    char output[OUTPUT_BYTES];
    size_t output_size;
    char error[ERROR_BYTES];
    size_t error_size;
    int writer_error;
} RunResult;

static void close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static bool append_text(
    char *buffer,
    size_t capacity,
    size_t *size,
    const char *format,
    ...
)
{
    va_list arguments;
    int written;

    if (buffer == NULL || size == NULL || format == NULL ||
        *size >= capacity) {
        return false;
    }
    va_start(arguments, format);
    written = vsnprintf(buffer + *size, capacity - *size, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= capacity - *size) {
        return false;
    }
    *size += (size_t)written;
    return true;
}

static bool write_all(int fd, const char *bytes, size_t size)
{
    size_t offset = 0U;

    while (offset < size) {
        ssize_t result = write(fd, bytes + offset, size - offset);
        if (result > 0) {
            offset += (size_t)result;
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static void *writer_main(void *context)
{
    Writer *writer = context;

    if (!write_all(writer->fd, writer->bytes, writer->size)) {
        writer->error_number = errno == 0 ? EIO : errno;
    }
    close_fd(&writer->fd);
    return NULL;
}

static bool read_all(
    int fd,
    char *buffer,
    size_t capacity,
    size_t *out_size
)
{
    size_t size = 0U;

    if (buffer == NULL || capacity == 0U || out_size == NULL) {
        return false;
    }
    while (size + 1U < capacity) {
        ssize_t result = read(fd, buffer + size, capacity - size - 1U);
        if (result > 0) {
            size += (size_t)result;
        } else if (result == 0) {
            buffer[size] = '\0';
            *out_size = size;
            return true;
        } else if (errno != EINTR) {
            return false;
        }
    }
    return false;
}

static bool make_control_socket(
    bool listening,
    int *out_fd,
    uint16_t *out_port
)
{
    struct sockaddr_in address;
    socklen_t address_size = (socklen_t)sizeof(address);
    int fd = -1;
    int enabled = 1;

    if (out_fd == NULL || out_port == NULL) {
        return false;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || setsockopt(
            fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)
        ) != 0) {
        close_fd(&fd);
        return false;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        close_fd(&fd);
        return false;
    }
    if (bind(fd, (const struct sockaddr *)(const void *)&address,
             sizeof(address)) != 0 ||
        (listening && listen(fd, 1) != 0) ||
        getsockname(fd, (struct sockaddr *)(void *)&address,
                    &address_size) != 0) {
        close_fd(&fd);
        return false;
    }
    *out_fd = fd;
    *out_port = ntohs(address.sin_port);
    return *out_port != 0U;
}

static bool build_config(
    char *buffer,
    size_t capacity,
    uint16_t control_port,
    const char *extra,
    const char *omit_key
)
{
    static const struct {
        const char *key;
        const char *value;
    } entries[] = {
        {"control_ip", "127.0.0.1"},
        {"media_ip", "127.0.0.1"},
        {"media_port", "9"},
        {"local_host_peer_id_high", "1"},
        {"local_host_peer_id_low", "2"},
        {"expected_edge_peer_id_high", "3"},
        {"expected_edge_peer_id_low", "4"},
        {"local_incarnation", "5"},
        {"nonce_high", "6"},
        {"nonce_low", "7"},
        {"target_manifest", "k230d_zero_gc2093_v1"},
        {"wifi_supported_fields", "1"},
        {"device_supported_fields", "1"},
        {"catalog_generation", "1"},
        {"wifi_incarnation", "11"},
        {"presentation_incarnation", "12"},
        {"scene_incarnation", "13"},
        {"device_incarnation", "14"},
        {"render_incarnation", "15"},
        {"camera_incarnation", "16"}
    };
    size_t size = 0U;
    size_t index;

    if (!append_text(buffer, capacity, &size, "control_port=%u\n",
                     (unsigned)control_port)) {
        return false;
    }
    for (index = 0U; index < sizeof(entries) / sizeof(entries[0]); ++index) {
        if (omit_key != NULL && strcmp(omit_key, entries[index].key) == 0) {
            continue;
        }
        if (!append_text(buffer, capacity, &size, "%s=%s\n",
                         entries[index].key, entries[index].value)) {
            return false;
        }
    }
    return extra == NULL || append_text(buffer, capacity, &size, "%s", extra);
}

static bool write_config_file(
    const char *contents,
    char *out_path,
    size_t path_capacity
)
{
    static const char path_template[] =
        "/tmp/meiso-composition-debug-host-XXXXXX";
    int fd;
    size_t size;

    if (contents == NULL || out_path == NULL ||
        path_capacity < sizeof(path_template)) {
        return false;
    }
    memcpy(out_path, path_template, sizeof(path_template));
    fd = mkstemp(out_path);
    if (fd < 0) {
        return false;
    }
    size = strlen(contents);
    if (!write_all(fd, contents, size) || close(fd) != 0) {
        close_fd(&fd);
        (void)unlink(out_path);
        return false;
    }
    return true;
}

static bool run_service(
    const char *config_contents,
    const char *input,
    size_t input_size,
    RunResult *out_result
)
{
    char config_path[sizeof("/tmp/meiso-composition-debug-host-XXXXXX")];
    char *arguments[] = {
        (char *)"composition_debug_host",
        (char *)"--config",
        config_path,
        (char *)"--interactive",
        NULL
    };
    int input_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    int error_pipe[2] = {-1, -1};
    int saved_input = -1;
    int saved_output = -1;
    int saved_error = -1;
    pthread_t writer_thread;
    bool writer_started = false;
    Writer writer;
    bool ok = false;

    if (config_contents == NULL || input == NULL || out_result == NULL ||
        !write_config_file(config_contents, config_path, sizeof(config_path))) {
        return false;
    }
    memset(out_result, 0, sizeof(*out_result));
    memset(&writer, 0, sizeof(writer));
    writer.bytes = input;
    writer.size = input_size;
    writer.fd = -1;
    if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0 ||
        pipe(error_pipe) != 0) {
        goto done;
    }
    if (fflush(NULL) != 0) {
        goto done;
    }
    saved_input = dup(STDIN_FILENO);
    saved_output = dup(STDOUT_FILENO);
    saved_error = dup(STDERR_FILENO);
    if (saved_input < 0 || saved_output < 0 || saved_error < 0 ||
        dup2(input_pipe[0], STDIN_FILENO) < 0 ||
        dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
        dup2(error_pipe[1], STDERR_FILENO) < 0) {
        goto done;
    }
    close_fd(&input_pipe[0]);
    close_fd(&output_pipe[1]);
    close_fd(&error_pipe[1]);
    writer.fd = input_pipe[1];
    input_pipe[1] = -1;
    if (pthread_create(&writer_thread, NULL, writer_main, &writer) != 0) {
        goto done;
    }
    writer_started = true;
    out_result->exit_code = meiso_composition_debug_host_run(4, arguments);
    if (fflush(NULL) != 0 || dup2(saved_input, STDIN_FILENO) < 0 ||
        dup2(saved_output, STDOUT_FILENO) < 0 ||
        dup2(saved_error, STDERR_FILENO) < 0) {
        goto done;
    }
    close_fd(&saved_input);
    close_fd(&saved_output);
    close_fd(&saved_error);
    if (pthread_join(writer_thread, NULL) != 0) {
        writer_started = false;
        goto done;
    }
    writer_started = false;
    out_result->writer_error = writer.error_number;
    if (!read_all(output_pipe[0], out_result->output,
                  sizeof(out_result->output), &out_result->output_size) ||
        !read_all(error_pipe[0], out_result->error,
                  sizeof(out_result->error), &out_result->error_size)) {
        goto done;
    }
    ok = true;

done:
    if (saved_input >= 0) {
        (void)dup2(saved_input, STDIN_FILENO);
    }
    if (saved_output >= 0) {
        (void)dup2(saved_output, STDOUT_FILENO);
    }
    if (saved_error >= 0) {
        (void)dup2(saved_error, STDERR_FILENO);
    }
    close_fd(&saved_input);
    close_fd(&saved_output);
    close_fd(&saved_error);
    if (writer_started) {
        close_fd(&writer.fd);
        (void)pthread_join(writer_thread, NULL);
    }
    close_fd(&writer.fd);
    close_fd(&input_pipe[0]);
    close_fd(&input_pipe[1]);
    close_fd(&output_pipe[0]);
    close_fd(&output_pipe[1]);
    close_fd(&error_pipe[0]);
    close_fd(&error_pipe[1]);
    (void)unlink(config_path);
    return ok;
}

static size_t occurrence_count(const char *text, const char *needle)
{
    size_t count = 0U;
    size_t needle_size = strlen(needle);

    while ((text = strstr(text, needle)) != NULL) {
        ++count;
        text += needle_size;
    }
    return count;
}

static bool output_is_closed_ndjson(const char *output)
{
    const char *line = output;

    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        size_t size;
        if (end == NULL) {
            return false;
        }
        size = (size_t)(end - line);
        if (size < 2U || line[0] != '{' || line[size - 1U] != '}' ||
            strstr(line, "\"unknown\"") != NULL) {
            return false;
        }
        line = end + 1;
    }
    return true;
}

static bool test_config_rejections(void)
{
    char config[CONFIG_BYTES];
    RunResult result;

    CHECK(build_config(config, sizeof(config), 9U,
                       "control_port=10\n", NULL));
    CHECK(run_service(config, "", 0U, &result));
    CHECK(result.exit_code == 2);
    CHECK(result.output_size == 0U);
    CHECK(strcmp(result.error, "E_CONFIG\n") == 0);

    CHECK(build_config(config, sizeof(config), 9U,
                       "unexpected_key=1\n", NULL));
    CHECK(run_service(config, "", 0U, &result));
    CHECK(result.exit_code == 2);
    CHECK(result.output_size == 0U);
    CHECK(strcmp(result.error, "E_CONFIG\n") == 0);

    CHECK(build_config(config, sizeof(config), 9U, NULL,
                       "camera_incarnation"));
    CHECK(run_service(config, "", 0U, &result));
    CHECK(result.exit_code == 2);
    CHECK(result.output_size == 0U);
    CHECK(strcmp(result.error, "E_CONFIG\n") == 0);
    return true;
}

static bool test_numeric_connect_failure(void)
{
    char config[CONFIG_BYTES];
    RunResult result;
    int bound_socket = -1;
    uint16_t port;

    CHECK(make_control_socket(false, &bound_socket, &port));
    CHECK(build_config(config, sizeof(config), port, NULL, NULL));
    CHECK(run_service(config, "", 0U, &result));
    CHECK(result.exit_code == 1);
    CHECK(result.output_size == 0U);
    CHECK(strcmp(result.error, "E_INIT\n") == 0);
    close_fd(&bound_socket);
    return true;
}

static bool append_scene(
    char *input,
    size_t capacity,
    size_t *size,
    uint64_t request_id,
    size_t object_count,
    bool long_numbers
)
{
    const char *zero = long_numbers ? "0.00000000000000000000" : "0";
    const char *one = long_numbers ? "1.00000000000000000000" : "1";
    size_t index;

    if (!append_text(
            input, capacity, size,
            "{\"command\":\"set_scene\",\"request_id\":%llu,"
            "\"enabled\":true,\"camera\":{\"position\":[%s,%s,%s],"
            "\"orientation_xyzw\":[%s,%s,%s,%s],"
            "\"vertical_fov_degrees\":60,\"near_z\":0.1,"
            "\"far_z\":100},\"objects\":[",
            (unsigned long long)request_id, zero, zero, one, zero, zero,
            zero, one
        )) {
        return false;
    }
    for (index = 0U; index < object_count; ++index) {
        if (!append_text(
                input, capacity, size,
                "%s{\"editor_id\":%llu,\"asset_handle\":\"cube\","
                "\"position\":[%s,%s,%s],\"orientation_xyzw\":[%s,%s,%s,%s],"
                "\"scale\":[%s,%s,%s],\"visible\":true}",
                index == 0U ? "" : ",", (unsigned long long)(index + 1U),
                zero, zero, zero, zero, zero, zero, one, one, one, one
            )) {
            return false;
        }
    }
    return append_text(input, capacity, size, "]}\n");
}

static bool run_valid_session(
    const char *input,
    size_t input_size,
    RunResult *out_result
)
{
    char config[CONFIG_BYTES];
    int listener = -1;
    uint16_t port;
    bool ok;

    if (!make_control_socket(true, &listener, &port) ||
        !build_config(config, sizeof(config), port, NULL, NULL)) {
        close_fd(&listener);
        return false;
    }
    ok = run_service(config, input, input_size, out_result);
    close_fd(&listener);
    return ok;
}

static bool test_pre_ready_commands_and_redaction(void)
{
    static const char invalid_commands[] =
        "{\"command\":\"set_scene\",\"request_id\":3,\"enabled\":true,"
        "\"space_epoch\":9,\"camera\":{\"position\":[0,0,1],"
        "\"orientation_xyzw\":[0,0,0,1],\"vertical_fov_degrees\":60,"
        "\"near_z\":0.1,\"far_z\":100},\"objects\":[]}\n"
        "{\"command\":\"set_scene\",\"request_id\":4,\"enabled\":true,"
        "\"camera\":{\"position\":[0,0,1],\"orientation_xyzw\":[0,0,0,1],"
        "\"vertical_fov_degrees\":60,\"near_z\":0.1,\"far_z\":100,"
        "\"source_clock_epoch\":9},\"objects\":[]}\n"
        "{\"command\":\"set_scene\",\"request_id\":5,\"enabled\":true,"
        "\"camera\":{\"position\":[0,0,1],\"orientation_xyzw\":[0,0,0,1],"
        "\"vertical_fov_degrees\":60,\"near_z\":0.1,\"far_z\":100,"
        "\"source_monotonic_ns\":9},\"objects\":[]}\n"
        "{\"command\":\"set_scene\",\"request_id\":6,\"enabled\":true,"
        "\"camera\":{\"position\":[0,0,1],\"orientation_xyzw\":[0,0,0,1],"
        "\"vertical_fov_degrees\":60,\"near_z\":0.1,\"far_z\":100,"
        "\"viewport_width\":320},\"objects\":[]}\n"
        "{\"command\":\"set_scene\",\"request_id\":7,\"enabled\":true,"
        "\"camera\":{\"position\":[0,0,1],\"orientation_xyzw\":[0,0,0,1],"
        "\"vertical_fov_degrees\":60,\"near_z\":0.1,\"far_z\":100},"
        "\"objects\":[{\"editor_id\":1,\"asset_handle\":\"cube\","
        "\"position\":[0,0,0],\"orientation_xyzw\":[0,0,0,1],"
        "\"scale\":[1,1,1],\"visible\":true,\"space_epoch\":9}]}\n";
    char input[INPUT_BYTES];
    size_t size = 0U;
    RunResult result;
    const char *forbidden[] = {
        "peer_id", "session_id", "nonce", "binding", "content_id",
        "object_id", "space_epoch", "source_clock_epoch",
        "source_monotonic_ns", "viewport_width"
    };
    size_t index;

    CHECK(append_text(
        input, sizeof(input), &size,
        "{\"command\":\"asset.load_builtin\",\"request_id\":1,"
        "\"asset_handle\":\"cube\"}\n"
    ));
    CHECK(append_scene(input, sizeof(input), &size, 2U, 1U, false));
    CHECK(append_text(input, sizeof(input), &size, "%s", invalid_commands));
    CHECK(append_text(
        input, sizeof(input), &size,
        "{\"command\":\"snapshot\",\"request_id\":8}\n"
    ));
    CHECK(run_valid_session(input, size, &result));
    CHECK(result.exit_code == 0);
    CHECK(result.writer_error == 0);
    CHECK(result.error_size == 0U);
    CHECK(strstr(
        result.output,
        "{\"type\":\"service.ready\",\"profile\":\"composition-v3\","
        "\"wire_owner\":\"native_service\","
        "\"asset_import\":\"builtin_cube_only\","
        "\"frame_projection\":\"CMFB/v1\"}\n"
    ) == result.output);
    CHECK(strstr(result.output, "\"kind\":\"put_mesh\"") != NULL);
    CHECK(strstr(result.output, "\"kind\":\"set_scene\"") != NULL);
    CHECK(strstr(result.output, "\"type\":\"snapshot\",\"request_id\":8") !=
          NULL);
    CHECK(occurrence_count(
        result.output,
        "{\"type\":\"error\",\"request_id\":0,"
        "\"code\":\"INVALID_COMMAND\"}"
    ) == 5U);
    CHECK(output_is_closed_ndjson(result.output));
    for (index = 0U; index < sizeof(forbidden) / sizeof(forbidden[0]);
         ++index) {
        CHECK(strstr(result.output, forbidden[index]) == NULL);
    }
    return true;
}

static bool test_large_hup_drain(void)
{
    char input[INPUT_BYTES];
    size_t size = 0U;
    size_t first_line_end;
    RunResult result;

    CHECK(append_text(
        input, sizeof(input), &size,
        "{\"command\":\"asset.load_builtin\",\"request_id\":1,"
        "\"asset_handle\":\"cube\"}\n"
    ));
    CHECK(append_scene(input, sizeof(input), &size, 2U, 32U, true));
    first_line_end = size;
    CHECK(append_scene(input, sizeof(input), &size, 3U, 32U, true));
    CHECK(first_line_end < 16384U);
    CHECK(size - first_line_end < 16384U);
    CHECK(size > 16384U);
    CHECK(run_valid_session(input, size, &result));
    CHECK(result.exit_code == 0);
    CHECK(result.writer_error == 0);
    CHECK(result.error_size == 0U);
    CHECK(occurrence_count(result.output, "\"type\":\"command.receipt\"") ==
          3U);
    CHECK(strstr(result.output, "\"request_id\":2") != NULL);
    CHECK(strstr(result.output, "\"request_id\":3") != NULL);
    CHECK(output_is_closed_ndjson(result.output));
    return true;
}

static bool test_partial_line_eof(void)
{
    static const char partial[] =
        "{\"command\":\"asset.load_builtin\",\"request_id\":1";
    RunResult result;

    CHECK(run_valid_session(partial, sizeof(partial) - 1U, &result));
    CHECK(result.exit_code == 1);
    CHECK(result.writer_error == 0);
    CHECK(strcmp(result.error, "E_RUNTIME\n") == 0);
    CHECK(strstr(result.output, "\"type\":\"service.ready\"") != NULL);
    CHECK(strstr(
        result.output,
        "{\"type\":\"error\",\"request_id\":0,"
        "\"code\":\"INVALID_COMMAND\"}\n"
    ) != NULL);
    CHECK(output_is_closed_ndjson(result.output));
    return true;
}

static volatile sig_atomic_t signal_count;

static void test_signal_handler(int signal_number)
{
    if (signal_number == SIGPIPE) {
        ++signal_count;
    }
}

static bool test_sigpipe_restore(void)
{
    struct sigaction before;
    struct sigaction after;
    struct sigaction original;
    RunResult result;

    memset(&before, 0, sizeof(before));
    before.sa_handler = test_signal_handler;
    CHECK(sigemptyset(&before.sa_mask) == 0);
    CHECK(sigaddset(&before.sa_mask, SIGUSR1) == 0);
    before.sa_flags = SA_RESTART;
    signal_count = 0;
    CHECK(sigaction(SIGPIPE, &before, &original) == 0);
    CHECK(run_valid_session("", 0U, &result));
    CHECK(result.exit_code == 0);
    CHECK(result.error_size == 0U);
    CHECK(sigaction(SIGPIPE, NULL, &after) == 0);
    CHECK(after.sa_handler == test_signal_handler);
    CHECK((after.sa_flags & SA_RESTART) != 0);
    CHECK(sigismember(&after.sa_mask, SIGUSR1) == 1);
    CHECK(signal_count == 0);
    CHECK(sigaction(SIGPIPE, &original, NULL) == 0);
    return true;
}

int main(void)
{
    if (!test_config_rejections() || !test_numeric_connect_failure() ||
        !test_pre_ready_commands_and_redaction() ||
        !test_large_hup_drain() || !test_partial_line_eof() ||
        !test_sigpipe_restore()) {
        return 1;
    }
    (void)puts("composition debug host tests passed");
    return 0;
}
