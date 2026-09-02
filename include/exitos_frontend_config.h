/* Shared, immutable process configuration for interception frontends. */
#ifndef EXITOS_FRONTEND_CONFIG_H
#define EXITOS_FRONTEND_CONFIG_H

struct exitos_ctx;
struct exitos_frontend_config;

/* EXITOS_FILES is the opt-in gate.  When it is absent or empty, construction
 * succeeds with an inert object and deliberately ignores every other knob.
 * Once armed, all settings are validated and copied before this returns.
 * Returns 0 or a negative errno; *out is NULL on failure. */
int exitos_frontend_config_from_env(struct exitos_frontend_config **out);

/* The configuration owns its context.  Callers must quiesce users and clear
 * any borrowed ctx alias before destroying the configuration. */
void exitos_frontend_config_destroy(struct exitos_frontend_config *config);

int exitos_frontend_config_enabled(const struct exitos_frontend_config *config);
const char *exitos_frontend_config_files(
    const struct exitos_frontend_config *config);
/* Borrowed for the lifetime of config. */
struct exitos_ctx *exitos_frontend_config_ctx(
    const struct exitos_frontend_config *config);
int exitos_frontend_config_path_selected(
    const struct exitos_frontend_config *config, const char *path);

/* Static preparation policy.  donor-fallocate is the only active preparation
 * mode; its directory and pool width are meaningful only when it is enabled. */
int exitos_frontend_config_donor_fallocate(
    const struct exitos_frontend_config *config);
const char *exitos_frontend_config_donor_dir(
    const struct exitos_frontend_config *config);
unsigned exitos_frontend_config_donor_files(
    const struct exitos_frontend_config *config);

/* Measurement-only preparation barrier.  When explicitly armed, the Nth
 * successful prepared fallocate is told to stop the process after publishing
 * PREPARE_READY.  The expected count is immutable and must equal the donor
 * pool width, so all serialized fio setup completes before timed workers can
 * be created.  Arrival returns 1 exactly once, 0 otherwise. */
int exitos_frontend_config_prepare_ready_stop(
    const struct exitos_frontend_config *config);
unsigned exitos_frontend_config_prepare_expected(
    const struct exitos_frontend_config *config);
int exitos_frontend_config_prepare_arrive(
    struct exitos_frontend_config *config, unsigned *arrived,
    unsigned *expected);
int exitos_frontend_config_prepare_stop_take(
    struct exitos_frontend_config *config);

/* Whether the frontend may take the raw fast path after setup.  Defaults to 1
 * for every successfully constructed configuration, including an inert one. */
int exitos_frontend_config_fastpath(
    const struct exitos_frontend_config *config);

#endif
