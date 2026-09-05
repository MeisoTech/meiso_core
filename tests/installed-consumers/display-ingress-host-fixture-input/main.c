#include <meiso/host/display_ingress_fixture_input.h>

#include <string.h>

int meiso_difi_cpp17_sha256(
    const uint8_t *payload,
    size_t payload_size,
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE]
);

int main(void)
{
    static const uint8_t PIXEL[4U] = {0x11U, 0x22U, 0x33U, 0xffU};
    static const uint8_t EXPECTED_SHA[32U] = {
        0x9aU, 0xadU, 0x15U, 0x2cU, 0xe3U, 0xaeU, 0x5cU, 0x9bU,
        0xa6U, 0xdcU, 0x6aU, 0x41U, 0x5dU, 0x01U, 0xeaU, 0x8cU,
        0xceU, 0x54U, 0x8fU, 0x33U, 0x24U, 0xc7U, 0x42U, 0xa8U,
        0xadU, 0x2cU, 0x97U, 0xbbU, 0x7fU, 0x13U, 0x31U, 0xb9U
    };
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];

    return meiso_difi_cpp17_sha256(PIXEL, sizeof(PIXEL), digest) == 0 &&
        memcmp(digest, EXPECTED_SHA, sizeof(digest)) == 0 ? 0 : 1;
}
