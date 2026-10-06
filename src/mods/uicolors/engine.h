/* UI Colors - colour remapping engine shared between the hook layer and the editor dialog. */
#pragma once
#include <windows.h>
#include "../../common/vh_api.h"

extern const VHApi *A;

/* Areas of the editor a colour can be overridden for. AREA_ALL is the global table. */
enum {
    AREA_ALL = 0,
    AREA_MUSICAL,     /* MUSICAL EDITOR view: piano roll, rulers, control tracks */
    AREA_TRACK,       /* TRACK EDITOR view */
    AREA_MIXER,       /* MIXER window */
    AREA_BARS,        /* tool / transport bars */
    AREA_OTHER,       /* frames, dialogs, everything else */
    AREA_BITMAPS,     /* pixels of the editor's built-in bitmaps (icons, buttons) */
    AREA_SPRITES,     /* opaque pixels of masked sprites (vibrato squiggle, note symbols) */
    AREA_NOTETEXT,    /* lyric text on notes, keyed by the note's fill colour */
    AREA_COUNT
};
extern const wchar_t *const kAreaNames[AREA_COUNT];

/* What a colour is used for. A theme table is (area, kind); lookups fall back
 * area+kind -> area -> All+kind -> All. Sections: [Musical], [Musical.Lines]... */
enum { KIND_ANY = 0, KIND_FILL, KIND_LINE, KIND_TEXT, KIND_COUNT };
extern const wchar_t *const kKindNames[KIND_COUNT];
#define TBL(area, kind) ((area) * KIND_COUNT + (kind))
#define TBL_COUNT (AREA_COUNT * KIND_COUNT)

#define NO_COLOR 0xFFFFFFFFu

/* theme: original RGB -> replacement RGB per table (COLORREF, 0x00BBGGRR) */
COLORREF theme_lookup(int tbl, COLORREF orig);   /* NO_COLOR if unmapped */
void theme_set(int tbl, COLORREF orig, COLORREF repl);
void theme_clear_entry(int tbl, COLORREF orig);
void theme_clear(void);
BOOL theme_load(const wchar_t *path, wchar_t *nameOut, int nameLen);
BOOL theme_save(const wchar_t *path, const wchar_t *name);
int theme_count(int tbl);
BOOL theme_entry(int tbl, int index, COLORREF *orig, COLORREF *repl);

COLORREF remap(int area, int kind, COLORREF c);

/* gradient replacements (fills only; lines and text use the first colour) */
enum { GRAD_H = 0, GRAD_V = 1 };                 /* left to right, top to bottom */
void theme_set_grad(int tbl, COLORREF orig, COLORREF repl, COLORREF c2, int dir);   /* c2 NO_COLOR = solid */
BOOL theme_lookup_grad(int tbl, COLORREF orig, COLORREF *c2, int *dir);
BOOL remap_grad(int area, int kind, COLORREF c, COLORREF *c1, COLORREF *c2, int *dir);

/* rainbow notes */
enum { RAINBOW_OFF = 0, RAINBOW_GRADIENT, RAINBOW_CYCLE };
void rainbow_set_mode(int mode);
int rainbow_mode(void);

/* voicebank portraits behind the notes (portraits.inl) */
void portraits_open(void);
void portraits_load(void);

/* spinning notes */
enum { SPIN_OFF = 0, SPIN_ALWAYS, SPIN_PLAYING };
#define SPIN_SPEEDS 4                         /* slow, medium, fast, silly */
void spin_set(int mode, int speed, BOOL vary);
void spin_set_playing_fn(BOOL (*fn)(void));   /* TRUE while the song plays */

/* jelly notes (modes as for spinning) */
void jelly_set(int mode);

/* gravity notes (modes as for spinning) */
#define GRAV_STRENGTHS 4                      /* moon, earth, jupiter, space (none) */
#define GRAV_BOUNCES 3                        /* none, some, super bouncy */
void grav_set(int mode, int strength, int bounce, BOOL dragTool);
void grav_drop(void);                         /* drop them all again from where they are */
void grav_explode(void);                      /* blast them all away from the middle */
void grav_quake(void);                        /* shake the floor for a moment */

#define NOTE_FILL_RGB    0xB5DC56   /* the editor's note colour (web order) */
#define NOTE_OVERLAP_RGB 0xC9F06A   /* notes that overlap another note */

/* census of colours the editor really uses, so themes can be built from it */
typedef struct { COLORREF color; int area; int kind; unsigned count; char what[12]; } CensusEntry;
int census_snapshot(CensusEntry *out, int max);
void census_reset(void);
BOOL census_export(const wchar_t *path);

int current_area(HDC dc);

/* apply after a theme change: drop cached GDI objects, re-tint bitmaps, repaint */
void engine_refresh(void);
BOOL engine_install(void);
void engine_patch_symbols(void);   /* note symbols drawn by comctl32; call once the UI is up */
void engine_set_enabled(BOOL on);
BOOL engine_enabled(void);
