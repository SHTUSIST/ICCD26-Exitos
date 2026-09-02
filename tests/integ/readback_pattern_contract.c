/* White-box contract for the deterministic readback record pattern.
 *
 * This deliberately includes the benchmark implementation so the test can
 * inspect its private pattern generator without adding a production API.
 */
#define main readback_cli_main
#include "../../attribution/readback.c"
#undef main

static uint64_t load_le64(const unsigned char *p)
{
    uint64_t v = 0;

    for (unsigned int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (i * 8U);
    return v;
}

int main(void)
{
    static const enum pattern_tag tags[] = {
        PAT_BASE, PAT_PAYLOAD, PAT_GUARD_L, PAT_GUARD_R,
    };
    static const size_t records[] = {
        0, 1, 255, 256, MAX_RECORDS, MAX_RECORDS + 1U,
    };
    void *a = NULL, *b = NULL;
    int rc = 1;

    if (alloc_record(&a) != 0 || alloc_record(&b) != 0) {
        fprintf(stderr, "pattern contract: aligned allocation failed\n");
        goto out;
    }

    /* The old byte-periodic generator makes these two second halves exactly
     * equal.  A whole-record swap outside the small text header would then be
     * invisible over most of the record. */
    fill_pattern(a, PAT_PAYLOAD, 0);
    fill_pattern(b, PAT_PAYLOAD, 256);
    if (memcmp((unsigned char *)a + 4096,
               (unsigned char *)b + 4096, 4096) == 0) {
        fprintf(stderr,
                "pattern contract: records 0 and 256 share the entire second 4K\n");
        goto out;
    }

    /* A later GREEN implementation must encode both the record and word
     * offset in every word, including both 4K halves.  The exact encoding is
     * checked here instead of relying only on probabilistic memcmp samples. */
    for (size_t t = 0; t < sizeof(tags) / sizeof(tags[0]); t++) {
        for (size_t r = 0; r < sizeof(records) / sizeof(records[0]); r++) {
            fill_pattern(a, tags[t], records[r]);
            for (size_t word = 0; word < REC / sizeof(uint64_t); word++) {
                uint64_t got = load_le64((unsigned char *)a + word * 8U);
                uint64_t want = UINT64_C(0xe100000000000000) |
                                ((uint64_t)tags[t] << 52U) |
                                ((uint64_t)records[r] << 31U) |
                                (uint64_t)word;
                if (got != want) {
                    fprintf(stderr, "pattern contract: tag=%u record=%zu "
                            "word=%zu got=%016" PRIx64
                            " expected=%016" PRIx64 "\n",
                            (unsigned int)tags[t], records[r], word, got, want);
                    goto out;
                }
            }
        }
    }

    rc = 0;
out:
    free(a);
    free(b);
    return rc;
}
