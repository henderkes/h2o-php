#ifndef H2O_PHP_CPU_BUDGET_H
#define H2O_PHP_CPU_BUDGET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HP_CPU_BUDGET_JSON_CAP 2048

/* Internal PHP defaults only. H2O's network-loop CPU policy is unchanged. */
typedef enum {
    HP_CPU_GOMAX_ABSENT,
    HP_CPU_GOMAX_EMPTY,
    HP_CPU_GOMAX_POSITIVE,
    HP_CPU_GOMAX_NONPOSITIVE,
    HP_CPU_GOMAX_INVALID
} hp_cpu_gomax_state_t;
typedef enum {
    HP_CPU_DISCOVERY_LINUX_AFFINITY,
    HP_CPU_DISCOVERY_PLATFORM_ONLINE,
    HP_CPU_DISCOVERY_FALLBACK_ONE
} hp_cpu_discovery_t;
typedef enum {
    HP_CPU_QUOTA_NONE,
    HP_CPU_QUOTA_FINITE,
    HP_CPU_QUOTA_UNLIMITED,
    HP_CPU_QUOTA_ERROR,
    HP_CPU_QUOTA_MALFORMED,
    HP_CPU_QUOTA_SKIPPED,
    HP_CPU_QUOTA_UNSUPPORTED
} hp_cpu_quota_state_t;
typedef struct {
    hp_cpu_quota_state_t state;
    unsigned version;
    int64_t quota_us, period_us;
    int error;
} hp_cpu_quota_info_t;
typedef struct {
    unsigned affinity_cpus;
    unsigned startup_cpus;
    unsigned defaults_cpus;
    hp_cpu_discovery_t discovery;
    hp_cpu_gomax_state_t gomax_state;
    bool gomax_present;
    int affinity_error;
    int containermaxprocs, updatemaxprocs;
    hp_cpu_quota_info_t raw_quota, caddy_quota;
    bool caddy_override, automatic_update_eligible;
    /* Nonzero defaults_error rejects only omitted CPU-based pool defaults.
     * Explicit positive PHP budgets do not depend on a representable C. */
    int defaults_error;
    /* False: exact private Go1.27 parser and transient startup updates remain
     * unqualified even when the ordinary stable Linux policy is applied. */
    bool quota_qualified;
} hp_cpu_budget_info_t;

/* Capture before YAML callbacks or PHP startup can affect process state. The
 * record is immutable, process-wide, and independent of the PHP env sandbox. */
void hp_cpu_budget_capture_startup(void);
const hp_cpu_budget_info_t *hp_cpu_budget_info(void);
/* Separate contracts deliberately: reference pool defaults can use a later
 * Caddy quota override, but its scaler normalizes against package-init P0. */
unsigned hp_cpu_budget_default_count(void);
bool hp_cpu_budget_default_count_checked(unsigned *);
unsigned hp_cpu_budget_scaling_count(void);
/* Additive effective-config/metrics metadata, with no raw environment value. */
bool hp_cpu_budget_json(char *, size_t);
#endif
