#include "meiso_composition_cpu_raster_renderer_private.h"

#include <fenv.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Geometry owns camera transforms, clipping, coverage and depth writes. */

static bool add_checked(uint64_t *value, uint64_t addition)
{
    if (addition > UINT64_MAX - *value) {
        return false;
    }
    *value += addition;
    return true;
}

static bool expected_mesh_size(const MeisoCompositionCpuRasterRendererJob *job,
                               const MeisoContentId *content_id,
                               uint32_t *out_size)
{
    uint32_t index;

    for (index = 0U; index < job->unique_mesh_count; ++index) {
        if (meiso_composition_cpu_raster_renderer_private_content_id_equal(
                content_id, &job->unique_meshes[index])) {
            *out_size = job->unique_mesh_sizes[index];
            return true;
        }
    }
    return false;
}

static bool finite_vector(const double *values, size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (!isfinite(values[index])) {
            return false;
        }
    }
    return true;
}

static void rotate_vector(const double quaternion[4], const double input[3],
                          double output[3])
{
    const double tx =
        2.0 * (quaternion[1] * input[2] - quaternion[2] * input[1]);
    const double ty =
        2.0 * (quaternion[2] * input[0] - quaternion[0] * input[2]);
    const double tz =
        2.0 * (quaternion[0] * input[1] - quaternion[1] * input[0]);

    output[0] =
        input[0] + quaternion[3] * tx + quaternion[1] * tz - quaternion[2] * ty;
    output[1] =
        input[1] + quaternion[3] * ty + quaternion[2] * tx - quaternion[0] * tz;
    output[2] =
        input[2] + quaternion[3] * tz + quaternion[0] * ty - quaternion[1] * tx;
}

bool meiso_composition_cpu_raster_renderer_private_prepare_camera(
    const MeisoCompositionSceneCamera *source,
    MeisoCompositionCpuRasterCamera *out_camera)
{
    static const double degrees_to_radians =
        0.017453292519943295769236907684886;
    const double half_fov =
        (double)source->vertical_fov_degrees * degrees_to_radians * 0.5;
    const double tangent = tan(half_fov);
    const double near_z = (double)source->near_z;
    const double far_z = (double)source->far_z;
    size_t index;

    memset(out_camera, 0, sizeof(*out_camera));
    if (!isfinite(tangent) || tangent <= 0.0 || !isfinite(near_z) ||
        !isfinite(far_z) || near_z <= 0.0 || far_z <= near_z) {
        return false;
    }
    for (index = 0U; index < 3U; ++index) {
        out_camera->position[index] = source->position[index];
        out_camera->orientation_xyzw[index] =
            -(double)source->orientation_xyzw[index];
    }
    out_camera->orientation_xyzw[3] = source->orientation_xyzw[3];
    out_camera->focal_y = 1.0 / tangent;
    out_camera->focal_x =
        out_camera->focal_y * ((double)MEISO_COMPOSITION_SCENE_TARGET_HEIGHT /
                               (double)MEISO_COMPOSITION_SCENE_TARGET_WIDTH);
    out_camera->projection_a = (far_z + near_z) / (near_z - far_z);
    out_camera->projection_b = (2.0 * far_z * near_z) / (near_z - far_z);
    return finite_vector(out_camera->position, 3U) &&
           finite_vector(out_camera->orientation_xyzw, 4U) &&
           isfinite(out_camera->focal_x) && isfinite(out_camera->focal_y) &&
           isfinite(out_camera->projection_a) &&
           isfinite(out_camera->projection_b);
}

static bool make_clip_vertex(const MeisoCompositionCpuRasterCamera *camera,
                             const MeisoCompositionSceneObject *object,
                             const MeisoCompositionSceneMeshVertex *vertex,
                             bool has_vertex_color,
                             MeisoCompositionCpuRasterClipVertex *out_vertex)
{
    double object_quaternion[4];
    double local[3];
    double world_offset[3];
    double world[3];
    double relative[3];
    double view[3];
    size_t index;

    for (index = 0U; index < 3U; ++index) {
        local[index] =
            (double)vertex->position[index] * object->transform.scale[index];
        object_quaternion[index] = object->transform.orientation_xyzw[index];
    }
    object_quaternion[3] = object->transform.orientation_xyzw[3];
    if (!finite_vector(local, 3U) || !finite_vector(object_quaternion, 4U)) {
        return false;
    }
    rotate_vector(object_quaternion, local, world_offset);
    for (index = 0U; index < 3U; ++index) {
        world[index] = world_offset[index] + object->transform.position[index];
        relative[index] = world[index] - camera->position[index];
    }
    rotate_vector(camera->orientation_xyzw, relative, view);
    out_vertex->clip[0] = camera->focal_x * view[0];
    out_vertex->clip[1] = camera->focal_y * view[1];
    out_vertex->clip[2] = camera->projection_a * view[2] + camera->projection_b;
    out_vertex->clip[3] = -view[2];
    for (index = 0U; index < 4U; ++index) {
        out_vertex->color[index] =
            has_vertex_color ? (double)vertex->color_rgba[index] : 255.0;
    }
    return finite_vector(out_vertex->clip, 4U) &&
           finite_vector(out_vertex->color, 4U);
}

static double clip_distance(const MeisoCompositionCpuRasterClipVertex *vertex,
                            uint32_t plane)
{
    switch (plane) {
    case 0U:
        return vertex->clip[0] + vertex->clip[3];
    case 1U:
        return vertex->clip[3] - vertex->clip[0];
    case 2U:
        return vertex->clip[1] + vertex->clip[3];
    case 3U:
        return vertex->clip[3] - vertex->clip[1];
    case 4U:
        return vertex->clip[2] + vertex->clip[3];
    default:
        return vertex->clip[3] - vertex->clip[2];
    }
}

static bool
interpolate_clip_vertex(const MeisoCompositionCpuRasterClipVertex *start,
                        const MeisoCompositionCpuRasterClipVertex *end,
                        double start_distance, double end_distance,
                        MeisoCompositionCpuRasterClipVertex *out_vertex)
{
    const double denominator = start_distance - end_distance;
    double t;
    size_t index;

    if (!isfinite(denominator) || denominator == 0.0) {
        return false;
    }
    t = start_distance / denominator;
    if (!isfinite(t)) {
        return false;
    }
    if (t < 0.0) {
        t = 0.0;
    } else if (t > 1.0) {
        t = 1.0;
    }
    for (index = 0U; index < 4U; ++index) {
        out_vertex->clip[index] =
            start->clip[index] + t * (end->clip[index] - start->clip[index]);
        out_vertex->color[index] =
            start->color[index] + t * (end->color[index] - start->color[index]);
    }
    return finite_vector(out_vertex->clip, 4U) &&
           finite_vector(out_vertex->color, 4U);
}

static bool
clip_triangle(const MeisoCompositionCpuRasterClipVertex input[3],
              MeisoCompositionCpuRasterClipVertex
                  output[MEISO_COMPOSITION_CPU_RASTER_CLIP_VERTEX_CAPACITY],
              uint32_t *out_count, bool *out_changed)
{
    MeisoCompositionCpuRasterClipVertex
        first[MEISO_COMPOSITION_CPU_RASTER_CLIP_VERTEX_CAPACITY];
    MeisoCompositionCpuRasterClipVertex
        second[MEISO_COMPOSITION_CPU_RASTER_CLIP_VERTEX_CAPACITY];
    MeisoCompositionCpuRasterClipVertex *current = first;
    MeisoCompositionCpuRasterClipVertex *next = second;
    uint32_t current_count = 3U;
    uint32_t plane;
    bool changed = false;

    memcpy(first, input, sizeof(MeisoCompositionCpuRasterClipVertex) * 3U);
    for (plane = 0U; plane < MEISO_COMPOSITION_CPU_RASTER_CLIP_PLANE_COUNT;
         ++plane) {
        uint32_t next_count = 0U;
        uint32_t index;

        if (current_count == 0U) {
            break;
        }
        for (index = 0U; index < current_count; ++index) {
            const MeisoCompositionCpuRasterClipVertex *start =
                &current[(index + current_count - 1U) % current_count];
            const MeisoCompositionCpuRasterClipVertex *end = &current[index];
            const double start_distance = clip_distance(start, plane);
            const double end_distance = clip_distance(end, plane);
            const bool start_inside = start_distance >= 0.0;
            const bool end_inside = end_distance >= 0.0;

            if (!isfinite(start_distance) || !isfinite(end_distance)) {
                return false;
            }
            if (start_inside != end_inside) {
                if (next_count >=
                        MEISO_COMPOSITION_CPU_RASTER_CLIP_VERTEX_CAPACITY ||
                    !interpolate_clip_vertex(start, end, start_distance,
                                             end_distance, &next[next_count])) {
                    return false;
                }
                ++next_count;
                changed = true;
            }
            if (end_inside) {
                if (next_count >=
                    MEISO_COMPOSITION_CPU_RASTER_CLIP_VERTEX_CAPACITY) {
                    return false;
                }
                next[next_count++] = *end;
            } else {
                changed = true;
            }
        }
        current_count = next_count;
        {
            MeisoCompositionCpuRasterClipVertex *temporary = current;
            current = next;
            next = temporary;
        }
    }
    if (current_count > MEISO_COMPOSITION_CPU_RASTER_CLIP_VERTEX_CAPACITY) {
        return false;
    }
    for (plane = 0U; plane < current_count; ++plane) {
        if (!finite_vector(current[plane].clip, 4U) ||
            !finite_vector(current[plane].color, 4U) ||
            current[plane].clip[3] <= 0.0) {
            return false;
        }
    }
    memcpy(output, current,
           sizeof(MeisoCompositionCpuRasterClipVertex) * current_count);
    *out_count = current_count;
    *out_changed = changed;
    return true;
}

static bool round_even_nonnegative(double value, uint64_t maximum,
                                   uint64_t *out_value)
{
    double base_double;
    double fraction;
    uint64_t base;

    if (!isfinite(value) || value < 0.0 || value > (double)maximum) {
        return false;
    }
    base_double = floor(value);
    base = (uint64_t)base_double;
    fraction = value - base_double;
    if (fraction > 0.5 || (fraction == 0.5 && (base & UINT64_C(1)) != 0U)) {
        if (base == maximum) {
            return false;
        }
        ++base;
    }
    *out_value = base;
    return true;
}

static bool
make_screen_vertex(const MeisoCompositionCpuRasterClipVertex *clip,
                   MeisoCompositionCpuRasterScreenVertex *out_vertex)
{
    double x;
    double y;
    double z;
    double screen_x;
    double screen_y;
    uint64_t fixed_x;
    uint64_t fixed_y;
    size_t index;

    if (clip->clip[3] <= 0.0 || !isfinite(clip->clip[3])) {
        return false;
    }
    out_vertex->inverse_w = 1.0 / clip->clip[3];
    x = clip->clip[0] * out_vertex->inverse_w;
    y = clip->clip[1] * out_vertex->inverse_w;
    z = clip->clip[2] * out_vertex->inverse_w;
    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) {
        return false;
    }
    if (x < -1.0) {
        x = -1.0;
    } else if (x > 1.0) {
        x = 1.0;
    }
    if (y < -1.0) {
        y = -1.0;
    } else if (y > 1.0) {
        y = 1.0;
    }
    if (z < -1.0) {
        z = -1.0;
    } else if (z > 1.0) {
        z = 1.0;
    }
    screen_x = (x * 0.5 + 0.5) * (double)MEISO_COMPOSITION_SCENE_TARGET_WIDTH *
               (double)MEISO_COMPOSITION_CPU_RASTER_FIXED_SUBPIXEL_SCALE;
    screen_y = (0.5 - y * 0.5) * (double)MEISO_COMPOSITION_SCENE_TARGET_HEIGHT *
               (double)MEISO_COMPOSITION_CPU_RASTER_FIXED_SUBPIXEL_SCALE;
    if (!round_even_nonnegative(
            screen_x,
            (uint64_t)MEISO_COMPOSITION_SCENE_TARGET_WIDTH *
                MEISO_COMPOSITION_CPU_RASTER_FIXED_SUBPIXEL_SCALE,
            &fixed_x) ||
        !round_even_nonnegative(
            screen_y,
            (uint64_t)MEISO_COMPOSITION_SCENE_TARGET_HEIGHT *
                MEISO_COMPOSITION_CPU_RASTER_FIXED_SUBPIXEL_SCALE,
            &fixed_y) ||
        fixed_x > INT32_MAX || fixed_y > INT32_MAX) {
        return false;
    }
    out_vertex->x_fixed = (int32_t)fixed_x;
    out_vertex->y_fixed = (int32_t)fixed_y;
    out_vertex->ndc_x = x;
    out_vertex->ndc_y = y;
    out_vertex->ndc_z = z;
    for (index = 0U; index < 4U; ++index) {
        out_vertex->color_over_w[index] =
            clip->color[index] * out_vertex->inverse_w;
    }
    return isfinite(out_vertex->inverse_w) &&
           finite_vector(out_vertex->color_over_w, 4U);
}

static int64_t edge_value(const MeisoCompositionCpuRasterScreenVertex *start,
                          const MeisoCompositionCpuRasterScreenVertex *end,
                          int64_t x, int64_t y)
{
    return ((int64_t)end->x_fixed - start->x_fixed) * (y - start->y_fixed) -
           ((int64_t)end->y_fixed - start->y_fixed) * (x - start->x_fixed);
}

static bool edge_top_left(const MeisoCompositionCpuRasterScreenVertex *start,
                          const MeisoCompositionCpuRasterScreenVertex *end)
{
    const int32_t dx = end->x_fixed - start->x_fixed;
    const int32_t dy = end->y_fixed - start->y_fixed;

    return dy < 0 || (dy == 0 && dx > 0);
}

static int64_t floor_div_fixed(int64_t value)
{
    return value >= 0
               ? value / MEISO_COMPOSITION_CPU_RASTER_FIXED_SUBPIXEL_SCALE
               : -((-value + MEISO_COMPOSITION_CPU_RASTER_FIXED_SUBPIXEL_SCALE -
                    1) /
                   MEISO_COMPOSITION_CPU_RASTER_FIXED_SUBPIXEL_SCALE);
}

static int64_t ceil_div_fixed(int64_t value)
{
    return -floor_div_fixed(-value);
}

static bool
prepare_screen_triangle(const MeisoCompositionCpuRasterClipVertex *clip_a,
                        const MeisoCompositionCpuRasterClipVertex *clip_b,
                        const MeisoCompositionCpuRasterClipVertex *clip_c,
                        MeisoCompositionCpuRasterScreenVertex output[3],
                        bool *out_culled)
{
    const double inverse_a = 1.0 / clip_a->clip[3];
    const double inverse_b = 1.0 / clip_b->clip[3];
    const double inverse_c = 1.0 / clip_c->clip[3];
    const double ax = clip_a->clip[0] * inverse_a;
    const double ay = clip_a->clip[1] * inverse_a;
    const double bx = clip_b->clip[0] * inverse_b;
    const double by = clip_b->clip[1] * inverse_b;
    const double cx = clip_c->clip[0] * inverse_c;
    const double cy = clip_c->clip[1] * inverse_c;
    const double ndc_area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    int64_t fixed_area;

    *out_culled = false;
    if (!isfinite(ndc_area)) {
        return false;
    }
    if (ndc_area <= 0.0) {
        *out_culled = true;
        return true;
    }
    if (!make_screen_vertex(clip_a, &output[0]) ||
        !make_screen_vertex(clip_b, &output[1]) ||
        !make_screen_vertex(clip_c, &output[2])) {
        return false;
    }
    fixed_area = edge_value(&output[0], &output[1], output[2].x_fixed,
                            output[2].y_fixed);
    if (fixed_area == 0) {
        *out_culled = true;
        return true;
    }
    if (fixed_area < 0) {
        const MeisoCompositionCpuRasterScreenVertex temporary = output[1];
        output[1] = output[2];
        output[2] = temporary;
    }
    return true;
}

static bool edge_covered(int64_t value, bool top_left)
{
    return value > 0 || (value == 0 && top_left);
}

static void
rasterize_screen_triangle(const MeisoCompositionCpuRasterScreenVertex input[3],
                          uint8_t *color_target, uint8_t *depth_target,
                          uint64_t *in_out_fragments)
{
    const int32_t min_fixed_x =
        input[0].x_fixed < input[1].x_fixed
            ? (input[0].x_fixed < input[2].x_fixed ? input[0].x_fixed
                                                   : input[2].x_fixed)
            : (input[1].x_fixed < input[2].x_fixed ? input[1].x_fixed
                                                   : input[2].x_fixed);
    const int32_t max_fixed_x =
        input[0].x_fixed > input[1].x_fixed
            ? (input[0].x_fixed > input[2].x_fixed ? input[0].x_fixed
                                                   : input[2].x_fixed)
            : (input[1].x_fixed > input[2].x_fixed ? input[1].x_fixed
                                                   : input[2].x_fixed);
    const int32_t min_fixed_y =
        input[0].y_fixed < input[1].y_fixed
            ? (input[0].y_fixed < input[2].y_fixed ? input[0].y_fixed
                                                   : input[2].y_fixed)
            : (input[1].y_fixed < input[2].y_fixed ? input[1].y_fixed
                                                   : input[2].y_fixed);
    const int32_t max_fixed_y =
        input[0].y_fixed > input[1].y_fixed
            ? (input[0].y_fixed > input[2].y_fixed ? input[0].y_fixed
                                                   : input[2].y_fixed)
            : (input[1].y_fixed > input[2].y_fixed ? input[1].y_fixed
                                                   : input[2].y_fixed);
    int64_t min_x = ceil_div_fixed(
        (int64_t)min_fixed_x - MEISO_COMPOSITION_CPU_RASTER_FIXED_PIXEL_CENTER);
    int64_t max_x = floor_div_fixed(
        (int64_t)max_fixed_x - MEISO_COMPOSITION_CPU_RASTER_FIXED_PIXEL_CENTER);
    int64_t min_y = ceil_div_fixed(
        (int64_t)min_fixed_y - MEISO_COMPOSITION_CPU_RASTER_FIXED_PIXEL_CENTER);
    int64_t max_y = floor_div_fixed(
        (int64_t)max_fixed_y - MEISO_COMPOSITION_CPU_RASTER_FIXED_PIXEL_CENTER);
    const int64_t area =
        edge_value(&input[0], &input[1], input[2].x_fixed, input[2].y_fixed);
    const bool top_left_0 = edge_top_left(&input[1], &input[2]);
    const bool top_left_1 = edge_top_left(&input[2], &input[0]);
    const bool top_left_2 = edge_top_left(&input[0], &input[1]);
    int64_t y;

    if (min_x < 0) {
        min_x = 0;
    }
    if (min_y < 0) {
        min_y = 0;
    }
    if (max_x >= MEISO_COMPOSITION_SCENE_TARGET_WIDTH) {
        max_x = MEISO_COMPOSITION_SCENE_TARGET_WIDTH - 1U;
    }
    if (max_y >= MEISO_COMPOSITION_SCENE_TARGET_HEIGHT) {
        max_y = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT - 1U;
    }
    for (y = min_y; y <= max_y; ++y) {
        int64_t x;
        const int64_t sample_y =
            y * MEISO_COMPOSITION_CPU_RASTER_FIXED_SUBPIXEL_SCALE +
            MEISO_COMPOSITION_CPU_RASTER_FIXED_PIXEL_CENTER;

        for (x = min_x; x <= max_x; ++x) {
            const int64_t sample_x =
                x * MEISO_COMPOSITION_CPU_RASTER_FIXED_SUBPIXEL_SCALE +
                MEISO_COMPOSITION_CPU_RASTER_FIXED_PIXEL_CENTER;
            const int64_t edge0 =
                edge_value(&input[1], &input[2], sample_x, sample_y);
            const int64_t edge1 =
                edge_value(&input[2], &input[0], sample_x, sample_y);
            const int64_t edge2 =
                edge_value(&input[0], &input[1], sample_x, sample_y);
            double lambda0;
            double lambda1;
            double lambda2;
            double depth_normalized;
            uint64_t depth_value;
            size_t pixel;
            uint32_t old_depth;

            if (!edge_covered(edge0, top_left_0) ||
                !edge_covered(edge1, top_left_1) ||
                !edge_covered(edge2, top_left_2)) {
                continue;
            }
            lambda0 = (double)edge0 / (double)area;
            lambda1 = (double)edge1 / (double)area;
            lambda2 = (double)edge2 / (double)area;
            depth_normalized =
                (lambda0 * input[0].ndc_z + lambda1 * input[1].ndc_z +
                 lambda2 * input[2].ndc_z) *
                    0.5 +
                0.5;
            if (depth_normalized < 0.0) {
                depth_normalized = 0.0;
            } else if (depth_normalized > 1.0) {
                depth_normalized = 1.0;
            }
            if (!round_even_nonnegative(depth_normalized * 4294967294.0,
                                        UINT32_C(0xfffffffe), &depth_value)) {
                continue;
            }
            pixel =
                ((size_t)y * MEISO_COMPOSITION_SCENE_TARGET_WIDTH + (size_t)x) *
                4U;
            old_depth =
                meiso_composition_cpu_raster_renderer_private_read_be_u32(
                    depth_target + pixel);
            if ((uint32_t)depth_value < old_depth) {
                const double denominator = lambda0 * input[0].inverse_w +
                                           lambda1 * input[1].inverse_w +
                                           lambda2 * input[2].inverse_w;
                size_t channel;

                if (!isfinite(denominator) || denominator <= 0.0) {
                    continue;
                }
                for (channel = 0U; channel < 4U; ++channel) {
                    double value = (lambda0 * input[0].color_over_w[channel] +
                                    lambda1 * input[1].color_over_w[channel] +
                                    lambda2 * input[2].color_over_w[channel]) /
                                   denominator;
                    uint64_t rounded;

                    if (value < 0.0) {
                        value = 0.0;
                    } else if (value > 255.0) {
                        value = 255.0;
                    }
                    if (!round_even_nonnegative(value, 255U, &rounded)) {
                        rounded = 0U;
                    }
                    color_target[pixel + channel] = (uint8_t)rounded;
                }
                meiso_composition_cpu_raster_renderer_private_write_be_u32(
                    depth_target + pixel, (uint32_t)depth_value);
                if (*in_out_fragments != UINT64_MAX) {
                    ++*in_out_fragments;
                }
            }
        }
    }
}

MeisoCompositionCpuRasterRenderStatus
meiso_composition_cpu_raster_renderer_private_process_geometry(
    const MeisoCompositionCpuRasterRenderer *renderer,
    const MeisoCompositionCpuRasterRendererJob *job,
    const MeisoCompositionCpuRasterCamera *camera, bool draw,
    uint8_t *color_target, uint8_t *depth_target,
    MeisoCompositionCpuRasterWorkCounts *counts)
{
    uint32_t object_index;

    memset(counts, 0, sizeof(*counts));
    for (object_index = 0U; object_index < job->desired.object_count;
         ++object_index) {
        const MeisoCompositionSceneObject *object =
            &job->desired.objects[object_index];
        MeisoBlobView blob;
        MeisoCompositionSceneMeshView mesh;
        uint32_t expected_size;
        uint32_t index_offset;

        if ((object->flags & MEISO_COMPOSITION_SCENE_OBJECT_VISIBLE) == 0U) {
            continue;
        }
        if (!expected_mesh_size(job, &object->mesh_content_id,
                                &expected_size)) {
            return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_MESH;
        }
        if (!meiso_blob_store_lookup(renderer->blob_store,
                                     &object->mesh_content_id, &blob)) {
            return MEISO_COMPOSITION_CPU_RASTER_RENDER_MISSING_BLOB;
        }
        if (!meiso_composition_cpu_raster_renderer_private_content_id_equal(
                &object->mesh_content_id, &blob.content_id) ||
            blob.content.size != expected_size ||
            !meiso_composition_scene_mesh_blob_verify(&object->mesh_content_id,
                                                      blob.content, NULL) ||
            meiso_composition_scene_mesh_decode(blob.content, &mesh) !=
                MEISO_COMPOSITION_SCENE_CODEC_OK ||
            mesh.index_count % 3U != 0U) {
            return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_MESH;
        }
        for (index_offset = 0U; index_offset < mesh.index_count;
             index_offset += 3U) {
            uint32_t vertex_indices[3];
            MeisoCompositionSceneMeshVertex mesh_vertices[3];
            MeisoCompositionCpuRasterClipVertex triangle[3];
            MeisoCompositionCpuRasterClipVertex
                clipped[MEISO_COMPOSITION_CPU_RASTER_CLIP_VERTEX_CAPACITY];
            uint32_t clipped_count;
            bool changed;
            uint32_t vertex;
            uint32_t fan;

            if (!add_checked(&counts->submitted, 1U)) {
                return MEISO_COMPOSITION_CPU_RASTER_RENDER_COUNTER_EXHAUSTED;
            }
            for (vertex = 0U; vertex < 3U; ++vertex) {
                if (meiso_composition_scene_mesh_view_index(
                        &mesh, index_offset + vertex,
                        &vertex_indices[vertex]) !=
                        MEISO_COMPOSITION_SCENE_CODEC_OK ||
                    meiso_composition_scene_mesh_view_vertex(
                        &mesh, vertex_indices[vertex],
                        &mesh_vertices[vertex]) !=
                        MEISO_COMPOSITION_SCENE_CODEC_OK ||
                    !make_clip_vertex(camera, object, &mesh_vertices[vertex],
                                      mesh.has_vertex_color,
                                      &triangle[vertex])) {
                    return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_GEOMETRY;
                }
            }
            if (!clip_triangle(triangle, clipped, &clipped_count, &changed)) {
                return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_GEOMETRY;
            }
            if (changed && !add_checked(&counts->clipped, 1U)) {
                return MEISO_COMPOSITION_CPU_RASTER_RENDER_COUNTER_EXHAUSTED;
            }
            if (clipped_count < 3U) {
                continue;
            }
            if (clipped_count >
                MEISO_COMPOSITION_CPU_RASTER_CLIP_VERTEX_CAPACITY) {
                return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_GEOMETRY;
            }
            for (fan = 1U; fan + 1U < clipped_count; ++fan) {
                MeisoCompositionCpuRasterScreenVertex screen[3];
                bool culled;

                if (!prepare_screen_triangle(&clipped[0], &clipped[fan],
                                             &clipped[fan + 1U], screen,
                                             &culled)) {
                    return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_GEOMETRY;
                }
                if (culled) {
                    if (!add_checked(&counts->culled, 1U)) {
                        return MEISO_COMPOSITION_CPU_RASTER_RENDER_COUNTER_EXHAUSTED;
                    }
                    continue;
                }
                if (!add_checked(&counts->rasterized, 1U)) {
                    return MEISO_COMPOSITION_CPU_RASTER_RENDER_COUNTER_EXHAUSTED;
                }
                if (draw) {
                    rasterize_screen_triangle(screen, color_target,
                                              depth_target,
                                              &counts->fragments_depth_passed);
                }
            }
        }
    }
    return MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE;
}
