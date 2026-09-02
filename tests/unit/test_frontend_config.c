/* Shared, immutable frontend configuration contract.  This test is written
 * before the active implementation.  The fallback declarations keep the RED
 * phase at the missing-symbol boundary until the production header exists. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tap.h"
#include "exitos_intercept.h"

#if defined(__has_include)
# if __has_include("exitos_frontend_config.h")
#  include "exitos_frontend_config.h"
#  define EXITOS_HAVE_FRONTEND_CONFIG_HEADER 1
# endif
#endif

#ifndef EXITOS_HAVE_FRONTEND_CONFIG_HEADER
struct exitos_frontend_config;
int exitos_frontend_config_from_env(struct exitos_frontend_config **out);
void exitos_frontend_config_destroy(struct exitos_frontend_config *config);
int exitos_frontend_config_enabled(const struct exitos_frontend_config *config);
const char *exitos_frontend_config_files(
    const struct exitos_frontend_config *config);
struct exitos_ctx *exitos_frontend_config_ctx(
    const struct exitos_frontend_config *config);
int exitos_frontend_config_path_selected(
    const struct exitos_frontend_config *config, const char *path);
int exitos_frontend_config_donor_fallocate(
    const struct exitos_frontend_config *config);
const char *exitos_frontend_config_donor_dir(
    const struct exitos_frontend_config *config);
unsigned exitos_frontend_config_donor_files(
    const struct exitos_frontend_config *config);
int exitos_frontend_config_fastpath(
    const struct exitos_frontend_config *config);
int exitos_frontend_config_prepare_ready_stop(
    const struct exitos_frontend_config *config);
unsigned exitos_frontend_config_prepare_expected(
    const struct exitos_frontend_config *config);
int exitos_frontend_config_prepare_arrive(
    struct exitos_frontend_config *config, unsigned *arrived,
    unsigned *expected);
int exitos_frontend_config_prepare_stop_take(
    struct exitos_frontend_config *config);
#endif

static void clear_frontend_env(void)
{
    unsetenv("EXITOS_FILES");
    unsetenv("EXITOS_IOPATH");
    unsetenv("EXITOS_STRICT");
    unsetenv("EXITOS_VERIFY_IDENTITY");
    unsetenv("EXITOS_PREPARE");
    unsetenv("EXITOS_DONOR_DIR");
    unsetenv("EXITOS_DONOR_FILES");
    unsetenv("EXITOS_FASTPATH");
    unsetenv("EXITOS_PREPARE_READY_STOP");
    unsetenv("EXITOS_PREPARE_EXPECTED");
}

static struct exitos_frontend_config *load_config(int expected_rc,
                                                   const char *what)
{
    struct exitos_frontend_config *config =
        (struct exitos_frontend_config *)(uintptr_t)0x1;
    int rc = exitos_frontend_config_from_env(&config);

    T_EQ(rc, expected_rc, "%s returns the specified status", what);
    if (expected_rc == 0)
        T_OK(config != NULL, "%s returns a configuration object", what);
    else
        T_OK(config == NULL, "%s leaves no partial configuration", what);
    return expected_rc == 0 ? config : NULL;
}

static void expect_invalid_setting(const char *name, const char *value,
                                   const char *what)
{
    struct exitos_frontend_config *config;

    clear_frontend_env();
    setenv("EXITOS_FILES", "wal", 1);
    setenv(name, value, 1);
    config = load_config(-EINVAL, what);
    (void)config;
}

static void arm_donor_base(void)
{
    clear_frontend_env();
    setenv("EXITOS_FILES", "wal", 1);
    setenv("EXITOS_PREPARE", "donor-fallocate", 1);
    setenv("EXITOS_DONOR_DIR", "/var/lib/exitos/donors", 1);
    setenv("EXITOS_DONOR_FILES", "4", 1);
}

int main(void)
{
    static const char *const valid_modes[] = {
        "pwrite", "nvme", "uring", "uringpoll", "uringwritepoll"
    };
    static const char *const invalid_bools[] = {
        "", "2", "01", "-1", "true", "yes"
    };
    static const char *const invalid_counts[] = {
        "", "0", "+1", "-1", "01", "1K", " 1", "1 ",
        "2147483648", "4294967295", "4294967296",
        "18446744073709551616"
    };
    static const char *const invalid_prepare[] = {
        "", "none", "donor", "DONOR-FALLOCATE", "donor-fallocate ",
        " donor-fallocate"
    };
    struct exitos_frontend_config *config;
    size_t i;

    clear_frontend_env();
    T_EQ(exitos_frontend_config_from_env(NULL), -EINVAL,
         "NULL output pointer is rejected");

    /* A loaded-but-unarmed DSO is inert.  It must ignore every malformed knob
     * because an unrelated application's environment cannot make loading the
     * library fail before EXITOS_FILES opts that process in. */
    clear_frontend_env();
    setenv("EXITOS_IOPATH", "not-a-mode", 1);
    setenv("EXITOS_STRICT", "not-a-bool", 1);
    setenv("EXITOS_VERIFY_IDENTITY", "not-a-bool", 1);
    setenv("EXITOS_PREPARE", "not-a-preparer", 1);
    setenv("EXITOS_DONOR_DIR", "relative", 1);
    setenv("EXITOS_DONOR_FILES", "not-a-count", 1);
    setenv("EXITOS_FASTPATH", "not-a-bool", 1);
    config = load_config(0, "inert malformed environment");
    if (config) {
        T_EQ(exitos_frontend_config_enabled(config), 0,
             "missing EXITOS_FILES leaves the frontend inert");
        T_OK(exitos_frontend_config_ctx(config) == NULL,
             "inert frontend allocates no interception context");
        T_OK(strcmp(exitos_frontend_config_files(config), "") == 0,
             "inert frontend has an empty selector snapshot");
        T_EQ(exitos_frontend_config_path_selected(config, "/tmp/wal"), 0,
             "inert frontend selects no path");
        T_EQ(exitos_frontend_config_donor_fallocate(config), 0,
             "inert frontend enables no preparation mode");
        T_OK(strcmp(exitos_frontend_config_donor_dir(config), "") == 0,
             "inert frontend retains no donor directory");
        T_EQ(exitos_frontend_config_donor_files(config), 0,
             "inert frontend retains no donor count");
        T_EQ(exitos_frontend_config_fastpath(config), 1,
             "EXITOS_FASTPATH has its documented default in inert config");
        T_EQ(exitos_frontend_config_prepare_ready_stop(config), 0,
             "inert frontend cannot arm the preparation stop");
        exitos_frontend_config_destroy(config);
    }

    clear_frontend_env();
    setenv("EXITOS_FILES", "", 1);
    setenv("EXITOS_PREPARE", "bad", 1);
    config = load_config(0, "empty EXITOS_FILES environment");
    if (config) {
        T_EQ(exitos_frontend_config_enabled(config), 0,
             "empty EXITOS_FILES preserves the original inert behavior");
        exitos_frontend_config_destroy(config);
    }

    /* The five documented I/O paths form a closed table. */
    for (i = 0; i < sizeof(valid_modes) / sizeof(valid_modes[0]); ++i) {
        clear_frontend_env();
        setenv("EXITOS_FILES", "wal:database", 1);
        setenv("EXITOS_IOPATH", valid_modes[i], 1);
        config = load_config(0, valid_modes[i]);
        if (!config)
            continue;
        T_EQ(exitos_frontend_config_enabled(config), 1,
             "valid I/O mode arms the frontend");
        T_OK(exitos_frontend_config_ctx(config) != NULL,
             "valid I/O mode constructs one context");
        T_OK(strcmp(exitos_frontend_config_files(config), "wal:database") == 0,
             "configuration retains the exact selector");
        T_EQ(exitos_frontend_config_path_selected(config, "/var/log/wal.1"), 1,
             "colon-list matcher selects matching paths");
        T_EQ(exitos_frontend_config_path_selected(config, "/var/log/other"), 0,
             "colon-list matcher rejects nonmatching paths");
        T_EQ(exitos_frontend_config_fastpath(config), 1,
             "fast path defaults on for an armed frontend");
        setenv("EXITOS_FILES", "changed-after-create", 1);
        T_OK(strcmp(exitos_frontend_config_files(config), "wal:database") == 0,
             "selector is immutable after configuration creation");
        T_EQ(exitos_frontend_config_path_selected(config, "/var/log/wal.2"), 1,
             "path matching uses the immutable selector copy");
        exitos_frontend_config_destroy(config);
    }

    expect_invalid_setting("EXITOS_IOPATH", "", "empty explicit I/O mode");
    expect_invalid_setting("EXITOS_IOPATH", "adaptive-magic",
                           "unknown explicit I/O mode");

    for (i = 0; i < 2; ++i) {
        const char *value = i ? "1" : "0";

        clear_frontend_env();
        setenv("EXITOS_FILES", "wal", 1);
        setenv("EXITOS_STRICT", value, 1);
        config = load_config(0, i ? "EXITOS_STRICT=1" : "EXITOS_STRICT=0");
        exitos_frontend_config_destroy(config);

        clear_frontend_env();
        setenv("EXITOS_FILES", "wal", 1);
        setenv("EXITOS_VERIFY_IDENTITY", value, 1);
        config = load_config(0, i ? "EXITOS_VERIFY_IDENTITY=1"
                                  : "EXITOS_VERIFY_IDENTITY=0");
        exitos_frontend_config_destroy(config);
    }

    for (i = 0; i < sizeof(invalid_bools) / sizeof(invalid_bools[0]); ++i) {
        expect_invalid_setting("EXITOS_STRICT", invalid_bools[i],
                               "malformed EXITOS_STRICT");
        expect_invalid_setting("EXITOS_VERIFY_IDENTITY", invalid_bools[i],
                               "malformed EXITOS_VERIFY_IDENTITY");
        expect_invalid_setting("EXITOS_FASTPATH", invalid_bools[i],
                               "malformed EXITOS_FASTPATH");
    }

    clear_frontend_env();
    setenv("EXITOS_FILES", "wal", 1);
    setenv("EXITOS_STRICT", "1", 1);
    setenv("EXITOS_VERIFY_IDENTITY", "0", 1);
    config = load_config(0, "strict policy with explicit identity zero");
    exitos_frontend_config_destroy(config);

    /* Missing EXITOS_PREPARE is the one spelling of disabled preparation.
     * Donor-only settings while disabled are errors so typos cannot silently
     * turn an intended experiment into the baseline. */
    clear_frontend_env();
    setenv("EXITOS_FILES", "wal", 1);
    config = load_config(0, "preparation disabled by default");
    if (config) {
        T_EQ(exitos_frontend_config_donor_fallocate(config), 0,
             "missing EXITOS_PREPARE disables donor fallocate");
        T_OK(strcmp(exitos_frontend_config_donor_dir(config), "") == 0,
             "disabled preparation exposes no donor directory");
        T_EQ(exitos_frontend_config_donor_files(config), 0,
             "disabled preparation exposes no donor count");
        exitos_frontend_config_destroy(config);
    }

    for (i = 0; i < sizeof(invalid_prepare) / sizeof(invalid_prepare[0]); ++i)
        expect_invalid_setting("EXITOS_PREPARE", invalid_prepare[i],
                               "malformed EXITOS_PREPARE");

    expect_invalid_setting("EXITOS_DONOR_DIR", "/tmp/donors",
                           "donor directory while preparation is disabled");
    expect_invalid_setting("EXITOS_DONOR_DIR", "",
                           "empty donor directory while preparation is disabled");
    expect_invalid_setting("EXITOS_DONOR_FILES", "4",
                           "donor count while preparation is disabled");
    expect_invalid_setting("EXITOS_DONOR_FILES", "",
                           "empty donor count while preparation is disabled");

    clear_frontend_env();
    setenv("EXITOS_FILES", "wal", 1);
    setenv("EXITOS_PREPARE", "donor-fallocate", 1);
    setenv("EXITOS_DONOR_FILES", "4", 1);
    config = load_config(-EINVAL, "enabled donor mode without directory");

    clear_frontend_env();
    setenv("EXITOS_FILES", "wal", 1);
    setenv("EXITOS_PREPARE", "donor-fallocate", 1);
    setenv("EXITOS_DONOR_DIR", "/tmp/donors", 1);
    config = load_config(-EINVAL, "enabled donor mode without count");

    {
        static const char *const invalid_dirs[] = { "", ".", "relative", "/" };

        for (i = 0; i < sizeof(invalid_dirs) / sizeof(invalid_dirs[0]); ++i) {
            arm_donor_base();
            setenv("EXITOS_DONOR_DIR", invalid_dirs[i], 1);
            config = load_config(-EINVAL, "invalid donor directory");
        }
    }

    for (i = 0; i < sizeof(invalid_counts) / sizeof(invalid_counts[0]); ++i) {
        arm_donor_base();
        setenv("EXITOS_DONOR_FILES", invalid_counts[i], 1);
        config = load_config(-EINVAL, "noncanonical donor file count");
    }

    arm_donor_base();
    setenv("EXITOS_DONOR_FILES", "1", 1);
    config = load_config(0, "minimum donor file count");
    if (config) {
        T_EQ(exitos_frontend_config_donor_files(config), 1,
             "minimum canonical donor count is accepted");
        exitos_frontend_config_destroy(config);
    }

    arm_donor_base();
    setenv("EXITOS_DONOR_FILES", "2147483647", 1);
    config = load_config(0, "maximum representable donor file count");
    if (config) {
        T_EQ(exitos_frontend_config_donor_files(config), INT_MAX,
             "INT_MAX donor count is accepted exactly");
        exitos_frontend_config_destroy(config);
    }

    /* Complete donor configuration and fast-path selection are static. */
    arm_donor_base();
    setenv("EXITOS_FASTPATH", "0", 1);
    config = load_config(0, "donor fallocate with fast path disabled");
    if (config) {
        T_EQ(exitos_frontend_config_donor_fallocate(config), 1,
             "exact donor-fallocate spelling enables the preparer");
        T_OK(strcmp(exitos_frontend_config_donor_dir(config),
                    "/var/lib/exitos/donors") == 0,
             "absolute non-root donor directory is retained exactly");
        T_EQ(exitos_frontend_config_donor_files(config), 4,
             "canonical donor file count is retained exactly");
        T_EQ(exitos_frontend_config_fastpath(config), 0,
             "EXITOS_FASTPATH=0 disables fast-path takeover");

        setenv("EXITOS_PREPARE", "bad-after-create", 1);
        setenv("EXITOS_DONOR_DIR", "/changed", 1);
        setenv("EXITOS_DONOR_FILES", "99", 1);
        setenv("EXITOS_FASTPATH", "1", 1);
        T_EQ(exitos_frontend_config_donor_fallocate(config), 1,
             "preparation mode is immutable after creation");
        T_OK(strcmp(exitos_frontend_config_donor_dir(config),
                    "/var/lib/exitos/donors") == 0,
             "donor directory is immutable after creation");
        T_EQ(exitos_frontend_config_donor_files(config), 4,
             "donor count is immutable after creation");
        T_EQ(exitos_frontend_config_fastpath(config), 0,
             "fast-path selection is immutable after creation");
        exitos_frontend_config_destroy(config);
    }

    arm_donor_base();
    setenv("EXITOS_FASTPATH", "1", 1);
    config = load_config(0, "explicit fast path enable");
    if (config) {
        T_EQ(exitos_frontend_config_fastpath(config), 1,
             "EXITOS_FASTPATH=1 enables fast-path takeover");
        exitos_frontend_config_destroy(config);
    }

    /* The measurement-only ready stop is immutable, fail-closed, and tied to
     * the exact donor width.  The Nth successful preparation is the sole
     * caller told to stop the process; earlier and later arrivals proceed. */
    arm_donor_base();
    setenv("EXITOS_PREPARE_READY_STOP", "1", 1);
    config = load_config(-EINVAL, "ready stop without expected count");

    arm_donor_base();
    setenv("EXITOS_PREPARE_EXPECTED", "4", 1);
    config = load_config(-EINVAL, "expected count without ready stop");

    arm_donor_base();
    setenv("EXITOS_PREPARE_READY_STOP", "0", 1);
    setenv("EXITOS_PREPARE_EXPECTED", "4", 1);
    config = load_config(-EINVAL, "disabled stop with stray expected count");

    arm_donor_base();
    setenv("EXITOS_PREPARE_READY_STOP", "1", 1);
    setenv("EXITOS_PREPARE_EXPECTED", "3", 1);
    config = load_config(-EINVAL, "expected count differs from donor width");

    arm_donor_base();
    setenv("EXITOS_PREPARE_READY_STOP", "1", 1);
    setenv("EXITOS_PREPARE_EXPECTED", "04", 1);
    config = load_config(-EINVAL, "noncanonical expected count");

    arm_donor_base();
    setenv("EXITOS_PREPARE_READY_STOP", "1", 1);
    setenv("EXITOS_PREPARE_EXPECTED", "4", 1);
    config = load_config(0, "exact preparation ready stop");
    if (config) {
        unsigned arrived = 99, expected = 99;

        T_EQ(exitos_frontend_config_prepare_ready_stop(config), 1,
             "ready stop is armed exactly");
        T_EQ(exitos_frontend_config_prepare_expected(config), 4,
             "ready stop retains the immutable expected count");
        T_EQ(exitos_frontend_config_prepare_arrive(config, &arrived, &expected), 0,
             "first successful preparation does not stop");
        T_EQ(arrived, 1, "first arrival count is exact");
        T_EQ(expected, 4, "arrival reports the immutable expected count");
        T_EQ(exitos_frontend_config_prepare_arrive(config, &arrived, &expected), 0,
             "second successful preparation does not stop");
        T_EQ(exitos_frontend_config_prepare_arrive(config, &arrived, &expected), 0,
             "third successful preparation does not stop");
        T_EQ(exitos_frontend_config_prepare_arrive(config, &arrived, &expected), 1,
             "exactly the fourth successful preparation arms one stop");
        T_EQ(arrived, 4, "triggering arrival count is exact");
        T_EQ(exitos_frontend_config_prepare_stop_take(config), 1,
             "the first selected close consumes the armed stop");
        T_EQ(exitos_frontend_config_prepare_stop_take(config), 0,
             "the armed stop can be consumed only once");
        T_EQ(exitos_frontend_config_prepare_arrive(config, &arrived, &expected), 0,
             "later preparations cannot trigger a second stop");
        T_EQ(arrived, 5, "later arrivals remain observable");
        setenv("EXITOS_PREPARE_EXPECTED", "99", 1);
        T_EQ(exitos_frontend_config_prepare_expected(config), 4,
             "ready-stop expectation is immutable after construction");
        exitos_frontend_config_destroy(config);
    }

    clear_frontend_env();
    T_DONE();
}
