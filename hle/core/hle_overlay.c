/* hle_overlay.c - code overlays in the Metrowerks MWo3 format: when an overlay image has been loaded into the overlay
 * window, its header (magic "MWo3", id, load address, text / data / bss sizes, name) selects the translated overlay
 * unit the dispatcher uses from then on. The game pack registers the units (rc_overlay_lookup[id]) and the window
 * (rc_overlay_lo / rc_overlay_hi) at boot and calls hle_overlay_loaded from its loader hook. */
#include <string.h>
#include "hle.h"

void hle_overlay_loaded(uint32_t addr)
{
    if (memcmp(gptr(addr, 4), "MWo3", 4) != 0) {
        LOG("overlay load at %08x: no MWo3 header - dispatcher unchanged\n", addr);
        return;
    }
    uint32_t id = gw(addr + 4), load = gw(addr + 8), text = gw(addr + 12), data = gw(addr + 16), bss = gw(addr + 20);
    char name[33];
    memcpy(name, gptr(addr + 0x20, 32), 32);
    name[32] = 0;
    if (id == 0 || id >= RC_MAX_OVERLAYS || !rc_overlay_lookup[id]) {
        LOG("overlay %s has unknown id %u\n", name, id);
        return;
    }
    rc_overlay_current = (int)id;
    rc_overlay_lo = load;
    rc_overlay_hi = load + 0x40 + text + data + bss;
    char ms[96];
    snprintf(ms, sizeof ms, "overlay %s loaded (id %u, %08x-%08x)", name, id, rc_overlay_lo, rc_overlay_hi);
    hle_milestone(ms);
}
