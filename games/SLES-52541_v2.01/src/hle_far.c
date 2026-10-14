/* hle_far.c - far-entity policy of the world update: the player settings far_ai / far_freeze / far_collision
 * (hle_settings.c: x_agfar / x_agfrz / x_farcol). Default 0 = off: the world update runs exactly as translated.
 *
 * How: the world update (0x283900) stays the translated original. sites.def routes four of its calls through the
 * handlers below (ps2rc/pipeline/prepare_gen.py --sites: the call site, found by its return address, calls
 * handler(c, target) instead of the target); a handler either runs the call (rc_dispatch) or, by our policy, skips it
 * and/or does extra work around it with the game's own functions:
 *   - the call before the update's moving-entity passes (return address 0x283a98): latch this frame's camera
 *     position, frame counter and the players (the policy's inputs);
 *   - the per-entity control call (vtable +48, return address 0x283b18): far_freeze / far_ai skip it for the
 *     entities they select;
 *   - the call that opens the collision section (0x595C20, return address 0x283c38): after it, the entities the
 *     policy selects get the "in a safe position" flag (+28 bit 5) - the update's own collision passes then leave
 *     them alone;
 *   - the call that closes the collision section (0x595C60, return address 0x283fe0): before it, the far_collision
 *     entities of this frame are moved by their own speed (0x24CC90, 0x24CD30) and their render matrix / frame are
 *     brought up to date (0x10ED80 or 0x25FB70, then 0x22F4D0).
 * This is our own scheduling policy (which entity is simulated in which frame), not a re-implementation of the
 * world update: the update itself, the entities' control / collision / movement and the render-frame updates are
 * the game's functions, called by address. The policy is the one the developer world module used (same entities,
 * same frames), so a build with these handlers is guest-identical to it at the same settings.
 *
 * Facts used (addresses / offsets for interoperability): gp+2376 frame counter, gp+2484 moving-entity list (node:
 * item +0, next +4); entity +4 position (no matrix), +20 matrix (position at +48), +24 render object (+4 frame;
 * frame +16 = its matrix), +28 bit 5 "in a safe position", +54 bits 0-2 type (2 vehicle, 3 ped, 4 object) / bits 3-7
 * status (0 = player-driven); players 0x70A260 (stride 444, +0 the player's ped); camera 0x6FF190 (same entity
 * layout).
 *
 * far_collision=D (x_farcol): a moving ped / vehicle that is not a player nor player-driven and is farther than D m
 *   from the camera gets its collision passes every 2nd frame only (alternating by entity); in its other frame it is
 *   moved by its own speed without collision. far_ai=D (x_agfar): beyond D m, peds, parked / abandoned vehicles
 *   (status 4) and objects run their control call (AI, gravity, friction) every 2nd frame (same parity). far_freeze=F
 *   (x_agfrz): beyond F m, peds, vehicles not driven by a player and objects are frozen (no control call, no
 *   collision passes, no movement). x_farcol=-1 is accepted and means "no far-collision LOD" here (the developer
 *   module's check mode).
 */
#include <string.h>
#include "hle.h"
#include "hle_xgame.h"

#define FAR_PLAYERS 0x0070A260u
#define FAR_CAMERA  0x006FF190u
#define FAR_SKIP_MAX 256

void far_site_frame(R5900Ctx *c, uint32_t target);
void far_site_control(R5900Ctx *c, uint32_t target);
void far_site_coll_begin(R5900Ctx *c, uint32_t target);
void far_site_coll_end(R5900Ctx *c, uint32_t target);

static int far_gen = -1;                 /* hle_x_gen when the keys were read (run-time A/B switches) */
static int far_col, far_ai, far_frz;     /* x_farcol / x_agfar / x_agfrz */
static float col_d2, ai_d2, frz_d2;
static int far_on;                       /* this frame: any key > 0 (latched at the frame site) */
static float cam[3];
static uint32_t frame_no, player0, player1;
static uint32_t skip[FAR_SKIP_MAX];
static int nskip;
static uint64_t st_frames, st_ctl_skipped, st_frozen, st_col_skipped, st_marked_frozen;

static void far_read_keys(void)
{
    far_gen = hle_x_gen;
    far_col = hle_x_opt("farcol", 0);
    far_ai = hle_x_opt("agfar", 0);
    far_frz = hle_x_opt("agfrz", 0);
    col_d2 = far_col > 0 ? (float)far_col * (float)far_col : 0.0f;
    ai_d2 = far_ai > 0 ? (float)far_ai * (float)far_ai : 0.0f;
    frz_d2 = far_frz > 0 ? (float)far_frz * (float)far_frz : 0.0f;
}

/* the entity's position: its matrix's when it has one, else its own */
static float dist2(uint32_t e)
{
    uint32_t m = gw(e + 20);
    uint32_t a = m ? m + 48 : e + 4;
    float d2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        float p = u2f(gw(a + 4 * i)) - cam[i];
        d2 += p * p;
    }
    return d2;
}

static int is_player(uint32_t e, uint32_t status) { return status == 0 || e == player0 || e == player1; }
static int odd_frame(uint32_t e) { return ((frame_no + (e >> 4)) & 1u) != 0; }

/* far_freeze (all = vehicles of every status) / far_ai (vehicles: status 4 only) candidates: -1 = never */
static float ag_dist2(uint32_t e, int all)
{
    uint8_t b54 = gb(e + 54);
    uint32_t type = b54 & 7u, status = b54 >> 3;
    if (is_player(e, status)) return -1.0f;
    if (type != 3 && type != 4 && !(type == 2 && (all || status == 4))) return -1.0f;
    return dist2(e);
}
static int frozen(uint32_t e) { return far_frz > 0 && ag_dist2(e, 1) > frz_d2; }
static int ai_off(uint32_t e) { return far_ai > 0 && odd_frame(e) && ag_dist2(e, 0) > ai_d2; }
static int col_off(uint32_t e)
{
    if (far_col <= 0) return 0;
    uint8_t b54 = gb(e + 54);
    uint32_t type = b54 & 7u, status = b54 >> 3;
    if (type != 2 && type != 3) return 0;               /* peds and vehicles */
    if (is_player(e, status)) return 0;
    if (!(dist2(e) > col_d2)) return 0;
    return odd_frame(e);
}

static inline void call1(R5900Ctx *c, uint32_t fn, uint32_t a0)
{
    c->r[4].d[0] = (uint64_t)(int64_t)(int32_t)a0;
    rc_dispatch(c, fn);
}
static inline void call2(R5900Ctx *c, uint32_t fn, uint32_t a0, uint32_t a1)
{
    c->r[4].d[0] = (uint64_t)(int64_t)(int32_t)a0;
    c->r[5].d[0] = (uint64_t)(int64_t)(int32_t)a1;
    rc_dispatch(c, fn);
}

void far_site_frame(R5900Ctx *c, uint32_t target)
{
    if (far_gen != hle_x_gen) far_read_keys();
    far_on = far_col > 0 || far_ai > 0 || far_frz > 0;
    nskip = 0;
    if (far_on) {
        uint32_t gp = (uint32_t)c->r[28].w[0];
        uint32_t m = gw(FAR_CAMERA + 20);
        uint32_t a = m ? m + 48 : FAR_CAMERA + 4;
        for (int i = 0; i < 3; i++) cam[i] = u2f(gw(a + 4 * i));
        frame_no = gw(gp + 2376);
        player0 = gw(FAR_PLAYERS);
        player1 = gw(FAR_PLAYERS + 444);
        st_frames++;
    }
    rc_dispatch(c, target);
}

void far_site_control(R5900Ctx *c, uint32_t target)
{
    if (far_on) {
        uint32_t e = (uint32_t)c->r[4].d[0];           /* the call's argument: the entity */
        if (frozen(e)) { st_frozen++; return; }
        if (ai_off(e)) { st_ctl_skipped++; return; }
    }
    rc_dispatch(c, target);
}

void far_site_coll_begin(R5900Ctx *c, uint32_t target)
{
    rc_dispatch(c, target);
    if (!far_on) return;
    uint32_t gp = (uint32_t)c->r[28].w[0];
    for (uint32_t n = gw(gp + 2484); n; n = gw(n + 4)) {
        uint32_t e = gw(n);
        if (gb(e + 28) & 0x20) continue;                 /* already in a safe position */
        if (frozen(e)) {
            sb_(e + 28, (uint8_t)(gb(e + 28) | 0x20));
            st_marked_frozen++;
            continue;
        }
        if (nskip < FAR_SKIP_MAX && col_off(e)) {
            skip[nskip++] = e;
            sb_(e + 28, (uint8_t)(gb(e + 28) | 0x20));
        }
    }
    st_col_skipped += (uint64_t)nskip;
}

void far_site_coll_end(R5900Ctx *c, uint32_t target)
{
    if (far_on && nskip) {
        R5900Reg128 a0 = c->r[4], a1 = c->r[5];         /* the call's arguments, already in the context */
        for (int i = 0; i < nskip; i++) {
            uint32_t e = skip[i];
            call1(c, 0x24CC90, e);                       /* move by its speed */
            call1(c, 0x24CD30, e);                       /* turn by its turn speed */
            uint32_t rw = gw(e + 24);
            if (rw) {                                    /* render matrix from the matrix / the position */
                uint32_t frame = gw(rw + 4), m = gw(e + 20);
                if (m) call2(c, 0x10ED80, m, frame + 16);
                else call2(c, 0x25FB70, e + 4, frame + 16);
            }
            call1(c, 0x22F4D0, e);                       /* the render frame */
        }
        nskip = 0;
        c->r[4] = a0;
        c->r[5] = a1;
    }
    rc_dispatch(c, target);
}

void hle_far_report(void)
{
    if (!st_frames) return;
    double f = (double)st_frames;
    LOG("far policy (sites): far_collision=%d far_ai=%d far_freeze=%d: %llu frames; per frame: collision skipped %.2f, "
        "control skipped %.2f (far_ai), control frozen %.2f, collision frozen %.2f\n", far_col, far_ai, far_frz,
        (unsigned long long)st_frames, (double)st_col_skipped / f, (double)st_ctl_skipped / f, (double)st_frozen / f,
        (double)st_marked_frozen / f);
}
