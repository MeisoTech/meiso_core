#include "composition_coordinator.h"
#include "composition_reference_renderer.h"
#include "composition_scene.h"
#include "meiso_blob.h"
#include "scene_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                      \
                __FILE__, __LINE__, #condition                                  \
            );                                                                  \
            return false;                                                       \
        }                                                                       \
    } while (0)

#ifndef MEISO_OFFSCREEN_RUNNER_SOURCE_PATH
#error "runner source path is required"
#endif

#ifndef MEISO_OFFSCREEN_CROSS_SCRIPT_PATH
#error "cross script path is required"
#endif

enum {
    RECEIPT_LINE_COUNT = 10U,
    RECEIPT_LINE_CAPACITY = 768U,
    RECEIPT_CAPACITY = 8192U,
    RUNNER_SCRATCH_BYTES_64 = 14720U
};

int meiso_composition_offscreen_runner_run(
    FILE *output,
    FILE *error_output
);

typedef struct {
    int status;
    char output[RECEIPT_CAPACITY];
    size_t output_size;
    char error[256];
    size_t error_size;
} CapturedRun;

static bool read_stream(
    FILE *stream,
    char *output,
    size_t capacity,
    size_t *out_size
)
{
    size_t size;

    if (stream == NULL || output == NULL || capacity == 0U ||
        out_size == NULL || fflush(stream) != 0 || fseek(stream, 0L, SEEK_SET) !=
            0) {
        return false;
    }
    size = fread(output, 1U, capacity - 1U, stream);
    if (ferror(stream) != 0 || !feof(stream)) {
        return false;
    }
    output[size] = '\0';
    *out_size = size;
    return true;
}

static bool capture_success_run(CapturedRun *captured)
{
    FILE *output = NULL;
    FILE *error_output = NULL;
    bool okay = false;

    if (captured == NULL) {
        return false;
    }
    memset(captured, 0, sizeof(*captured));
    output = tmpfile();
    error_output = tmpfile();
    if (output == NULL || error_output == NULL) {
        goto cleanup;
    }
    captured->status = meiso_composition_offscreen_runner_run(
        output, error_output
    );
    if (!read_stream(
            output,
            captured->output,
            sizeof(captured->output),
            &captured->output_size
        ) ||
        !read_stream(
            error_output,
            captured->error,
            sizeof(captured->error),
            &captured->error_size
        )) {
        goto cleanup;
    }
    okay = true;

cleanup:
    if (error_output != NULL) {
        (void)fclose(error_output);
    }
    if (output != NULL) {
        (void)fclose(output);
    }
    return okay;
}

static bool split_receipt(
    const CapturedRun *captured,
    char lines[RECEIPT_LINE_COUNT][RECEIPT_LINE_CAPACITY]
)
{
    size_t offset = 0U;
    size_t count = 0U;

    if (captured == NULL || captured->output_size == 0U ||
        captured->output[captured->output_size - 1U] != '\n') {
        return false;
    }
    while (offset < captured->output_size) {
        const char *start = captured->output + offset;
        const char *newline = (const char *)memchr(
            start, '\n', captured->output_size - offset
        );
        size_t length;

        if (newline == NULL || count >= RECEIPT_LINE_COUNT) {
            return false;
        }
        length = (size_t)(newline - start);
        if (length == 0U || length >= RECEIPT_LINE_CAPACITY ||
            memchr(start, '\r', length) != NULL) {
            return false;
        }
        memcpy(lines[count], start, length);
        lines[count][length] = '\0';
        ++count;
        offset += length + 1U;
    }
    return count == RECEIPT_LINE_COUNT;
}

static bool parse_arena_line(
    const char *line,
    const char *expected_name,
    const char *expected_domain,
    size_t expected_memory,
    size_t expected_alignment,
    size_t expected_subtotal
)
{
    char name[64];
    char domain[64];
    size_t memory_required = 0U;
    size_t alignment = 0U;
    size_t subtotal = 0U;
    int consumed = -1;

    memset(name, 0, sizeof(name));
    memset(domain, 0, sizeof(domain));
    return sscanf(
               line,
               "{\"type\":\"arena\",\"name\":\"%63[^\"]\","
               "\"memory_required\":%zu,\"alignment\":%zu,"
               "\"domain\":\"%63[^\"]\",\"subtotal\":%zu}%n",
               name,
               &memory_required,
               &alignment,
               domain,
               &subtotal,
               &consumed
           ) == 5 && consumed >= 0 && (size_t)consumed == strlen(line) &&
        strcmp(name, expected_name) == 0 &&
        strcmp(domain, expected_domain) == 0 &&
        memory_required == expected_memory &&
        alignment == expected_alignment && subtotal == expected_subtotal;
}

static bool verify_logical_mesh_line(const char *line)
{
    uint8_t cube_storage[512];
    MeisoCompositionSceneMeshView mesh;
    size_t cube_size = 0U;
    unsigned published_count = 0U;
    size_t published_bytes = 0U;
    uint64_t expected_resident_bytes;
    unsigned long long resident_bytes = 0U;
    int consumed = -1;

    CHECK(meiso_composition_scene_mesh_encode(
        meiso_composition_scene_canonical_cube(),
        (MeisoMutableBuffer){cube_storage, sizeof(cube_storage)},
        &cube_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(meiso_composition_scene_mesh_decode(
        (MeisoByteView){cube_storage, cube_size}, &mesh
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    expected_resident_bytes =
        mesh.vertex_records.size + mesh.index_records.size;
    CHECK(sscanf(
        line,
        "{\"type\":\"logical_mesh\",\"published_blob_count\":%u,"
        "\"published_blob_bytes\":%zu,\"resident_mesh_bytes\":%llu,"
        "\"mesh_header_in_resident\":false}%n",
        &published_count,
        &published_bytes,
        &resident_bytes,
        &consumed
    ) == 3);
    return consumed >= 0 && (size_t)consumed == strlen(line) &&
        published_count == 1U && published_bytes == cube_size &&
        resident_bytes == (unsigned long long)expected_resident_bytes;
}

static bool verify_receipt(const CapturedRun *captured)
{
    static const char *const arena_names[5] = {
        "blob_store", "scene_store", "coordinator", "reference_renderer",
        "runner_scratch"
    };
    static const char *const arena_domains[5] = {
        "cpu_heap_core_blob", "cpu_heap_profile", "cpu_heap_profile",
        "cpu_heap_reference_targets", "cpu_heap_runner"
    };
    static const char render_line[] =
        "{\"type\":\"render\",\"flags\":3,\"objects\":1,"
        "\"unique_meshes\":1,\"target_slots\":2,"
        "\"target_bytes\":1228800,"
        "\"color_sha256\":"
        "\"3fd397c5e259c43d5ec480f9b86a7c86e94fb239d03404f330b88e690c0aee65\","
        "\"depth_sha256\":"
        "\"6d9c957af7b2500b309a425d8cd4314aea3f99348f5d1699b96315a1772faaae\","
        "\"thumbnail_sha256\":"
        "\"07f056872d92a2335e0d3f1da675bf02892238c2f8b7e27c24e85df5a4cddb91\"}";
    static const char limitations_line[] =
        "{\"type\":\"limitations\",\"network\":false,"
        "\"driver\":false,\"stream\":false,\"runtime\":false,"
        "\"session\":false,\"state\":false,\"scheduler\":false,"
        "\"offline_lease_execution\":false,"
        "\"full_lifecycle\":false,"
        "\"geometry_correct\":false,\"physical\":false,"
        "\"performance\":false}";
    static const char lifecycle_line[] =
        "{\"type\":\"allocator_lifecycle\","
        "\"scope\":\"runner_checked_allocator\","
        "\"allocations_at_enable\":5,\"frees_at_enable\":0,"
        "\"allocation_calls\":5,\"successful_allocations\":5,"
        "\"successful_allocations_after_enable\":0,"
        "\"allocation_attempts_after_enable\":0,\"realloc_calls\":0,"
        "\"free_calls\":5,\"frees_after_enable\":5}";
    const MeisoResourceDescriptor *blob_descriptor =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
        );
    const MeisoBlobConfig blob_config = {
        MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS
    };
    const size_t memory_required[5] = {
        meiso_blob_store_memory_required(blob_descriptor, &blob_config),
        meiso_composition_scene_store_memory_required(),
        meiso_composition_coordinator_memory_required(),
        meiso_composition_reference_renderer_memory_required(),
        RUNNER_SCRATCH_BYTES_64
    };
    const size_t alignment[5] = {
        meiso_blob_alignment_required(),
        meiso_composition_scene_store_alignment_required(),
        meiso_composition_coordinator_alignment_required(),
        meiso_composition_reference_renderer_alignment_required(),
        _Alignof(max_align_t)
    };
    char lines[RECEIPT_LINE_COUNT][RECEIPT_LINE_CAPACITY];
    size_t subtotal = 0U;
    size_t index;

    CHECK(captured != NULL && captured->status == 0 &&
          captured->error_size == 0U && captured->error[0] == '\0' &&
          captured->output_size < sizeof(captured->output) &&
          strstr(captured->output, "TARGET") == NULL &&
          strchr(captured->output, '/') == NULL &&
          strchr(captured->output, '\\') == NULL &&
          strstr(captured->output, "password") == NULL &&
          strstr(captured->output, "secret") == NULL &&
          strstr(captured->output, "prepared_token") == NULL &&
          split_receipt(captured, lines));
    CHECK(strcmp(
        lines[0],
        "{\"type\":\"composition_offscreen\",\"schema\":1,"
        "\"verification\":\"HOST_OFFSCREEN_REFERENCE_VERIFIED\","
        "\"build_target\":\"native_host\","
        "\"profile\":\"composition-scene-lab-v0\","
        "\"scenario\":\"baseline_cube_once\"}"
    ) == 0);
    for (index = 0U; index < 5U; ++index) {
        CHECK(memory_required[index] != 0U && alignment[index] != 0U &&
              subtotal <= SIZE_MAX - memory_required[index]);
        subtotal += memory_required[index];
        CHECK(parse_arena_line(
            lines[index + 1U],
            arena_names[index],
            arena_domains[index],
            memory_required[index],
            alignment[index],
            subtotal
        ));
    }
    CHECK(verify_logical_mesh_line(lines[6]) &&
          strcmp(lines[7], render_line) == 0 &&
          strcmp(lines[8], limitations_line) == 0 &&
          strcmp(lines[9], lifecycle_line) == 0);
    return true;
}

static bool read_path(
    const char *path,
    char *output,
    size_t capacity,
    size_t *out_size
)
{
    FILE *file = NULL;
    size_t size;
    bool okay = false;

    if (path == NULL || output == NULL || capacity == 0U ||
        out_size == NULL) {
        return false;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    size = fread(output, 1U, capacity - 1U, file);
    if (ferror(file) == 0 && feof(file)) {
        output[size] = '\0';
        *out_size = size;
        okay = true;
    }
    (void)fclose(file);
    return okay;
}

static bool test_repeatable_native_receipt(void)
{
    CapturedRun first;
    CapturedRun second;
    char original_directory[4096];
    bool second_captured;

    CHECK(getcwd(original_directory, sizeof(original_directory)) != NULL);
    CHECK(capture_success_run(&first));
    CHECK(chdir("/tmp") == 0);
    second_captured = capture_success_run(&second);
    CHECK(chdir(original_directory) == 0);
    CHECK(second_captured && first.output_size == second.output_size &&
          first.error_size == second.error_size &&
          memcmp(first.output, second.output, first.output_size + 1U) == 0 &&
          memcmp(first.error, second.error, first.error_size + 1U) == 0 &&
          strstr(first.output, original_directory) == NULL &&
          verify_receipt(&first) && verify_receipt(&second));
    return true;
}

static bool test_argument_and_writer_failures(void)
{
    FILE *shared = NULL;
    FILE *error_output = NULL;
    FILE *bad_output = NULL;
    char message[64];
    size_t message_size = 0U;
    int status;
    bool okay = false;

    shared = tmpfile();
    if (shared == NULL) {
        goto cleanup;
    }
    status = meiso_composition_offscreen_runner_run(shared, shared);
    if (status != 2 ||
        !read_stream(shared, message, sizeof(message), &message_size) ||
        message_size != strlen("E_ARGUMENT\n") ||
        strcmp(message, "E_ARGUMENT\n") != 0) {
        goto cleanup;
    }
    (void)fclose(shared);
    shared = NULL;

    error_output = tmpfile();
    if (error_output == NULL) {
        goto cleanup;
    }
    status = meiso_composition_offscreen_runner_run(NULL, error_output);
    if (status != 2 ||
        !read_stream(
            error_output, message, sizeof(message), &message_size
        ) ||
        strcmp(message, "E_ARGUMENT\n") != 0) {
        goto cleanup;
    }
    (void)fclose(error_output);
    error_output = tmpfile();
    bad_output = fopen("/dev/null", "rb");
    if (error_output == NULL || bad_output == NULL) {
        goto cleanup;
    }
    status = meiso_composition_offscreen_runner_run(
        bad_output, error_output
    );
    if (status != 4 ||
        !read_stream(
            error_output, message, sizeof(message), &message_size
        ) ||
        strcmp(message, "E_WRITE\n") != 0) {
        goto cleanup;
    }
    okay = true;

cleanup:
    if (bad_output != NULL) {
        (void)fclose(bad_output);
    }
    if (error_output != NULL) {
        (void)fclose(error_output);
    }
    if (shared != NULL) {
        (void)fclose(shared);
    }
    return okay;
}

static bool test_cross_script_is_build_only(void)
{
    char script[8192];
    char runner_source[65536];
    size_t script_size = 0U;
    size_t source_size = 0U;
    static const char script_suffix[] = "    -o \"$output\"\n";

    CHECK(read_path(
        MEISO_OFFSCREEN_CROSS_SCRIPT_PATH,
        script,
        sizeof(script),
        &script_size
    ));
    CHECK(read_path(
        MEISO_OFFSCREEN_RUNNER_SOURCE_PATH,
        runner_source,
        sizeof(runner_source),
        &source_size
    ));
    CHECK(script_size >= sizeof(script_suffix) - 1U &&
          memcmp(
              script + script_size - (sizeof(script_suffix) - 1U),
              script_suffix,
              sizeof(script_suffix) - 1U
          ) == 0 &&
          strstr(script, "set -eu") != NULL &&
          strstr(script, "${MEISO_K230_LINUX_CC:-}") != NULL &&
          strstr(script, "\"$MEISO_K230_LINUX_CC\"") != NULL &&
          strstr(script, "-DMEISO_K230_LINUX_TARGET=1") != NULL &&
          strstr(script, "-march=rv64gcv_xtheadc") != NULL &&
          strstr(script, "composition_offscreen_runner.c") != NULL &&
          strstr(script, "qemu") == NULL && strstr(script, "ssh") == NULL &&
          strstr(script, "scp") == NULL && strstr(script, "\ncc ") == NULL &&
          strstr(script, "\ngcc ") == NULL &&
          strstr(script, "\nclang ") == NULL &&
          source_size != 0U &&
          strstr(runner_source, "TARGET_OFFSCREEN_REFERENCE_VERIFIED") ==
              NULL &&
          strstr(runner_source, "OFFSCREEN_REFERENCE_VECTOR_PASSED") !=
              NULL &&
          strstr(runner_source, "k230d_linux_candidate") != NULL);
    return true;
}

int main(void)
{
    if (!test_repeatable_native_receipt() ||
        !test_argument_and_writer_failures() ||
        !test_cross_script_is_build_only()) {
        return 1;
    }
    (void)puts("composition offscreen runner tests passed");
    return 0;
}
