#define _POSIX_C_SOURCE 200809L

#include "meiso_k230d_wifi_runtime_edge_private.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Owns strict private configuration parsing. The caller receives a complete,
 * validated value or no configuration mutation. No descriptor escapes here.
 */

static char *trim_text(char *value)
{
    char *start = value;
    char *end;

    while (*start != '\0' && isspace((unsigned char)*start)) {
        start += 1;
    }
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end -= 1;
    }
    *end = '\0';
    return start;
}

static bool copy_config_text(char *output, size_t capacity, const char *value)
{
    const size_t length = value != NULL ? strlen(value) : 0U;
    if (output == NULL || capacity == 0U || length == 0U ||
        length >= capacity) {
        return false;
    }
    memmove(output, value, length + 1U);
    return true;
}

static bool parse_u32(const char *text, int base, uint32_t *out_value)
{
    char *end = NULL;
    uintmax_t value;

    if (text == NULL || out_value == NULL || text[0] == '\0' ||
        text[0] == '-' || text[0] == '+') {
        return false;
    }
    errno = 0;
    value = strtoumax(text, &end, base);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
        return false;
    }
    *out_value = (uint32_t)value;
    return true;
}

static bool parse_u64_config(
    const char *text,
    int base,
    uint64_t *out_value
)
{
    char *end = NULL;
    uintmax_t value;

    if (text == NULL || out_value == NULL || text[0] == '\0' ||
        text[0] == '-' || text[0] == '+') {
        return false;
    }
    errno = 0;
    value = strtoumax(text, &end, base);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
#if UINTMAX_MAX > UINT64_MAX
    if (value > UINT64_MAX) {
        return false;
    }
#endif
    *out_value = (uint64_t)value;
    return true;
}

bool meiso_k230d_wifi_runtime_edge_private_parse_config(
    const char *path,
    K230dWifiRuntimeEdgeConfig *out_config
)
{
    enum {
        HAVE_LISTEN_IPV4 = 1U << 0U,
        HAVE_LISTEN_PORT = 1U << 1U,
        HAVE_INTERFACE = 1U << 2U,
        HAVE_FIELDS = 1U << 3U,
        HAVE_MIN_PERIOD = 1U << 4U,
        HAVE_MAX_PERIOD = 1U << 5U,
        HAVE_PERIOD_STEP = 1U << 6U,
        HAVE_LOCAL_EDGE_HIGH = 1U << 7U,
        HAVE_LOCAL_EDGE_LOW = 1U << 8U,
        HAVE_EXPECTED_HOST_HIGH = 1U << 9U,
        HAVE_EXPECTED_HOST_LOW = 1U << 10U,
        HAVE_ALL = (1U << 11U) - 1U
    };
    K230dWifiRuntimeEdgeConfig value;
    FILE *file;
    char line[K230D_WIFI_RUNTIME_EDGE_CONFIG_LINE_BYTES];
    unsigned int present = 0U;
    bool valid = true;

    if (path == NULL || out_config == NULL) {
        return false;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    while (valid && fgets(line, (int)sizeof(line), file) != NULL) {
        char *entry;
        char *equals;
        char *key;
        char *field_value;
        uint32_t number;

        if (strchr(line, '\n') == NULL && !feof(file)) {
            valid = false;
            break;
        }
        entry = trim_text(line);
        if (*entry == '\0' || *entry == '#') {
            continue;
        }
        equals = strchr(entry, '=');
        if (equals == NULL || strchr(equals + 1, '=') != NULL) {
            valid = false;
            break;
        }
        *equals = '\0';
        key = trim_text(entry);
        field_value = trim_text(equals + 1);
        if (strcmp(key, "listen_ipv4") == 0 &&
            (present & HAVE_LISTEN_IPV4) == 0U &&
            copy_config_text(
                value.listen_ipv4, sizeof(value.listen_ipv4), field_value
            )) {
            present |= HAVE_LISTEN_IPV4;
        } else if (strcmp(key, "listen_port") == 0 &&
                   (present & HAVE_LISTEN_PORT) == 0U &&
                   parse_u32(field_value, 10, &number) && number != 0U &&
                   number <= UINT16_MAX) {
            value.listen_port = (uint16_t)number;
            present |= HAVE_LISTEN_PORT;
        } else if (strcmp(key, "interface") == 0 &&
                   (present & HAVE_INTERFACE) == 0U &&
                   copy_config_text(
                       value.interface_name,
                       sizeof(value.interface_name),
                       field_value
                   )) {
            present |= HAVE_INTERFACE;
        } else if (strcmp(key, "allowed_fields") == 0 &&
                   (present & HAVE_FIELDS) == 0U &&
                   parse_u32(field_value, 0, &number) && number != 0U &&
                   (number & ~MEISO_WIFI_TELEMETRY_FIELD_ALL) == 0U) {
            value.allowed_fields = number;
            present |= HAVE_FIELDS;
        } else if (strcmp(key, "min_period_ms") == 0 &&
                   (present & HAVE_MIN_PERIOD) == 0U &&
                   parse_u32(field_value, 10, &value.min_period_ms)) {
            present |= HAVE_MIN_PERIOD;
        } else if (strcmp(key, "max_period_ms") == 0 &&
                   (present & HAVE_MAX_PERIOD) == 0U &&
                   parse_u32(field_value, 10, &value.max_period_ms)) {
            present |= HAVE_MAX_PERIOD;
        } else if (strcmp(key, "period_step_ms") == 0 &&
                   (present & HAVE_PERIOD_STEP) == 0U &&
                   parse_u32(field_value, 10, &value.period_step_ms)) {
            present |= HAVE_PERIOD_STEP;
        } else if (strcmp(key, "local_edge_peer_id_high") == 0 &&
                   (present & HAVE_LOCAL_EDGE_HIGH) == 0U &&
                   parse_u64_config(
                       field_value, 0, &value.local_edge_peer_id.high
                   )) {
            present |= HAVE_LOCAL_EDGE_HIGH;
        } else if (strcmp(key, "local_edge_peer_id_low") == 0 &&
                   (present & HAVE_LOCAL_EDGE_LOW) == 0U &&
                   parse_u64_config(
                       field_value, 0, &value.local_edge_peer_id.low
                   )) {
            present |= HAVE_LOCAL_EDGE_LOW;
        } else if (strcmp(key, "expected_host_peer_id_high") == 0 &&
                   (present & HAVE_EXPECTED_HOST_HIGH) == 0U &&
                   parse_u64_config(
                       field_value, 0, &value.expected_host_peer_id.high
                   )) {
            present |= HAVE_EXPECTED_HOST_HIGH;
        } else if (strcmp(key, "expected_host_peer_id_low") == 0 &&
                   (present & HAVE_EXPECTED_HOST_LOW) == 0U &&
                   parse_u64_config(
                       field_value, 0, &value.expected_host_peer_id.low
                   )) {
            present |= HAVE_EXPECTED_HOST_LOW;
        } else {
            valid = false;
        }
    }
    if (ferror(file)) {
        (void)fclose(file);
        return false;
    }
    if (fclose(file) != 0) {
        return false;
    }
    if (!valid || present != HAVE_ALL ||
        !meiso_peer_id_valid(value.local_edge_peer_id) ||
        !meiso_peer_id_valid(value.expected_host_peer_id) ||
        meiso_peer_id_equal(
            value.local_edge_peer_id, value.expected_host_peer_id
        ) ||
        inet_pton(AF_INET, value.listen_ipv4, &(struct in_addr){0}) != 1) {
        return false;
    }
    *out_config = value;
    return true;
}
