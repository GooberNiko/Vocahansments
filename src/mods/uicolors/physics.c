/*
 * Rigid boxes for the gravity notes: rotation, friction, restitution and
 * stable stacking. It follows Erin Catto's Box2D-Lite: box-box contacts by
 * clipping, sequential impulses with accumulated, warm-started impulses.
 * Screen coordinates: y grows downwards and gravity is +y.
 */
#include <math.h>
#include <string.h>
#include "physics.h"

typedef struct { PV c1, c2; } M22;      /* columns */

static PV v(double x, double y) { PV r = { x, y }; return r; }
static PV add(PV a, PV b) { return v(a.x + b.x, a.y + b.y); }
static PV sub(PV a, PV b) { return v(a.x - b.x, a.y - b.y); }
static PV mul(double s, PV a) { return v(s * a.x, s * a.y); }
static PV neg(PV a) { return v(-a.x, -a.y); }
static PV vabs(PV a) { return v(fabs(a.x), fabs(a.y)); }
static double dot(PV a, PV b) { return a.x * b.x + a.y * b.y; }
static double cross(PV a, PV b) { return a.x * b.y - a.y * b.x; }
static PV cross_vs(PV a, double s) { return v(s * a.y, -s * a.x); }
static PV cross_sv(double s, PV a) { return v(-s * a.y, s * a.x); }

static M22 rotm(double a) { double c = cos(a), s = sin(a); M22 m = { { c, s }, { -s, c } }; return m; }
static M22 transpose(M22 m) { M22 t = { { m.c1.x, m.c2.x }, { m.c1.y, m.c2.y } }; return t; }
static PV mv(M22 m, PV a) { return v(m.c1.x * a.x + m.c2.x * a.y, m.c1.y * a.x + m.c2.y * a.y); }
static M22 mm(M22 a, M22 b) { M22 r = { mv(a, b.c1), mv(a, b.c2) }; return r; }
static M22 mabs(M22 m) { M22 r = { vabs(m.c1), vabs(m.c2) }; return r; }

/* ------------------------------------------------------------ bodies -- */

static PBody g_b[PHYS_MAX_BODIES];
static int g_nb;

void phys_reset(void) { g_nb = 0; phys_release(); }
int phys_count(void) { return g_nb; }
PBody *phys_body(int i) { return i >= 0 && i < g_nb ? &g_b[i] : 0; }

int phys_add(double cx, double cy, double w, double h, double mass, int user)
{
    if (g_nb >= PHYS_MAX_BODIES || w <= 0 || h <= 0) return -1;
    PBody *b = &g_b[g_nb];
    memset(b, 0, sizeof *b);
    b->pos = v(cx, cy);
    b->size = v(w, h);
    b->friction = 0.5;
    b->user = user;
    if (mass > 0) {
        b->invMass = 1 / mass;
        b->invI = 1 / (mass * (w * w + h * h) / 12);
    }
    return g_nb++;
}

/* ---------------------------------------------------------- contacts -- */

enum { NO_EDGE = 0, EDGE1, EDGE2, EDGE3, EDGE4 };
typedef union { struct { char in1, out1, in2, out2; } e; int key; } Feature;
typedef struct { PV v; Feature fp; } ClipV;

typedef struct {
    PV pos, normal, r1, r2;
    double sep, Pn, Pt, massN, massT, bias, bounce;
    Feature fp;
} Contact;

static int clip_segment(ClipV out[2], const ClipV in[2], PV n, double offset, char clipEdge)
{
    int k = 0;
    double d0 = dot(n, in[0].v) - offset, d1 = dot(n, in[1].v) - offset;
    if (d0 <= 0) out[k++] = in[0];
    if (d1 <= 0) out[k++] = in[1];
    if (d0 * d1 < 0) {
        double f = d0 / (d0 - d1);
        out[k].v = add(in[0].v, mul(f, sub(in[1].v, in[0].v)));
        if (d0 > 0) { out[k].fp = in[0].fp; out[k].fp.e.in1 = clipEdge; out[k].fp.e.in2 = NO_EDGE; }
        else { out[k].fp = in[1].fp; out[k].fp.e.out1 = clipEdge; out[k].fp.e.out2 = NO_EDGE; }
        k++;
    }
    return k;
}

static void incident_edge(ClipV c[2], PV h, PV pos, M22 R, PV normal)
{
    PV n = neg(mv(transpose(R), normal)), na = vabs(n);
    memset(c, 0, 2 * sizeof *c);
    if (na.x > na.y) {
        if (n.x > 0) { c[0].v = v(h.x, -h.y); c[0].fp.e.in2 = EDGE3; c[0].fp.e.out2 = EDGE4; c[1].v = v(h.x, h.y); c[1].fp.e.in2 = EDGE4; c[1].fp.e.out2 = EDGE1; }
        else { c[0].v = v(-h.x, h.y); c[0].fp.e.in2 = EDGE1; c[0].fp.e.out2 = EDGE2; c[1].v = v(-h.x, -h.y); c[1].fp.e.in2 = EDGE2; c[1].fp.e.out2 = EDGE3; }
    } else {
        if (n.y > 0) { c[0].v = v(h.x, h.y); c[0].fp.e.in2 = EDGE4; c[0].fp.e.out2 = EDGE1; c[1].v = v(-h.x, h.y); c[1].fp.e.in2 = EDGE1; c[1].fp.e.out2 = EDGE2; }
        else { c[0].v = v(-h.x, -h.y); c[0].fp.e.in2 = EDGE2; c[0].fp.e.out2 = EDGE3; c[1].v = v(h.x, -h.y); c[1].fp.e.in2 = EDGE3; c[1].fp.e.out2 = EDGE4; }
    }
    c[0].v = add(pos, mv(R, c[0].v));
    c[1].v = add(pos, mv(R, c[1].v));
}

static int collide(Contact *out, const PBody *A, const PBody *B)
{
    PV hA = mul(0.5, A->size), hB = mul(0.5, B->size), pA = A->pos, pB = B->pos;
    M22 RA = rotm(A->rot), RB = rotm(B->rot), RAT = transpose(RA), RBT = transpose(RB);
    PV dp = sub(pB, pA), dA = mv(RAT, dp), dB = mv(RBT, dp);
    M22 C = mm(RAT, RB), absC = mabs(C), absCT = transpose(absC);

    PV faceA = sub(sub(vabs(dA), hA), mv(absC, hB));
    if (faceA.x > 0 || faceA.y > 0) return 0;
    PV faceB = sub(sub(vabs(dB), mv(absCT, hA)), hB);
    if (faceB.x > 0 || faceB.y > 0) return 0;

    enum { FAX, FAY, FBX, FBY } axis = FAX;
    double sep = faceA.x;
    PV normal = dA.x > 0 ? RA.c1 : neg(RA.c1);
    const double rel = 0.95, abs_ = 0.01;
    if (faceA.y > rel * sep + abs_ * hA.y) { axis = FAY; sep = faceA.y; normal = dA.y > 0 ? RA.c2 : neg(RA.c2); }
    if (faceB.x > rel * sep + abs_ * hB.x) { axis = FBX; sep = faceB.x; normal = dB.x > 0 ? RB.c1 : neg(RB.c1); }
    if (faceB.y > rel * sep + abs_ * hB.y) { axis = FBY; sep = faceB.y; normal = dB.y > 0 ? RB.c2 : neg(RB.c2); }

    PV front_n, side_n;
    ClipV inc[2];
    double front, negSide, posSide, side;
    char negEdge, posEdge;
    switch (axis) {
    case FAX:
        front_n = normal; front = dot(pA, front_n) + hA.x; side_n = RA.c2; side = dot(pA, side_n);
        negSide = -side + hA.y; posSide = side + hA.y; negEdge = EDGE3; posEdge = EDGE1;
        incident_edge(inc, hB, pB, RB, front_n);
        break;
    case FAY:
        front_n = normal; front = dot(pA, front_n) + hA.y; side_n = RA.c1; side = dot(pA, side_n);
        negSide = -side + hA.x; posSide = side + hA.x; negEdge = EDGE2; posEdge = EDGE4;
        incident_edge(inc, hB, pB, RB, front_n);
        break;
    case FBX:
        front_n = neg(normal); front = dot(pB, front_n) + hB.x; side_n = RB.c2; side = dot(pB, side_n);
        negSide = -side + hB.y; posSide = side + hB.y; negEdge = EDGE3; posEdge = EDGE1;
        incident_edge(inc, hA, pA, RA, front_n);
        break;
    default:
        front_n = neg(normal); front = dot(pB, front_n) + hB.y; side_n = RB.c1; side = dot(pB, side_n);
        negSide = -side + hB.x; posSide = side + hB.x; negEdge = EDGE2; posEdge = EDGE4;
        incident_edge(inc, hA, pA, RA, front_n);
        break;
    }

    ClipV c1[2], c2[2];
    if (clip_segment(c1, inc, neg(side_n), negSide, negEdge) < 2) return 0;
    if (clip_segment(c2, c1, side_n, posSide, posEdge) < 2) return 0;

    int n = 0;
    for (int i = 0; i < 2; i++) {
        double s = dot(front_n, c2[i].v) - front;
        if (s > 0) continue;
        memset(&out[n], 0, sizeof out[n]);
        out[n].sep = s;
        out[n].normal = normal;
        out[n].pos = sub(c2[i].v, mul(s, front_n));
        out[n].fp = c2[i].fp;
        if (axis == FBX || axis == FBY) {
            char t = out[n].fp.e.in1; out[n].fp.e.in1 = out[n].fp.e.in2; out[n].fp.e.in2 = t;
            t = out[n].fp.e.out1; out[n].fp.e.out1 = out[n].fp.e.out2; out[n].fp.e.out2 = t;
        }
        n++;
    }
    return n;
}

/* --------------------------------------------------------- arbiters -- */

typedef struct { int a, b, n; double friction; Contact c[2]; } Arbiter;
#define MAX_ARB 8192
static Arbiter g_arb[2][MAX_ARB];
static int g_narb[2], g_cur;

static Arbiter *find_old(int a, int b)
{
    /* the old list is sorted by (a, b): pairs are visited in that order */
    Arbiter *o = g_arb[g_cur ^ 1];
    int lo = 0, hi = g_narb[g_cur ^ 1] - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (o[m].a == a && o[m].b == b) return &o[m];
        if (o[m].a < a || (o[m].a == a && o[m].b < b)) lo = m + 1; else hi = m - 1;
    }
    return 0;
}

static void broadphase(void)
{
    g_cur ^= 1;
    Arbiter *list = g_arb[g_cur];
    int n = 0;
    for (int i = 0; i < g_nb; i++) {
        PBody *A = &g_b[i];
        double ra = 0.5 * hypot(A->size.x, A->size.y);
        for (int j = i + 1; j < g_nb && n < MAX_ARB; j++) {
            PBody *B = &g_b[j];
            if (A->invMass == 0 && B->invMass == 0) continue;
            double rb = 0.5 * hypot(B->size.x, B->size.y);
            if (fabs(A->pos.x - B->pos.x) > ra + rb || fabs(A->pos.y - B->pos.y) > ra + rb) continue;
            Arbiter *ar = &list[n];
            ar->a = i; ar->b = j;
            ar->n = collide(ar->c, A, B);
            if (!ar->n) continue;
            ar->friction = sqrt(A->friction * B->friction);
            /* warm start: contacts that persist keep their impulses */
            Arbiter *old = find_old(i, j);
            for (int k = 0; old && k < ar->n; k++)
                for (int m = 0; m < old->n; m++)
                    if (old->c[m].fp.key == ar->c[k].fp.key) { ar->c[k].Pn = old->c[m].Pn; ar->c[k].Pt = old->c[m].Pt; break; }
            n++;
        }
    }
    g_narb[g_cur] = n;
}

/* ------------------------------------------------------------- drag -- */

static int g_grab = -1;
static PV g_grabLocal, g_grabTarget;

int phys_grabbed(void) { return g_grab; }
void phys_release(void) { g_grab = -1; }
void phys_drag_to(PV t) { g_grabTarget = t; }

void phys_grab(int body, PV local, PV target)
{
    if (body < 0 || body >= g_nb || g_b[body].invMass == 0) return;
    g_grab = body; g_grabLocal = local; g_grabTarget = target;
}

int phys_hit(PV p, PV *local)
{
    for (int i = g_nb - 1; i >= 0; i--) {
        PBody *b = &g_b[i];
        if (b->invMass == 0) continue;
        PV l = mv(transpose(rotm(b->rot)), sub(p, b->pos));
        if (fabs(l.x) <= b->size.x / 2 + 2 && fabs(l.y) <= b->size.y / 2 + 2) { if (local) *local = l; return i; }
    }
    return -1;
}

/* soft point constraint pulling the grabbed anchor towards the mouse */
static void drag_impulse(double inv_dt)
{
    if (g_grab < 0) return;
    PBody *b = &g_b[g_grab];
    PV r = mv(rotm(b->rot), g_grabLocal), p = add(b->pos, r);
    PV vp = add(b->vel, cross_sv(b->angVel, r));
    double im = b->invMass, iI = b->invI;
    double k11 = im + iI * r.y * r.y, k12 = -iI * r.x * r.y, k22 = im + iI * r.x * r.x, det = k11 * k22 - k12 * k12;
    if (fabs(det) < 1e-12) return;
    PV want = sub(mul(0.25 * inv_dt, sub(g_grabTarget, p)), vp);   /* close a quarter of the gap per step */
    PV P = v((k22 * want.x - k12 * want.y) / det, (k11 * want.y - k12 * want.x) / det);
    P = mul(0.5, P);                                                 /* soft: spread over the iterations */
    b->vel = add(b->vel, mul(im, P));
    b->angVel += iI * cross(r, P);
}

/* ------------------------------------------------------------- step -- */

void phys_step(double dt, double gravity, double restitution, int iterations)
{
    if (dt <= 0) return;
    double inv_dt = 1 / dt;
    broadphase();
    Arbiter *list = g_arb[g_cur];
    int na = g_narb[g_cur];

    for (int i = 0; i < g_nb; i++) {
        PBody *b = &g_b[i];
        if (b->invMass == 0) continue;
        b->vel.y += dt * gravity;
        b->angVel *= 0.995;                    /* a little drag so piles settle */
    }

    /* pre-step: effective masses, position bias, bounce, warm start */
    for (int a = 0; a < na; a++) {
        Arbiter *ar = &list[a];
        PBody *b1 = &g_b[ar->a], *b2 = &g_b[ar->b];
        for (int k = 0; k < ar->n; k++) {
            Contact *c = &ar->c[k];
            c->r1 = sub(c->pos, b1->pos); c->r2 = sub(c->pos, b2->pos);
            double rn1 = dot(c->r1, c->normal), rn2 = dot(c->r2, c->normal);
            double kN = b1->invMass + b2->invMass + b1->invI * (dot(c->r1, c->r1) - rn1 * rn1) + b2->invI * (dot(c->r2, c->r2) - rn2 * rn2);
            c->massN = 1 / kN;
            PV t = cross_vs(c->normal, 1);
            double rt1 = dot(c->r1, t), rt2 = dot(c->r2, t);
            double kT = b1->invMass + b2->invMass + b1->invI * (dot(c->r1, c->r1) - rt1 * rt1) + b2->invI * (dot(c->r2, c->r2) - rt2 * rt2);
            c->massT = 1 / kT;
            c->bias = -0.2 * inv_dt * fmin(0, c->sep + 0.5);
            PV dv = sub(add(b2->vel, cross_sv(b2->angVel, c->r2)), add(b1->vel, cross_sv(b1->angVel, c->r1)));
            double vn = dot(dv, c->normal);
            c->bounce = vn < -60 ? -restitution * vn : 0;
            PV P = add(mul(c->Pn, c->normal), mul(c->Pt, t));
            b1->vel = sub(b1->vel, mul(b1->invMass, P)); b1->angVel -= b1->invI * cross(c->r1, P);
            b2->vel = add(b2->vel, mul(b2->invMass, P)); b2->angVel += b2->invI * cross(c->r2, P);
        }
    }

    for (int it = 0; it < iterations; it++) {
        drag_impulse(inv_dt);
        for (int a = 0; a < na; a++) {
            Arbiter *ar = &list[a];
            PBody *b1 = &g_b[ar->a], *b2 = &g_b[ar->b];
            for (int k = 0; k < ar->n; k++) {
                Contact *c = &ar->c[k];
                PV dv = sub(add(b2->vel, cross_sv(b2->angVel, c->r2)), add(b1->vel, cross_sv(b1->angVel, c->r1)));
                double vn = dot(dv, c->normal);
                double dPn = c->massN * (-vn + fmax(c->bias, c->bounce));
                double Pn0 = c->Pn;
                c->Pn = fmax(Pn0 + dPn, 0);
                dPn = c->Pn - Pn0;
                PV P = mul(dPn, c->normal);
                b1->vel = sub(b1->vel, mul(b1->invMass, P)); b1->angVel -= b1->invI * cross(c->r1, P);
                b2->vel = add(b2->vel, mul(b2->invMass, P)); b2->angVel += b2->invI * cross(c->r2, P);

                dv = sub(add(b2->vel, cross_sv(b2->angVel, c->r2)), add(b1->vel, cross_sv(b1->angVel, c->r1)));
                PV t = cross_vs(c->normal, 1);
                double dPt = c->massT * -dot(dv, t), maxPt = ar->friction * c->Pn, Pt0 = c->Pt;
                c->Pt = fmax(-maxPt, fmin(Pt0 + dPt, maxPt));
                dPt = c->Pt - Pt0;
                P = mul(dPt, t);
                b1->vel = sub(b1->vel, mul(b1->invMass, P)); b1->angVel -= b1->invI * cross(c->r1, P);
                b2->vel = add(b2->vel, mul(b2->invMass, P)); b2->angVel += b2->invI * cross(c->r2, P);
            }
        }
    }

    for (int i = 0; i < g_nb; i++) {
        PBody *b = &g_b[i];
        if (b->invMass == 0) continue;
        double sp = hypot(b->vel.x, b->vel.y);
        if (sp > 6000) b->vel = mul(6000 / sp, b->vel);      /* nothing gets thrown through a wall */
        if (b->angVel > 40) b->angVel = 40;
        if (b->angVel < -40) b->angVel = -40;
        b->pos = add(b->pos, mul(dt, b->vel));
        b->rot += dt * b->angVel;
    }
}
