/* hle_ce.c - night-time levers and a game-clock test aid (x_cbatch default 1 = same picture, the others default
 * 0 = off: the calls run exactly as translated). Keys (ps2rc.cfg / host environment HLE_X_<KEY>; switchable in the developer in-run A/B):
 *
 *   x_clock=HHMM  test aid: the in-game clock is held at HH:MM (1..2400, 2400 = midnight) - written every frame just
 *                 before the time-of-day colour update, so the whole frame (colours, street lights, light pools,
 *                 coronas, population by hour) is drawn at that time. For reproducible day / night measurements.
 *   x_cbatch=1    the coronas' sprites are drawn in one GE batch per render state instead of one GE draw per corona
 *                 (hle_sky.c): the corona pass flushes the game's render buffer once per visible corona (~13 GE draws
 *                 per frame at night, 1-3 by day). Same picture (additive sprites; any state change still splits). Default 1;
 *                 x_cbatch=0 = one GE draw per corona.
 *   x_lamp=D      the 2D-effect lights of a visible entity (street lamps, traffic lights, neon: corona, light pool,
 *                 point light) are processed only within D metres of the camera. Visible: lamps farther than D m
 *                 stay dark (no corona, no light pool).
 *   x_lpool=P     the light pools the lamps put on the ground (drawn up to 40 m by the game) only up to P % of
 *                 that distance (1..99). Visible: the pools of farther lamps are not drawn.
 *
 * How: sites.def routes these calls through the handlers below (prepare_gen --sites):
 *   - the time-of-day colour update called by the game step (return address 0x243690): x_clock writes the clock
 *     (hours u8 0x66C02C, minutes u8 0x66C030, seconds u16 0x66C034) first;
 *   - the corona pass called by the effects step (0x247010): x_cbatch opens the sprite batch around it;
 *   - the entity lights call of the two pre-render functions (0x22FABC, 0x167EE0; a0 = the entity): x_lamp skips it
 *     for entities beyond D m;
 *   - the two light-pool store calls inside the entity lights function (0x29B5CC, 0x29B62C; $f18 = the pool's
 *     draw distance, 40.0): x_lpool scales that distance.
 * Our own policy around the game's functions (which calls run, one distance argument); the functions themselves run
 * as translated. Facts used: camera 0x6FF190 (+20 matrix, position at +48; +4 position without one); entity +4
 * position / +20 matrix.
 */
#include <string.h>
#include "hle.h"
#include "hle_fast.h"
#include "hle_sky.h"

#define CE_CAMERA 0x006FF190u
#define CE_CLOCK_H 0x0066C02Cu
#define CE_CLOCK_M 0x0066C030u
#define CE_CLOCK_S 0x0066C034u

void ce_site_daycolour(R5900Ctx *c, uint32_t target);
void ce_site_coronas(R5900Ctx *c, uint32_t target);
void ce_site_lights(R5900Ctx *c, uint32_t target);
void ce_site_pool(R5900Ctx *c, uint32_t target);
void hle_ce_report(void);

static int ce_gen = -1;
static int o_clock, o_cbatch, o_lamp, o_lpool;
static float lamp_d2;
static uint64_t st_frames, st_clock_frames, st_corona_passes, st_corona_batched, st_lights, st_lights_skipped,
    st_pools, st_pools_scaled;

static void ce_read_keys(void)
{
    ce_gen = hle_x_gen;
    o_clock = hle_x_opt("clock", 0);
    o_cbatch = hle_x_opt("cbatch", 1);
    o_lamp = hle_x_opt("lamp", 0);
    o_lpool = hle_x_opt("lpool", 0);
    if (o_clock < 0 || o_clock > 2400 || o_clock % 100 > 59) o_clock = 0;
    if (o_lpool < 0 || o_lpool > 99) o_lpool = 0;
    lamp_d2 = o_lamp > 0 ? (float)o_lamp * (float)o_lamp : 0.0f;
    static int logged;
    if (!logged++)
        LOG("night levers: x_clock=%d x_cbatch=%d x_lamp=%d x_lpool=%d (0 = off; sites 0x243690 0x247010 "
            "0x22fabc 0x167ee0 0x29b5cc 0x29b62c)\n", o_clock, o_cbatch, o_lamp, o_lpool);
}

/* once per game step, before the time-of-day colour update */
void ce_site_daycolour(R5900Ctx *c, uint32_t target)
{
    if (ce_gen != hle_x_gen) ce_read_keys();
    st_frames++;
    if (o_clock > 0) {
        unsigned h = (unsigned)(o_clock / 100) % 24, m = (unsigned)(o_clock % 100);
        sb_(CE_CLOCK_H, (uint8_t)h);
        sb_(CE_CLOCK_M, (uint8_t)m);
        sh_(CE_CLOCK_S, 0);
        st_clock_frames++;
    }
    rc_dispatch(c, target);
}

void ce_site_coronas(R5900Ctx *c, uint32_t target)
{
    if (ce_gen != hle_x_gen) ce_read_keys();
    st_corona_passes++;
    if (o_cbatch > 0) {
        hle_sky_spr_batch(1);
        rc_dispatch(c, target);
        hle_sky_spr_batch(0);                    /* draws what is still pending */
        st_corona_batched++;
        return;
    }
    rc_dispatch(c, target);
}

void ce_site_lights(R5900Ctx *c, uint32_t target)
{
    st_lights++;
    if (lamp_d2 > 0.0f) {
        uint32_t e = (uint32_t)c->r[4].d[0];
        if (e >= 0x100000u && e + 0x40u < hle_ram_end) {
            uint32_t m = gw(e + 20), cm = gw(CE_CAMERA + 20);
            uint32_t a = m ? m + 48 : e + 4, ca = cm ? cm + 48 : CE_CAMERA + 4;
            if (a + 12u < hle_ram_end && ca + 12u < hle_ram_end) {
                float d2 = 0.0f;
                for (int i = 0; i < 3; i++) {
                    float p = u2f(gw(a + 4u * (uint32_t)i)) - u2f(gw(ca + 4u * (uint32_t)i));
                    d2 += p * p;
                }
                if (d2 > lamp_d2) { st_lights_skipped++; return; }    /* void function: nothing to return */
            }
        }
    }
    rc_dispatch(c, target);
}

void ce_site_pool(R5900Ctx *c, uint32_t target)
{
    st_pools++;
    if (o_lpool > 0) {
        float d = c->fpr[18].f * (float)o_lpool * 0.01f;
        c->fpr[18].f = d;
        st_pools_scaled++;
    }
    rc_dispatch(c, target);
}

void hle_ce_report(void)
{
    if (!st_frames) return;
    double f = (double)st_frames;
    LOG("night levers: x_clock=%d x_cbatch=%d x_lamp=%d x_lpool=%d (last values): %llu frames, clock held "
        "in %llu; per frame: corona passes %.2f (batched %.2f), entity-light calls %.2f (skipped by x_lamp %.2f), "
        "light-pool stores %.2f (distance scaled %.2f)\n", o_clock, o_cbatch, o_lamp, o_lpool,
        (unsigned long long)st_frames, (unsigned long long)st_clock_frames, (double)st_corona_passes / f,
        (double)st_corona_batched / f, (double)st_lights / f, (double)st_lights_skipped / f, (double)st_pools / f,
        (double)st_pools_scaled / f);
}
