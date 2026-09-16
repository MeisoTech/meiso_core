#include "display_ingress_profile_private.h"
#include "../../src/core/meiso_sha256_private.h"

/* Preserve the fixture API's nonempty, bounded, nonaliasing digest contract. */
bool meiso_display_ingress_private_payload_sha256(
    MeisoByteView payload,
    uint8_t out_digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    const uintptr_t input = (uintptr_t)payload.data;
    const uintptr_t output = (uintptr_t)out_digest;
    MeisoSha256 sha;

    if (!meiso_byte_view_valid(payload) || payload.data == NULL ||
        payload.size == 0U ||
        payload.size > MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_BYTES ||
        out_digest == NULL || payload.size - 1U > UINTPTR_MAX - input ||
        MEISO_SHA256_DIGEST_SIZE - 1U > UINTPTR_MAX - output ||
        (input <= output + MEISO_SHA256_DIGEST_SIZE - 1U &&
         output <= input + payload.size - 1U)) {
        return false;
    }
    meiso_sha256_init(&sha);
    meiso_sha256_update(&sha, payload.data, payload.size);
    meiso_sha256_final(&sha, out_digest);
    return true;
}
