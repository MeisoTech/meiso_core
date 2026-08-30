#define _POSIX_C_SOURCE 200809L

#include "meiso_wifi_runtime_host_private.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parses and validates the stable host CLI configuration contract. */

static bool parse_u64(const char *text, uint64_t maximum, uint64_t *out_value)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || out_value == NULL || text[0] == '\0' ||
        text[0] == '-' || text[0] == '+') {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        (uint64_t)value > maximum) {
        return false;
    }
    *out_value = (uint64_t)value;
    return true;
}
static char *trim(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        ++text;
    }
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' ||
            end[-1] == '\r' || end[-1] == '\n')) {
        --end;
    }
    *end = '\0';
    return text;
}

static bool parse_config_line(
    MeisoWifiRuntimeHostPrivateBootstrapConfig *config,
    uint32_t *seen,
    char *line
)
{
    char *separator;
    char *key;
    char *value;
    char *comment = strchr(line, '#');
    uint64_t number;
    uint32_t bit;

    if (comment != NULL) {
        *comment = '\0';
    }
    key = trim(line);
    if (*key == '\0') {
        return true;
    }
    separator = strchr(key, '=');
    if (separator == NULL || strchr(separator + 1, '=') != NULL) {
        return false;
    }
    *separator = '\0';
    value = trim(separator + 1);
    key = trim(key);
    if (strcmp(key, "connect_ipv4") == 0) {
        bit = UINT32_C(1) << 0U;
        if (inet_pton(AF_INET, value, &config->connect_address) != 1) {
            return false;
        }
    } else if (strcmp(key, "connect_port") == 0) {
        bit = UINT32_C(1) << 1U;
        if (!parse_u64(value, UINT16_MAX, &number) || number == 0U) {
            return false;
        }
        config->connect_port = (uint16_t)number;
    } else if (strcmp(key, "selected_fields") == 0) {
        bit = UINT32_C(1) << 2U;
        if (!parse_u64(value, UINT32_MAX, &number) || number == 0U ||
            ((uint32_t)number &
                ~(uint32_t)MEISO_WIFI_TELEMETRY_FIELD_ALL) != 0U) {
            return false;
        }
        config->desired.selected_fields = (uint32_t)number;
    } else if (strcmp(key, "sample_period_ms") == 0) {
        bit = UINT32_C(1) << 3U;
        if (!parse_u64(value, UINT32_MAX, &number)) {
            return false;
        }
        config->desired.sample_period_ms = (uint32_t)number;
    } else if (strcmp(key, "sample_count") == 0) {
        bit = UINT32_C(1) << 4U;
        if (!parse_u64(value, MEISO_WIFI_RUNTIME_HOST_PRIVATE_MAX_SAMPLES, &number) || number == 0U) {
            return false;
        }
        config->sample_count = (uint32_t)number;
    } else if (strcmp(key, "local_host_peer_id_high") == 0) {
        bit = UINT32_C(1) << 5U;
        if (!parse_u64(value, UINT64_MAX, &number)) {
            return false;
        }
        config->local_peer_id.high = number;
    } else if (strcmp(key, "local_host_peer_id_low") == 0) {
        bit = UINT32_C(1) << 6U;
        if (!parse_u64(value, UINT64_MAX, &number)) {
            return false;
        }
        config->local_peer_id.low = number;
    } else if (strcmp(key, "expected_edge_peer_id_high") == 0) {
        bit = UINT32_C(1) << 7U;
        if (!parse_u64(value, UINT64_MAX, &number)) {
            return false;
        }
        config->expected_edge_peer_id.high = number;
    } else if (strcmp(key, "expected_edge_peer_id_low") == 0) {
        bit = UINT32_C(1) << 8U;
        if (!parse_u64(value, UINT64_MAX, &number)) {
            return false;
        }
        config->expected_edge_peer_id.low = number;
    } else {
        return false;
    }
    if ((*seen & bit) != 0U) {
        return false;
    }
    *seen |= bit;
    return true;
}

bool meiso_wifi_runtime_host_private_load_config(
    const char *path,
    bool interactive,
    MeisoWifiRuntimeHostPrivateBootstrapConfig *out_config
)
{
    FILE *file;
    char line[MEISO_WIFI_RUNTIME_HOST_PRIVATE_CONFIG_LINE_BYTES];
    uint8_t encoded[MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE];
    uint32_t seen = 0U;
    size_t encoded_size = 0U;
    bool ok = true;

    if (path == NULL || out_config == NULL) {
        return false;
    }
    memset(out_config, 0, sizeof(*out_config));
    file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strchr(line, '\n') == NULL && !feof(file)) {
            ok = false;
            break;
        }
        if (!parse_config_line(out_config, &seen, line)) {
            ok = false;
            break;
        }
    }
    if (ferror(file) != 0) {
        ok = false;
    }
    (void)fclose(file);
    const uint32_t bootstrap_fields = UINT32_C(0x1e3);
    const uint32_t one_shot_fields = UINT32_C(0x01c);
    const uint32_t desired_fields = seen & one_shot_fields;
    const bool config_shape_valid = interactive ?
        (seen == bootstrap_fields ||
         seen == (bootstrap_fields | one_shot_fields)) :
        seen == (bootstrap_fields | one_shot_fields);

    return ok && config_shape_valid &&
        meiso_peer_id_valid(out_config->local_peer_id) &&
        meiso_peer_id_valid(out_config->expected_edge_peer_id) &&
        !meiso_peer_id_equal(
            out_config->local_peer_id,
            out_config->expected_edge_peer_id
        ) && (!interactive || desired_fields == 0U ||
            desired_fields == one_shot_fields) &&
        (desired_fields == 0U ||
            meiso_wifi_telemetry_desired_parameters_encode(
                &out_config->desired,
                (MeisoMutableBuffer){encoded, sizeof(encoded)},
                &encoded_size
            ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
}
