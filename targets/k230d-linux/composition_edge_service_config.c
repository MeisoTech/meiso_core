#define _POSIX_C_SOURCE 200809L

#include "meiso_k230d_composition_edge_service_private.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Owns strict parsing of the private process bootstrap file. It publishes
 * only a fully validated, value-owned configuration to the lifecycle layer.
 */

static char *trim_text(char *text)
{
    char *end;
    while (*text == ' ' || *text == '\t' || *text == '\r' ||
           *text == '\n') {
        ++text;
    }
    end = text + strlen(text);
    while (end != text &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
            end[-1] == '\n')) {
        --end;
    }
    *end = '\0';
    return text;
}

bool meiso_k230d_composition_edge_service_private_copy_text(
    char *output,
    size_t capacity,
    const char *input
)
{
    const size_t length = input != NULL ? strlen(input) : 0U;
    if (output == NULL || capacity == 0U || input == NULL || length == 0U ||
        length >= capacity) {
        return false;
    }
    memcpy(output, input, length + 1U);
    return true;
}

static bool parse_u64(const char *text, uint64_t *out_value)
{
    char *end = NULL;
    uintmax_t value;
    if (text == NULL || out_value == NULL || *text == '\0' ||
        *text == '-' || *text == '+') {
        return false;
    }
    errno = 0;
    value = strtoumax(text, &end, 0);
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

static bool parse_port(const char *text, uint16_t *out_port)
{
    uint64_t value;
    if (!parse_u64(text, &value) || value == 0U || value > UINT16_MAX) {
        return false;
    }
    *out_port = (uint16_t)value;
    return true;
}

bool meiso_k230d_composition_edge_service_private_parse_private_config(
    const char *path,
    K230dCompositionEdgeServiceConfig *out_config
)
{
    enum {
        HAVE_IPV4 = UINT16_C(1) << 0U,
        HAVE_CONTROL_PORT = UINT16_C(1) << 1U,
        HAVE_MEDIA_PORT = UINT16_C(1) << 2U,
        HAVE_INTERFACE = UINT16_C(1) << 3U,
        HAVE_LOCAL_HIGH = UINT16_C(1) << 4U,
        HAVE_LOCAL_LOW = UINT16_C(1) << 5U,
        HAVE_HOST_HIGH = UINT16_C(1) << 6U,
        HAVE_HOST_LOW = UINT16_C(1) << 7U,
        HAVE_LOCAL_INCARNATION = UINT16_C(1) << 8U,
        HAVE_CATALOG = UINT16_C(1) << 9U,
        HAVE_ENDPOINT_BASE = UINT16_C(1) << 10U,
        HAVE_ALL = (UINT16_C(1) << 11U) - 1U
    };
    K230dCompositionEdgeServiceConfig value;
    FILE *file;
    struct stat source_status;
    char line[CONFIG_LINE_BYTES];
    uint16_t present = 0U;
    size_t rows = 0U;
    bool valid = true;

    if (path == NULL || out_config == NULL ||
        stat(path, &source_status) != 0 ||
        !S_ISREG(source_status.st_mode)) {
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
        char *field;

        if (++rows > CONFIG_MAX_ROWS ||
            (strchr(line, '\n') == NULL && !feof(file))) {
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
        field = trim_text(equals + 1);
        if (strcmp(key, "listen_ipv4") == 0 &&
            (present & HAVE_IPV4) == 0U &&
            meiso_k230d_composition_edge_service_private_copy_text(
                value.listen_ipv4, sizeof(value.listen_ipv4), field
            )) {
            present |= HAVE_IPV4;
        } else if (strcmp(key, "control_port") == 0 &&
                   (present & HAVE_CONTROL_PORT) == 0U &&
                   parse_port(field, &value.control_port)) {
            present |= HAVE_CONTROL_PORT;
        } else if (strcmp(key, "media_port") == 0 &&
                   (present & HAVE_MEDIA_PORT) == 0U &&
                   parse_port(field, &value.media_port)) {
            present |= HAVE_MEDIA_PORT;
        } else if (strcmp(key, "interface") == 0 &&
                   (present & HAVE_INTERFACE) == 0U &&
                   meiso_k230d_composition_edge_service_private_copy_text(
                       value.interface_name,
                       sizeof(value.interface_name),
                       field
                   )) {
            present |= HAVE_INTERFACE;
        } else if (strcmp(key, "local_edge_peer_id_high") == 0 &&
                   (present & HAVE_LOCAL_HIGH) == 0U &&
                   parse_u64(field, &value.local_edge_peer_id.high)) {
            present |= HAVE_LOCAL_HIGH;
        } else if (strcmp(key, "local_edge_peer_id_low") == 0 &&
                   (present & HAVE_LOCAL_LOW) == 0U &&
                   parse_u64(field, &value.local_edge_peer_id.low)) {
            present |= HAVE_LOCAL_LOW;
        } else if (strcmp(key, "expected_host_peer_id_high") == 0 &&
                   (present & HAVE_HOST_HIGH) == 0U &&
                   parse_u64(field, &value.expected_host_peer_id.high)) {
            present |= HAVE_HOST_HIGH;
        } else if (strcmp(key, "expected_host_peer_id_low") == 0 &&
                   (present & HAVE_HOST_LOW) == 0U &&
                   parse_u64(field, &value.expected_host_peer_id.low)) {
            present |= HAVE_HOST_LOW;
        } else if (strcmp(key, "local_incarnation") == 0 &&
                   (present & HAVE_LOCAL_INCARNATION) == 0U &&
                   parse_u64(field, &value.local_incarnation)) {
            present |= HAVE_LOCAL_INCARNATION;
        } else if (strcmp(key, "catalog_generation") == 0 &&
                   (present & HAVE_CATALOG) == 0U &&
                   parse_u64(field, &value.catalog_generation)) {
            present |= HAVE_CATALOG;
        } else if (strcmp(key, "endpoint_incarnation_base") == 0 &&
                   (present & HAVE_ENDPOINT_BASE) == 0U &&
                   parse_u64(field, &value.endpoint_incarnation_base)) {
            present |= HAVE_ENDPOINT_BASE;
        } else {
            valid = false;
        }
    }
    {
        const bool read_failed = ferror(file) != 0;
        const bool close_failed = fclose(file) != 0;
        if (read_failed || close_failed) {
            return false;
        }
    }
    if (!valid || present != HAVE_ALL ||
        value.control_port == value.media_port ||
        value.local_incarnation == 0U || value.catalog_generation == 0U ||
        value.endpoint_incarnation_base == 0U ||
        value.endpoint_incarnation_base > UINT64_MAX - (ENDPOINT_COUNT - 1U) ||
        !meiso_peer_id_valid(value.local_edge_peer_id) ||
        !meiso_peer_id_valid(value.expected_host_peer_id) ||
        meiso_peer_id_equal(
            value.local_edge_peer_id, value.expected_host_peer_id
        ) || inet_pton(
            AF_INET, value.listen_ipv4, &(struct in_addr){0}
        ) != 1) {
        return false;
    }
    *out_config = value;
    return true;
}
