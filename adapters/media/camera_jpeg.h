#ifndef MEISO_CAMERA_JPEG_H
#define MEISO_CAMERA_JPEG_H
#include <stddef.h>
#include <stdint.h>
/* Encode a 320x240 RGBA keyframe into caller-owned bounded storage.
 * Zero means invalid/oversize/codec failure; caller drops it without queuing. */
size_t meiso_camera_jpeg_rgba(const uint8_t *rgba, uint8_t *out, size_t capacity);
/* NV12 interpretation must be explicitly known; this is not matrix discovery.
 * Uses row scratch only, never an additional full-frame RGBA copy. */
size_t meiso_camera_jpeg_nv12_bt601(const uint8_t *nv12, uint8_t *out, size_t capacity);
#endif
