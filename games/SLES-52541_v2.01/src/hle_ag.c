/* hle_ag.c - game-logic cuts behind poc6.cfg keys (default 0 = the original).
 *
 * x_agcam=R (host HLE_X_AGCAM=R, R in cm; 0 = off): the camera's world-collision cone cast at half rate.
 *   camera_cone_cast 0x2261C0 (called by the camera modes 0x20C370 / 0x225B30 from the camera update 0x201B20)
 *   computes where the camera may stand between a start point a2 (ARG 2: the point the camera looks from, near the
 *   player) and the wanted camera position a1 (ARG 1). When the points differ it casts a sphere cone against the world
 *   collision 0x1286A0 (a bisection over the buildings / objects near the segment, ~220 sphere-cast triangle tests
 *   per frame, each with one VU0 program #0 call). Result: v0 = hit, ARG 3 = the camera position a2 + f (a1 - a2),
 *   ARG 4 = f (1.0 without a hit); on a hit 0x1286A0 also stores a radius product in 0x66BFA8. Measured on a PSP:
 *   ~2.2-2.5 ms of the camera update's 3.1 ms per frame.
 *   With x_agcam=R the cast runs on one call and the next call of the same caller (same output pointers) in the next
 *   frame reuses its result (hit, f, the 0x66BFA8 value) on the new points - if neither point moved more than R cm
 *   since the computed call; then the cast runs again. Visible: at most one frame (40 ms) of a camera that is a few
 *   cm too far from the player when a wall appears between them; while driving / running fast (moves > R) every frame
 *   is computed as before. Guest effect: the world's scan code and the cone cast's stack lists are not touched on the
 *   reused calls (the scan code is only compared for equality).
 */
#include "hle.h"
#include "hle_fast.h"

#define A_CONE_RADIUS 0x0066BFA8u          /* written by 0x1286A0 on a hit */

void GUEST(002261c0)(R5900Ctx *c);

static int agcam = -2;                     /* x_agcam: -2 = not read yet */
static int agcam_gen = -1;                 /* hle_x_gen when x_agcam was read (run-time A/B switches) */
static int agcam_on;                       /* the last value > 0 (the report after run-time switches) */
static float agcam_d2;
typedef struct {
    uint32_t out_pos, out_frac;            /* the caller's output pointers = its identity */
    uint32_t frame;                        /* the game timer's frame counter at the last call */
    int computed;                          /* the last call ran the cast (1) or reused (0) */
    uint32_t p1[3], p2[3];                 /* the points of the computed call */
    uint32_t hit, frac, rad;
} AgCamSlot;
static AgCamSlot cam_slot[4];
static uint64_t cam_calls, cam_reused, cam_far, cam_hits;

static int moved_less(const uint32_t *a, uint32_t p)
{
    float d2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        float d = u2f(gw(p + 4u * i)) - u2f(a[i]);
        d2 += d * d;
    }
    return d2 < agcam_d2;                  /* NaN: not less -> computed */
}

HLE(002261c0)
{
    if (agcam_gen != hle_x_gen) {
        int was = agcam;
        agcam_gen = hle_x_gen;
        agcam = hle_x_opt("agcam", 0);
        if (agcam > 0) agcam_on = agcam;
        agcam_d2 = agcam > 0 ? ((float)agcam * 0.01f) * ((float)agcam * 0.01f) : 0.0f;
        if (was != -2 && was != agcam) memset(cam_slot, 0, sizeof cam_slot);   /* a switch starts clean */
    }
    if (agcam <= 0) {
        GUEST(002261c0)(c);
        return;
    }
    const uint32_t p1 = ARG(1), p2 = ARG(2), out_pos = ARG(3), out_frac = ARG(4);
    const uint32_t frame = gw((uint32_t)c->r[28].d[0] + 2376u);   /* the game timer's frame counter (gp + 2376) */
    cam_calls++;
    AgCamSlot *s = NULL, *freeslot = NULL;
    for (int i = 0; i < 4; i++) {
        if (cam_slot[i].out_pos == out_pos && cam_slot[i].out_frac == out_frac) { s = &cam_slot[i]; break; }
        if (!freeslot && !cam_slot[i].out_pos) freeslot = &cam_slot[i];
    }
    if (!s) s = freeslot ? freeslot : &cam_slot[(unsigned)cam_calls & 3u];
    /* the original's equal-points early return (no writes) stays the original */
    int same = F(gw(p1)) == F(gw(p2)) && F(gw(p1 + 4)) == F(gw(p2 + 4)) && F(gw(p1 + 8)) == F(gw(p2 + 8));
    if (!same && s->out_pos == out_pos && s->out_frac == out_frac && s->computed && s->frame + 1u == frame) {
        if (moved_less(s->p1, p1) && moved_less(s->p2, p2)) {
            if (s->hit) {
                /* as 0x2261C0 after a hit: pos = p2 + f (p1 - p2), the FPU's sub / mul / add per lane */
                uint32_t f = s->frac;
                sw_(out_frac, f);
                sw_(A_CONE_RADIUS, s->rad);
                for (int i = 0; i < 3; i++) {
                    uint32_t b = gw(p2 + 4u * i);
                    sw_(out_pos + 4u * i, fpu_add(b, fpu_mul(f, fpu_sub(gw(p1 + 4u * i), b))));
                }
                cam_hits++;
            } else {
                for (int i = 0; i < 3; i++) sw_(out_pos + 4u * i, gw(p1 + 4u * i));
                sw_(out_frac, 0x3F800000u);
            }
            RET(s->hit);
            s->computed = 0;
            s->frame = frame;
            cam_reused++;
            return;
        }
        cam_far++;
    }
    GUEST(002261c0)(c);
    if (same) return;
    s->out_pos = out_pos;
    s->out_frac = out_frac;
    s->frame = frame;
    s->computed = 1;
    for (int i = 0; i < 3; i++) {
        s->p1[i] = gw(p1 + 4u * i);
        s->p2[i] = gw(p2 + 4u * i);
    }
    s->hit = (uint32_t)c->r[2].d[0] & 0xFFu;
    s->frac = gw(out_frac);
    s->rad = gw(A_CONE_RADIUS);
}

void hle_ag_report(void)
{
    if (agcam > 0 || cam_calls)                                 /* also after run-time switches */

        LOG("x_agcam=%d (camera cone cast at half rate; counted while on): %llu calls, %llu reused (%.1f%%; %llu of them with a "
            "hit), %llu computed because a point moved more than %d cm\n", agcam, (unsigned long long)cam_calls,
            (unsigned long long)cam_reused, cam_calls ? 100.0 * (double)cam_reused / (double)cam_calls : 0.0,
            (unsigned long long)cam_hits, (unsigned long long)cam_far, agcam_on);
}
