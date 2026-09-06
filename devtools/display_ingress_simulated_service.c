#define _POSIX_C_SOURCE 200809L

#include "display_ingress_simulated_service_private.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    MeisoSessionRole role;
    const char *role_name;
    int control_fd;
    int media_fd;
    int frame_fd;
} Arguments;

static Service service_storage;
#ifdef MEISO_DISPLAY_INGRESS_DRM_SERVICE
static const char *drm_device_path;
#endif

static bool signal_input_ready(int *readiness_fd)
{
    static const uint8_t READY = SERVICE_PAIR_INPUT_READY_BYTE;
    ssize_t count;

    if (readiness_fd == NULL || *readiness_fd < 0) {
        return true;
    }
    do {
        count = write(*readiness_fd, &READY, sizeof(READY));
    } while (count < 0 && errno == EINTR);
    service_close_fd(readiness_fd);
    return count == (ssize_t)sizeof(READY);
}

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
    bool frame_seen = false;
    int index;

    if (out_arguments == NULL) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.control_fd = -1;
    value.media_fd = -1;
    value.frame_fd = -1;
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
        } else if (strcmp(argv[index], "--frame-fd") == 0 &&
                   index + 1 < argc && !frame_seen &&
                   parse_fd(argv[++index], &value.frame_fd)) {
            frame_seen = true;
#ifdef MEISO_DISPLAY_INGRESS_DRM_SERVICE
        } else if (strcmp(argv[index], "--drm-device") == 0 &&
                   index + 1 < argc && drm_device_path == NULL) {
            drm_device_path = argv[++index];
#endif
        } else {
            return false;
        }
    }
    if (!role_seen || !control_seen || !media_seen ||
        value.control_fd == value.media_fd ||
        (value.role == MEISO_SESSION_ROLE_HOST && !frame_seen) ||
        (value.role == MEISO_SESSION_ROLE_EDGE && frame_seen) ||
        (frame_seen &&
         (value.frame_fd == value.control_fd ||
          value.frame_fd == value.media_fd))) {
        return false;
    }
    *out_arguments = value;
#ifdef MEISO_DISPLAY_INGRESS_DRM_SERVICE
    if ((value.role == MEISO_SESSION_ROLE_EDGE) != (drm_device_path != NULL)) {
        return false;
    }
#endif
    return true;
}

int service_execute_role(
    MeisoSessionRole role,
    const char *role_name,
    int control_fd,
    int media_fd,
    int frame_fd,
    int readiness_fd
)
{
    bool success;
    bool receipt_written;

    memset(&service_storage, 0, sizeof(service_storage));
    service_storage.control_fd = control_fd;
    service_storage.media_fd = media_fd;
    service_storage.frame_fd = frame_fd;
    service_storage.role = role;
    service_storage.role_name = role_name;
#ifdef MEISO_DISPLAY_INGRESS_DRM_SERVICE
    service_storage.drm_device_path = drm_device_path;
#endif
    if (service_storage.role == MEISO_SESSION_ROLE_HOST) {
        success = service_read_host_fixture_input(&service_storage);
        service_close_fd(&service_storage.frame_fd);
        if (success && !signal_input_ready(&readiness_fd)) {
            success = service_fail(
                &service_storage, "application", "bootstrap"
            );
        }
    } else {
        success = true;
    }
    service_close_fd(&readiness_fd);
#ifdef MEISO_DISPLAY_INGRESS_DRM_SERVICE
    if (success) {
        success = service_drm_open(&service_storage);
    }
#endif
    if (success) {
        success = service_run(&service_storage);
    }
#ifdef MEISO_DISPLAY_INGRESS_DRM_SERVICE
    if (!service_drm_close(&service_storage)) {
        success = false;
    }
#endif
    service_close_fd(&service_storage.frame_fd);
    service_close_fd(&service_storage.media_fd);
    service_close_fd(&service_storage.control_fd);
    service_storage.descriptors_closed =
        service_storage.frame_fd == -1 && service_storage.media_fd == -1 &&
        service_storage.control_fd == -1;
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

int main(int argc, char **argv)
{
    Arguments arguments;
#ifndef MEISO_DISPLAY_INGRESS_DRM_SERVICE
    int frame_fd = -1;
    if (argc == 4 && argv != NULL && argv[0] != NULL &&
        argv[0][0] == '/' && strcmp(argv[1], "--pair") == 0 &&
        strcmp(argv[2], "--frame-fd") == 0 &&
        parse_fd(argv[3], &frame_fd)) {
        return service_execute_pair(frame_fd);
    }
#endif
    if (!parse_arguments(argc, argv, &arguments)) {
        fputs("E_ARGUMENT\n", stderr);
        return 2;
    }
    return service_execute_role(
        arguments.role,
        arguments.role_name,
        arguments.control_fd,
        arguments.media_fd,
        arguments.frame_fd,
        -1
    );
}
