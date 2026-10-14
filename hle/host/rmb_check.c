/* rmb_check.c - host-only rmb_mismatch for -DRMB_CHECK builds of a back-end v5s4 translation (runtime/r5900_rt.h,
 * lever maskbase): called for every guest access whose (base & mask) + offset leaves the window (the access then takes
 * the v4 address, so the run stays correct and is only reported).
 * mk/build.sh host links it for a v5s4 translation. The first 100
 * reports and then every 1000th go to the run log ("RMB_MISMATCH ..."); src/hle/hle_host.c prints the total at the end
 * ("maskbase check: N base+offset accesses outside the guest window" - 0 expected). */
#include <stdint.h>
#include "hle.h"

unsigned long hle_rmb_mismatches;
const int hle_rmb_checked = 1;

void rmb_mismatch(uint32_t base, int32_t off)
{
    hle_rmb_mismatches++;
    if (hle_rmb_mismatches > 100 && hle_rmb_mismatches % 1000) return;
    LOG("RMB_MISMATCH #%lu base=%08x off=%d from %p\n", hle_rmb_mismatches, base, off, __builtin_return_address(0));
}
