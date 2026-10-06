/* A small 2D rigid-body engine for the gravity notes. See physics.c. */
#pragma once

typedef struct { double x, y; } PV;

typedef struct {
    PV pos, vel;            /* centre, px and px/s */
    double rot, angVel;     /* radians, rad/s */
    PV size;                /* full width and height */
    double invMass, invI, friction;
    int user;               /* the caller's tag (which note this is) */
} PBody;

#define PHYS_MAX_BODIES 512

void phys_reset(void);
/* mass 0 = static (floor, walls); returns the body index or -1 */
int phys_add(double cx, double cy, double w, double h, double mass, int user);
void phys_step(double dt, double gravity, double restitution, int iterations);
int phys_count(void);
PBody *phys_body(int i);

/* dragging: the grabbed body's anchor (in its own frame) is pulled to the target */
int phys_hit(PV p, PV *local);              /* topmost moving body under p, or -1 */
void phys_grab(int body, PV local, PV target);
void phys_drag_to(PV target);
void phys_release(void);
int phys_grabbed(void);                     /* -1 when nothing is held */
