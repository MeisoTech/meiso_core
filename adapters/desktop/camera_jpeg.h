#ifndef MEISO_DESKTOP_CAMERA_JPEG_H
#define MEISO_DESKTOP_CAMERA_JPEG_H
#include <stddef.h>
#include <stdint.h>
/* Encode a 320x240 RGBA keyframe into caller-owned bounded storage.
 * Zero means invalid/oversize/codec failure; caller drops it without queuing. */
size_t meiso_desktop_camera_jpeg(const uint8_t *rgba, uint8_t *out, size_t capacity);
#endif
