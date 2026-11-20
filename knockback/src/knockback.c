
/* knockback: directional knockback + hitstun on a fixed step.
 * - knockback velocity = normalized attack direction * configurable speed
 * - hitstun length and velocity decay curve (linear / expo) are configurable
 * - actions are locked while stunned
 * - walls clamp position and kill the normal velocity component (slide)
 * - stacking rule: velocity is REPLACED by the new hit,
 *   stun timer takes max(remaining, new)
 * - invalid specs (negative stun, speed over cap, zero direction) are rejected
 */
#include <stdio.h>
#include <math.h>

#define SIM_DT (1.0 / 60.0)  /* fixed step, seconds */
#define SPEED_CAP 40.0       /* max knockback speed, units/s */

typedef enum { CURVE_LINEAR = 0, CURVE_EXPO = 1 } Curve;

typedef struct {
    double speed;     /* initial knockback speed, 0..SPEED_CAP */
    int stun_frames;  /* hitstun length in fixed frames, >= 0 */
    Curve curve;      /* velocity decay curve while stunned */
    double decay;     /* CURVE_EXPO only: per-frame retention, (0,1] */
} HitSpec;

typedef struct {
    double minx, miny, maxx, maxy;
} Arena;

typedef struct {
    double x, y;
    double vx, vy;
    int stun_left;
    Curve curve;
    double decay;
} Fighter;

static void fighter_init(Fighter* f, double x, double y) {
    f->x = x; f->y = y;
    f->vx = 0.0; f->vy = 0.0;
    f->stun_left = 0;
    f->curve = CURVE_LINEAR;
    f->decay = 1.0;
}

/* returns 0 on hit, -1 if the spec is rejected (fighter untouched) */
static int apply_hit(Fighter* f, const HitSpec* h, double dirx, double diry) {
    double len;
    if (h->stun_frames < 0) return -1;
    if (!(h->speed >= 0.0) || h->speed > SPEED_CAP) return -1;
    if (h->curve == CURVE_EXPO && !(h->decay > 0.0 && h->decay <= 1.0)) return -1;
    len = hypot(dirx, diry);
    if (len <= 0.0) return -1;

    f->vx = dirx / len * h->speed;
    f->vy = diry / len * h->speed;
    f->stun_left = h->stun_frames > f->stun_left ? h->stun_frames : f->stun_left;
    f->curve = h->curve;
    f->decay = h->decay;
    return 0;
}

static int fighter_can_act(const Fighter* f) { return f->stun_left == 0; }

/* voluntary movement; ignored during hitstun */
static int try_move(Fighter* f, const Arena* a, double mx, double my) {
    if (!fighter_can_act(f)) return -1;
    f->x += mx; f->y += my;
    if (f->x < a->minx) f->x = a->minx;
    if (f->x > a->maxx) f->x = a->maxx;
    if (f->y < a->miny) f->y = a->miny;
    if (f->y > a->maxy) f->y = a->maxy;
    return 0;
}

static void fighter_tick(Fighter* f, const Arena* a) {
    double nx, ny;
    if (f->stun_left <= 0) { f->vx = 0.0; f->vy = 0.0; return; }

    nx = f->x + f->vx * SIM_DT;
    ny = f->y + f->vy * SIM_DT;
    /* wall slide: clamp to the arena and zero the normal component,
     * the tangential component survives so the fighter slides along */
    if (nx < a->minx) { nx = a->minx; f->vx = 0.0; }
    if (nx > a->maxx) { nx = a->maxx; f->vx = 0.0; }
    if (ny < a->miny) { ny = a->miny; f->vy = 0.0; }
    if (ny > a->maxy) { ny = a->maxy; f->vy = 0.0; }
    f->x = nx; f->y = ny;

    f->stun_left--;
    if (f->stun_left == 0) {
        f->vx = 0.0; f->vy = 0.0;  /* stun over: velocity exactly zero */
    } else if (f->curve == CURVE_LINEAR) {
        /* v after k of T frames = v0*(T-k)/T  <=>  per-frame factor k/(k+1) */
        double s = (double)f->stun_left / (double)(f->stun_left + 1);
        f->vx *= s; f->vy *= s;
    } else {
        f->vx *= f->decay; f->vy *= f->decay;
    }
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    Arena wide = { -100.0, -100.0, 100.0, 100.0 };
    Fighter f;

    /* direction -> velocity: (3,4) normalizes to (0.6,0.8) */
    fighter_init(&f, 0.0, 0.0);
    {
        HitSpec h = { 10.0, 5, CURVE_LINEAR, 1.0 };
        check("hit_accepted", apply_hit(&f, &h, 3.0, 4.0) == 0);
        check("dir_normalized", fabs(f.vx - 6.0) < 1e-12 && fabs(f.vy - 8.0) < 1e-12);
        check("speed_preserved", fabs(hypot(f.vx, f.vy) - 10.0) < 1e-12);
    }

    /* hitstun locks actions for exactly stun_frames ticks */
    {
        int locked = 0, k;
        double px = f.x;
        check("stun_blocks_act", !fighter_can_act(&f));
        check("move_rejected", try_move(&f, &wide, 1.0, 0.0) == -1 && f.x == px);
        for (k = 0; k < 5; k++) {
            if (!fighter_can_act(&f)) locked++;
            fighter_tick(&f, &wide);
        }
        check("locked_exactly_stun", locked == 5);
        check("act_after_stun", fighter_can_act(&f));
        check("move_after_stun", try_move(&f, &wide, 1.0, 0.0) == 0);
    }

    /* linear curve vs hand-computed displacement; velocity zero at frame T */
    fighter_init(&f, 0.0, 0.0);
    {
        HitSpec h = { 12.0, 6, CURVE_LINEAR, 1.0 };
        double expect_x = 0.0, expect_v = 12.0;
        int zero_frame = -1, curve_ok = 1, k;
        apply_hit(&f, &h, 1.0, 0.0);
        for (k = 1; k <= 8; k++) {
            fighter_tick(&f, &wide);
            if (k <= 6) {
                /* tick k integrates v0*(T-(k-1))/T, then decays to v0*(T-k)/T */
                expect_x += expect_v * SIM_DT;
                expect_v = 12.0 * (6 - k) / 6.0;
                if (fabs(f.x - expect_x) > 1e-9) curve_ok = 0;
                if (fabs(f.vx - expect_v) > 1e-9) curve_ok = 0;
            }
            if (f.vx == 0.0 && zero_frame < 0) zero_frame = k;
        }
        check("linear_matches_hand", curve_ok);
        check("zero_at_stun_frame", zero_frame == 6);
        check("stays_zero", f.vx == 0.0 && f.stun_left == 0);
    }

    /* expo curve vs hand-computed displacement; hard zero at stun end */
    fighter_init(&f, 0.0, 0.0);
    {
        HitSpec h = { 8.0, 4, CURVE_EXPO, 0.5 };
        double expect_x = 0.0, expect_v = 8.0;
        int zero_frame = -1, curve_ok = 1, k;
        apply_hit(&f, &h, 1.0, 0.0);
        for (k = 1; k <= 6; k++) {
            fighter_tick(&f, &wide);
            if (k <= 4) {
                expect_x += expect_v * SIM_DT;
                expect_v *= 0.5;
                if (fabs(f.x - expect_x) > 1e-12) curve_ok = 0;
                if (k < 4 && fabs(f.vx - expect_v) > 1e-12) curve_ok = 0;
            }
            if (f.vx == 0.0 && zero_frame < 0) zero_frame = k;
        }
        check("expo_matches_hand", curve_ok);
        check("expo_zero_at_stun", zero_frame == 4);
    }

    /* wall slide: clamp on the normal axis, keep sliding tangentially */
    fighter_init(&f, 0.9, 0.0);
    {
        Arena box = { -1.0, -1.0, 1.0, 1.0 };
        HitSpec h = { 12.0, 30, CURVE_LINEAR, 1.0 };
        int inside = 1, slid = 0, k;
        double y_at_clamp = -1.0;
        apply_hit(&f, &h, 1.0, 1.0);  /* 45 degrees into the +x wall */
        for (k = 0; k < 30; k++) {
            fighter_tick(&f, &box);
            if (f.x > 1.0 + 1e-12 || f.y > 1.0 + 1e-12) inside = 0;
            if (f.x == 1.0 && y_at_clamp < 0.0) y_at_clamp = f.y;
            else if (y_at_clamp >= 0.0 && f.y > y_at_clamp + 1e-12) slid = 1;
        }
        check("wall_no_penetration", inside);
        check("wall_clamped", f.x == 1.0);
        check("wall_normal_killed", f.vx == 0.0);
        check("wall_slid_along", slid);
    }

    /* stacking: velocity replaced, stun = max(remaining, new) */
    fighter_init(&f, 0.0, 0.0);
    {
        HitSpec a = { 10.0, 10, CURVE_LINEAR, 1.0 };
        HitSpec weaker = { 4.0, 3, CURVE_LINEAR, 1.0 };
        HitSpec longer = { 6.0, 20, CURVE_LINEAR, 1.0 };
        int k;
        apply_hit(&f, &a, 1.0, 0.0);
        for (k = 0; k < 4; k++) fighter_tick(&f, &wide);  /* stun_left: 10 -> 6 */
        apply_hit(&f, &weaker, 0.0, 1.0);
        check("stack_keeps_longer_stun", f.stun_left == 6);
        check("stack_replaces_velocity", f.vx == 0.0 && fabs(f.vy - 4.0) < 1e-12);
        apply_hit(&f, &longer, 0.0, -1.0);
        check("stack_extends_stun", f.stun_left == 20);
        check("stack_replaces_again", fabs(f.vy - -6.0) < 1e-12);
    }

    /* invalid specs are rejected and leave the fighter untouched */
    fighter_init(&f, 1.0, 2.0);
    {
        HitSpec neg_stun = { 5.0, -1, CURVE_LINEAR, 1.0 };
        HitSpec over_cap = { SPEED_CAP + 0.1, 5, CURVE_LINEAR, 1.0 };
        HitSpec neg_speed = { -1.0, 5, CURVE_LINEAR, 1.0 };
        HitSpec ok = { 5.0, 5, CURVE_LINEAR, 1.0 };
        HitSpec bad_decay = { 5.0, 5, CURVE_EXPO, 1.5 };
        check("neg_stun_rejected", apply_hit(&f, &neg_stun, 1.0, 0.0) == -1);
        check("over_cap_rejected", apply_hit(&f, &over_cap, 1.0, 0.0) == -1);
        check("neg_speed_rejected", apply_hit(&f, &neg_speed, 1.0, 0.0) == -1);
        check("zero_dir_rejected", apply_hit(&f, &ok, 0.0, 0.0) == -1);
        check("bad_decay_rejected", apply_hit(&f, &bad_decay, 1.0, 0.0) == -1);
        check("reject_keeps_state", f.x == 1.0 && f.y == 2.0 && f.stun_left == 0);
        check("at_cap_accepted", apply_hit(&f, &ok, 1.0, 0.0) == 0);
    }

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
