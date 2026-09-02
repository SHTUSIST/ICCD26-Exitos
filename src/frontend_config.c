#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "exitos_frontend_config.h"
#include "exitos_intercept.h"

struct exitos_frontend_config {
    char *files;
    char *donor_dir;
    struct exitos_ctx *ctx;
    _Atomic unsigned prepare_arrived;
    _Atomic unsigned prepare_stop_pending;
    unsigned donor_files;
    unsigned prepare_expected;
    unsigned enabled : 1;
    unsigned donor_fallocate : 1;
    unsigned prepare_ready_stop : 1;
    unsigned fastpath : 1;
};

static int bool_from_env(const char *name, int fallback, int *out)
{
    const char *value = getenv(name);

    *out = fallback;
    if (!value)
        return 0;
    if (value[0] == '0' && value[1] == '\0') {
        *out = 0;
        return 0;
    }
    if (value[0] == '1' && value[1] == '\0') {
        *out = 1;
        return 0;
    }
    return -EINVAL;
}

static int donor_files_from_env(const char *value, unsigned *out)
{
    uint64_t parsed = 0;
    const unsigned char *p;

    *out = 0;
    if (!value || value[0] < '1' || value[0] > '9')
        return -EINVAL;

    /* Canonical base-ten unsigned integer: no sign, whitespace, suffix, zero,
     * or leading zero.  The incremental bound check avoids libc/locale quirks. */
    for (p = (const unsigned char *)value; *p; ++p) {
        unsigned digit;

        if (*p < '0' || *p > '9')
            return -EINVAL;
        digit = (unsigned)(*p - '0');
        /* donor_pool_create() deliberately uses an int width. Reject a value
         * the runtime cannot represent here, while parsing, rather than
         * accepting a configuration that makes the DSO constructor turn
         * silently inert later. */
        if (parsed > ((uint64_t)INT_MAX - digit) / 10u)
            return -EINVAL;
        parsed = parsed * 10u + digit;
    }
    *out = (unsigned)parsed;
    return 0;
}

static int configure_prepare(struct exitos_frontend_config *config)
{
    const char *prepare = getenv("EXITOS_PREPARE");
    const char *donor_dir = getenv("EXITOS_DONOR_DIR");
    const char *donor_files = getenv("EXITOS_DONOR_FILES");
    int rc;

    if (!prepare) {
        /* Presence is an error even for an empty value.  This makes a misspelled
         * or omitted EXITOS_PREPARE fail instead of benchmarking the baseline. */
        if (donor_dir || donor_files)
            return -EINVAL;
        return 0;
    }
    if (strcmp(prepare, "donor-fallocate") != 0)
        return -EINVAL;
    if (!donor_dir || donor_dir[0] != '/' || strcmp(donor_dir, "/") == 0)
        return -EINVAL;

    rc = donor_files_from_env(donor_files, &config->donor_files);
    if (rc != 0)
        return rc;
    config->donor_dir = strdup(donor_dir);
    if (!config->donor_dir)
        return -ENOMEM;
    config->donor_fallocate = 1;
    return 0;
}

static int configure_prepare_ready_stop(struct exitos_frontend_config *config)
{
    const char *stop_value = getenv("EXITOS_PREPARE_READY_STOP");
    const char *expected_value = getenv("EXITOS_PREPARE_EXPECTED");
    unsigned expected;
    int stop = 0;
    int rc;

    if (!stop_value) {
        if (expected_value)
            return -EINVAL;
        return 0;
    }
    rc = bool_from_env("EXITOS_PREPARE_READY_STOP", 0, &stop);
    if (rc != 0)
        return rc;
    if (!stop) {
        if (expected_value)
            return -EINVAL;
        return 0;
    }
    if (!config->donor_fallocate || !expected_value)
        return -EINVAL;
    rc = donor_files_from_env(expected_value, &expected);
    if (rc != 0 || expected != config->donor_files)
        return -EINVAL;
    config->prepare_expected = expected;
    config->prepare_ready_stop = 1;
    atomic_init(&config->prepare_arrived, 0);
    atomic_init(&config->prepare_stop_pending, 0);
    return 0;
}

int exitos_frontend_config_from_env(struct exitos_frontend_config **out)
{
    struct exitos_frontend_config *config;
    const char *files;
    int strict, verify, fastpath, rc;

    if (!out)
        return -EINVAL;
    *out = NULL;
    config = calloc(1, sizeof(*config));
    if (!config)
        return -ENOMEM;
    config->fastpath = 1;

    files = getenv("EXITOS_FILES");
    config->files = strdup(files && *files ? files : "");
    if (!config->files) {
        free(config);
        return -ENOMEM;
    }

    /* The DSO is deliberately inert until the application opts in. */
    if (!files || !*files) {
        *out = config;
        return 0;
    }

    rc = bool_from_env("EXITOS_STRICT", 0, &strict);
    if (rc == 0)
        rc = bool_from_env("EXITOS_VERIFY_IDENTITY", 0, &verify);
    if (rc == 0)
        rc = bool_from_env("EXITOS_FASTPATH", 1, &fastpath);
    if (rc == 0)
        rc = configure_prepare(config);
    if (rc == 0)
        rc = configure_prepare_ready_stop(config);
    if (rc == 0)
        rc = exitos_ctx_create_with_options(&config->ctx,
                                             getenv("EXITOS_IOPATH"));
    if (rc != 0) {
        exitos_frontend_config_destroy(config);
        return rc;
    }

    if (verify)
        exitos_ctx_verify_identity(config->ctx, 1);
    if (strict)
        exitos_ctx_strict(config->ctx, 1); /* strict implies identity */
    config->fastpath = (unsigned)fastpath;
    config->enabled = 1;
    *out = config;
    return 0;
}

void exitos_frontend_config_destroy(struct exitos_frontend_config *config)
{
    if (!config)
        return;
    exitos_ctx_destroy(config->ctx);
    free(config->donor_dir);
    free(config->files);
    free(config);
}

int exitos_frontend_config_enabled(const struct exitos_frontend_config *config)
{
    return config ? (int)config->enabled : 0;
}

const char *exitos_frontend_config_files(
    const struct exitos_frontend_config *config)
{
    return config && config->files ? config->files : "";
}

struct exitos_ctx *exitos_frontend_config_ctx(
    const struct exitos_frontend_config *config)
{
    return config ? config->ctx : NULL;
}

static int path_contains(const char *path, const char *token, size_t token_len)
{
    size_t path_len = strlen(path);
    size_t i;

    if (token_len > path_len)
        return 0;
    for (i = 0; i + token_len <= path_len; ++i)
        if (memcmp(path + i, token, token_len) == 0)
            return 1;
    return 0;
}

int exitos_frontend_config_path_selected(
    const struct exitos_frontend_config *config, const char *path)
{
    const char *begin, *end;

    if (!config || !config->enabled || !path)
        return 0;
    begin = config->files;
    while (*begin) {
        end = strchr(begin, ':');
        if (!end)
            end = begin + strlen(begin);
        if (end != begin && path_contains(path, begin, (size_t)(end - begin)))
            return 1;
        if (*end == '\0')
            break;
        begin = end + 1;
    }
    return 0;
}

int exitos_frontend_config_donor_fallocate(
    const struct exitos_frontend_config *config)
{
    return config ? (int)config->donor_fallocate : 0;
}

const char *exitos_frontend_config_donor_dir(
    const struct exitos_frontend_config *config)
{
    return config && config->donor_dir ? config->donor_dir : "";
}

unsigned exitos_frontend_config_donor_files(
    const struct exitos_frontend_config *config)
{
    return config ? config->donor_files : 0;
}

int exitos_frontend_config_prepare_ready_stop(
    const struct exitos_frontend_config *config)
{
    return config ? (int)config->prepare_ready_stop : 0;
}

unsigned exitos_frontend_config_prepare_expected(
    const struct exitos_frontend_config *config)
{
    return config ? config->prepare_expected : 0;
}

int exitos_frontend_config_prepare_arrive(
    struct exitos_frontend_config *config, unsigned *arrived,
    unsigned *expected)
{
    unsigned count = 0;

    if (config && config->prepare_ready_stop)
        count = atomic_fetch_add_explicit(&config->prepare_arrived, 1,
                                          memory_order_relaxed) + 1;
    if (arrived)
        *arrived = count;
    if (expected)
        *expected = config ? config->prepare_expected : 0;
    if (config && config->prepare_ready_stop &&
        count == config->prepare_expected) {
        atomic_store_explicit(&config->prepare_stop_pending, 1,
                              memory_order_release);
        return 1;
    }
    return 0;
}

int exitos_frontend_config_prepare_stop_take(
    struct exitos_frontend_config *config)
{
    unsigned pending = 1;

    if (!config || !config->prepare_ready_stop)
        return 0;
    return atomic_compare_exchange_strong_explicit(
        &config->prepare_stop_pending, &pending, 0,
        memory_order_acq_rel, memory_order_acquire);
}

int exitos_frontend_config_fastpath(
    const struct exitos_frontend_config *config)
{
    return config ? (int)config->fastpath : 0;
}
