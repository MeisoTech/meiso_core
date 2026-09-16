#include "../src/core/meiso_sha256_private.h"

#include <stdio.h>
#include <string.h>

/* Independent hashlib reference digests for bytes (index % 251). Cover both
 * padding branches, exact blocks, multiple blocks and incremental updates. */
static const struct {
    size_t size;
    const char *digest;
} vectors[] = {
    {0U, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {1U, "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d"},
    {55U, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
    {56U, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
    {63U, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
    {64U, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
    {65U, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
    {127U, "92ca0fa6651ee2f97b884b7246a562fa71250fedefe5ebf270d31c546bfea976"},
    {128U, "471fb943aa23c511f6f72f8d1652d9c880cfa392ad80503120547703e56a2be5"},
    {129U, "5099c6a56203f9687f7d33f4bfdf576d31dc91f6b695ecea38b2770c87631135"},
    {1025U, "bc0b6b10b89b9487a12fda2a8cc13194e7091c217aabf8b92846274026f4bcd0"},
};

static int matches(const uint8_t *digest, const char *expected)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < MEISO_SHA256_DIGEST_SIZE; ++i) {
        if (hex[digest[i] >> 4U] != expected[i * 2U] ||
            hex[digest[i] & 15U] != expected[i * 2U + 1U]) return 0;
    }
    return 1;
}

int main(void)
{
    uint8_t input[1025], digest[MEISO_SHA256_DIGEST_SIZE];
    MeisoSha256 sha;
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)(i % 251U);
    for (size_t v = 0; v < sizeof(vectors) / sizeof(vectors[0]); ++v) {
        const size_t size = vectors[v].size;
        for (size_t chunk = 1; chunk <= sizeof(input); chunk *= 2U) {
            meiso_sha256_init(&sha);
            meiso_sha256_update(&sha, NULL, 0);
            for (size_t i = 0; i < size; i += chunk) {
                size_t take = size - i < chunk ? size - i : chunk;
                meiso_sha256_update(&sha, input + i, take);
            }
            meiso_sha256_final(&sha, digest);
            if (!matches(digest, vectors[v].digest)) {
                fprintf(stderr, "SHA mismatch size=%zu chunk=%zu\n", size, chunk);
                return 1;
            }
        }
    }
    /* Standard million-'a' vector also exercises the running byte counter. */
    memset(input, 'a', 1000);
    meiso_sha256_init(&sha);
    for (size_t i = 0; i < 1000; ++i) meiso_sha256_update(&sha, input, 1000);
    meiso_sha256_final(&sha, digest);
    return matches(digest,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") ? 0 : 1;
}
