#define _POSIX_C_SOURCE 200809L

#include "meiso_wifi_runtime_host_private.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parses interactive NDJSON commands and drives apply/close transactions. */

static bool control_consume_literal(
    const char **cursor,
    const char *end,
    const char *literal
)
{
    const size_t length = literal != NULL ? strlen(literal) : 0U;

    if (cursor == NULL || *cursor == NULL || end == NULL ||
        literal == NULL || *cursor > end ||
        length > (size_t)(end - *cursor) ||
        memcmp(*cursor, literal, length) != 0) {
        return false;
    }
    *cursor += length;
    return true;
}
static bool control_parse_u32(
    const char **cursor,
    const char *end,
    uint32_t *out_value
)
{
    char *parsed_end = NULL;
    unsigned long long value;
    size_t digits;

    if (cursor == NULL || *cursor == NULL || end == NULL ||
        out_value == NULL || *cursor >= end) {
        return false;
    }
    digits = strspn(*cursor, "0123456789");
    if (digits == 0U || digits > (size_t)(end - *cursor)) {
        return false;
    }
    errno = 0;
    value = strtoull(*cursor, &parsed_end, 10);
    if (errno != 0 || parsed_end != *cursor + digits ||
        value > (unsigned long long)UINT32_MAX) {
        return false;
    }
    *cursor = parsed_end;
    *out_value = (uint32_t)value;
    return true;
}

static MeisoWifiRuntimeHostPrivateControlCommand parse_control_command(const char *line)
{
    MeisoWifiRuntimeHostPrivateControlCommand command;
    const char *cursor = line;
    const char *end;

    memset(&command, 0, sizeof(command));
    if (line == NULL) {
        return command;
    }
    end = line + strlen(line);
    if (strcmp(line, "{\"command\":\"close\"}") == 0) {
        command.type = MEISO_WIFI_RUNTIME_HOST_PRIVATE_CONTROL_CLOSE;
        return command;
    }
    if (!control_consume_literal(
            &cursor, end, "{\"command\":\"apply\",\"enabled\":"
        )) {
        return command;
    }
    if (control_consume_literal(&cursor, end, "true")) {
        command.enabled = true;
    } else if (control_consume_literal(&cursor, end, "false")) {
        command.enabled = false;
    } else {
        return command;
    }
    if (!control_consume_literal(
            &cursor, end, ",\"selected_fields\":"
        ) ||
        !control_parse_u32(
            &cursor, end, &command.parameters.selected_fields
        ) || !control_consume_literal(
            &cursor, end, ",\"sample_period_ms\":"
        ) || !control_parse_u32(
            &cursor, end, &command.parameters.sample_period_ms
        ) || !control_consume_literal(&cursor, end, "}") || cursor != end) {
        memset(&command, 0, sizeof(command));
        return command;
    }
    command.type = MEISO_WIFI_RUNTIME_HOST_PRIVATE_CONTROL_APPLY;
    return command;
}

bool meiso_wifi_runtime_host_private_extract_control_command(
    MeisoWifiRuntimeHostPrivateControlInput *input,
    MeisoWifiRuntimeHostPrivateControlCommand *out_command,
    bool *out_present
)
{
    size_t index;
    char line[MEISO_WIFI_RUNTIME_HOST_PRIVATE_CONTROL_LINE_BYTES];

    if (input == NULL || out_command == NULL || out_present == NULL) {
        return false;
    }
    *out_present = false;
    for (index = 0U; index < input->size; ++index) {
        if (input->bytes[index] != '\n') {
            continue;
        }
        if (index >= sizeof(line)) {
            return false;
        }
        memcpy(line, input->bytes, index);
        line[index] = '\0';
        memmove(
            input->bytes,
            input->bytes + index + 1U,
            input->size - index - 1U
        );
        input->size -= index + 1U;
        *out_command = parse_control_command(line);
        *out_present = true;
        return true;
    }
    return input->size < sizeof(input->bytes);
}

static void emit_control_rejected(const char *reason)
{
    (void)printf(
        "{\"type\":\"control\",\"status\":\"REJECTED\","
        "\"reason\":\"%s\"}\n",
        reason
    );
    (void)fflush(stdout);
}

static bool emit_control_applied(uint64_t version, bool enabled)
{
    if (version == 0U) {
        return false;
    }
    (void)printf(
        "{\"type\":\"control\",\"command\":\"APPLY\","
        "\"status\":\"APPLIED\",\"desired_version\":%" PRIu64 ","
        "\"enabled\":%s}\n",
        version,
        enabled ? "true" : "false"
    );
    (void)fflush(stdout);
    return true;
}

static void select_close_parameters(MeisoWifiRuntimeHostPrivate *host)
{
    if (meiso_wifi_telemetry_parameters_supported(
            &host->capability, &host->bootstrap.desired
        )) {
        return;
    }
    host->bootstrap.desired.selected_fields =
        host->capability.supported_fields;
    host->bootstrap.desired.sample_period_ms =
        host->capability.min_period_ms;
}

bool meiso_wifi_runtime_host_private_run_interactive(MeisoWifiRuntimeHostPrivate *host)
{
    MeisoWifiRuntimeHostPrivateControlInput input;
    uint64_t desired_version = 0U;
    bool desired_enabled = false;

    if (host == NULL) {
        return false;
    }
    memset(&input, 0, sizeof(input));
    (void)printf(
        "{\"type\":\"control\",\"status\":\"READY\","
        "\"mode\":\"INTERACTIVE\"}\n"
    );
    (void)fflush(stdout);
    for (;;) {
        MeisoWifiRuntimeHostPrivateControlCommand command;
        MeisoRuntimeEvent event;
        const MeisoWifiRuntimeHostPrivateInteractiveItemType item = meiso_wifi_runtime_host_private_wait_interactive_item(
            host, &input, &command, &event
        );

        if (item == MEISO_WIFI_RUNTIME_HOST_PRIVATE_INTERACTIVE_ITEM_FAILED) {
            return false;
        }
        if (item == MEISO_WIFI_RUNTIME_HOST_PRIVATE_INTERACTIVE_ITEM_RUNTIME) {
            if (!meiso_wifi_runtime_host_private_handle_interactive_runtime_event(
                    host, &event, desired_version, desired_enabled
                )) {
                return false;
            }
            continue;
        }
        if (command.type == MEISO_WIFI_RUNTIME_HOST_PRIVATE_CONTROL_INVALID) {
            emit_control_rejected("INVALID_COMMAND");
            continue;
        }
        if (command.type == MEISO_WIFI_RUNTIME_HOST_PRIVATE_CONTROL_APPLY) {
            if (!meiso_wifi_telemetry_parameters_supported(
                    &host->capability, &command.parameters
                )) {
                emit_control_rejected("UNSUPPORTED_PARAMETERS");
                continue;
            }
            if (desired_version == UINT64_MAX) {
                return false;
            }
            desired_version += 1U;
            host->bootstrap.desired = command.parameters;
            if (!meiso_wifi_runtime_host_private_publish_desired(
                    host, desired_version, command.enabled
                ) || !meiso_wifi_runtime_host_private_wait_desired_and_actual(
                    host,
                    desired_version,
                    command.enabled ? MEISO_RUNTIME_PRODUCTION_RUNNING :
                        MEISO_RUNTIME_PRODUCTION_STOPPED
                ) || !emit_control_applied(
                    desired_version, command.enabled
                )) {
                return false;
            }
            desired_enabled = command.enabled;
            continue;
        }
        if (desired_version == 0U || desired_enabled) {
            if (desired_version == UINT64_MAX) {
                return false;
            }
            select_close_parameters(host);
            desired_version += 1U;
            if (!meiso_wifi_runtime_host_private_publish_desired(host, desired_version, false) ||
                !meiso_wifi_runtime_host_private_wait_desired_and_actual(
                    host,
                    desired_version,
                    MEISO_RUNTIME_PRODUCTION_STOPPED
                )) {
                return false;
            }
        }
        if (!meiso_wifi_runtime_host_private_normal_close(host)) {
            return false;
        }
        (void)printf(
            "{\"type\":\"control\",\"command\":\"CLOSE\","
            "\"status\":\"COMPLETE\"}\n"
        );
        (void)fflush(stdout);
        return true;
    }
}
