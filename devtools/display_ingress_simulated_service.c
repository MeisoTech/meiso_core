#define _POSIX_C_SOURCE 200809L

#include "display_ingress_simulated_service_private.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    MeisoSessionRole role;
    const char *role_name;
    int control_fd;
    int media_fd;
} Arguments;

static Service service_storage;

static bool parse_fd(const char *text, int *out_fd)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || out_fd == NULL || text[0] == '\0' || text[0] == '-') {
        return false;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value > (unsigned long)INT_MAX || value < 3UL) {
        return false;
    }
    *out_fd = (int)value;
    return true;
}

static bool parse_arguments(int argc, char **argv, Arguments *out_arguments)
{
    Arguments value;
    bool role_seen = false;
    bool control_seen = false;
    bool media_seen = false;
    int index;

    if (out_arguments == NULL) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.control_fd = -1;
    value.media_fd = -1;
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--role") == 0 && index + 1 < argc &&
            !role_seen) {
            const char *role = argv[++index];
            if (strcmp(role, "host") == 0) {
                value.role = MEISO_SESSION_ROLE_HOST;
                value.role_name = "host";
            } else if (strcmp(role, "edge") == 0) {
                value.role = MEISO_SESSION_ROLE_EDGE;
                value.role_name = "edge";
            } else {
                return false;
            }
            role_seen = true;
        } else if (strcmp(argv[index], "--control-fd") == 0 &&
                   index + 1 < argc && !control_seen &&
                   parse_fd(argv[++index], &value.control_fd)) {
            control_seen = true;
        } else if (strcmp(argv[index], "--media-fd") == 0 &&
                   index + 1 < argc && !media_seen &&
                   parse_fd(argv[++index], &value.media_fd)) {
            media_seen = true;
        } else {
            return false;
        }
    }
    if (!role_seen || !control_seen || !media_seen ||
        value.control_fd == value.media_fd) {
        return false;
    }
    *out_arguments = value;
    return true;
}

int main(int argc, char **argv)
{
    Arguments arguments;
    bool success;
    bool receipt_written;

    if (!parse_arguments(argc, argv, &arguments)) {
        fputs("E_ARGUMENT\n", stderr);
        return 2;
    }
    memset(&service_storage, 0, sizeof(service_storage));
    service_storage.control_fd = arguments.control_fd;
    service_storage.media_fd = arguments.media_fd;
    service_storage.role = arguments.role;
    service_storage.role_name = arguments.role_name;
    success = service_run(&service_storage);
    service_close_fd(&service_storage.media_fd);
    service_close_fd(&service_storage.control_fd);
    service_storage.descriptors_closed =
        service_storage.media_fd == -1 && service_storage.control_fd == -1;
    if (success && !service_success_ready(&service_storage)) {
        success = service_fail(
            &service_storage, "application", "shutdown"
        );
    }
    if (success) {
        receipt_written = service_emit_success(&service_storage);
    } else {
        receipt_written = service_emit_failure(&service_storage);
    }
    return success && receipt_written ? 0 : 1;
}
