#!/bin/bash
# RETIRED destructive helper.  This file intentionally contains no controller
# reprobe implementation; it is kept only so old commands fail closed with an
# explanation instead of silently finding another script on PATH.
set -u

explain() {
    printf '%s\n' \
        'NVMe runtime polling reprobe is FROZEN/DISABLED.' \
        '' \
        'The old helper unbound and rebound a live controller.' \
        'One recovery path reported that the device did not return and a reboot' \
        'was required.  Controller and namespace names also changed across the' \
        'reprobe, invalidating the old name-based restore assumptions.' \
        '' \
        'This command now performs no discovery, mount operation, module-parameter' \
        'change, PCI operation, benchmark, or recovery action.  Use --plan for the' \
        'requirements of a future maintenance-window experiment.'
}

plan() {
    printf '%s\n' \
        'READ-ONLY PLAN; this command changes nothing.' \
        '1. Reserve an exclusive maintenance window and arrange console recovery.' \
        '2. Identify the target by controller serial + namespace NSID + dev_t;' \
        '   treat /dev/nvme*, /dev/ng*, IRQ numbers, and PCI enumeration as unstable.' \
        '3. Prove every namespace on the controller is unmounted and unused.' \
        '4. Enable poll queues at boot, then reboot; do not runtime-unbind a live device.' \
        '5. Re-resolve and re-prove the identity tuple after boot before measuring.' \
        '6. Reboot to the original boot configuration to disable polling and verify it.' \
        '7. Keep interrupt affinity, CPU topology, buffer PFNs, PSDT/SGL format, and' \
        '   byte-level readback as mandatory evidence for every accepted run.'
}

mode=${1:-}
case "$mode" in
    --explain)
        explain
        exit 0
        ;;
    --plan)
        plan
        exit 0
        ;;
    run|restore|--run|--restore|'')
        printf '%s\n' \
            'REFUSED: runtime NVMe polling/recovery is hard-frozen after a destructive incident.' \
            'Run with --explain or --plan for read-only information.' >&2
        exit 64
        ;;
    *)
        printf 'usage: %s --explain|--plan\n' "$0" >&2
        exit 64
        ;;
esac
