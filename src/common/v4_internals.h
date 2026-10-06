/*
 * VOCALOID4 Editor internals (VOCALOID4.exe 4.x "FE", PE timestamp 0x5800A1FB).
 * Reverse-engineered addresses, offsets and Vsq4.dll interface slots.
 * Everything here is verified by signature before use - see HookCode().
 */
#pragma once
#include <windows.h>

#define THISCALL __attribute__((thiscall))

/* --------------------------------------------------------------- exe code -- */
#define V4_MusicalPartWnd_OnLButtonDown 0x0047BA30   /* thiscall (UINT flags, int x, int y) */
static const BYTE V4_SIG_OnLButtonDown[6] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 };
#define V4_MusicalPartWnd_OnMouseMove   0x0047C2E0   /* thiscall (UINT flags, int x, int y) */
static const BYTE V4_SIG_OnMouseMove[6] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 };
#define V4_CMD_PITCH_RENDER  1760   /* WM_COMMAND id of the "render pitch" button */

/* sent to the tool bar dialog when a mod tool switches on (wParam = its button id),
 * so other mod tools switch off */
#define V4_WM_MODTOOL (WM_APP + 0x51)

/* CMusicalPartWnd (the MUSICAL EDITOR view) */
#define V4_MPW_HWND          0x20    /* CWnd::m_hWnd */
#define V4_MPW_PIANOROLL     0xCA0   /* embedded CPianoRollPane */

/* CPianoRollPane */
#define V4_PRP_RULER         0x08    /* CTrackRuler* (timeline) */
#define V4_PRP_RECT          0x0C    /* RECT, client coords of the note area */
#define V4_PRP_SCROLLY       0x2C    /* vertical scroll, pixels */
#define V4_PRP_ROWH          0x58    /* pixels per semitone row */
#define V4_PRP_PARTID        0xF0    /* VSQ object id of the open musical part */

/* CTrackRuler: CGridManager sub-object + horizontal scroll state */
#define V4_RUL_GRID          0x14
#define V4_RUL_LEFT          0x48
#define V4_RUL_SCROLL_A      0xAC
#define V4_RUL_SCROLL_B      0xB0

/* RTTI type descriptors inside VOCALOID4.exe (for __RTDynamicCast) */
#define V4_TD_IVSVsqObject   ((void *)0x00591508)
#define V4_TD_IVSMusicalPart ((void *)0x005916D4)
#define V4_TD_IVSMusicalNote ((void *)0x00594E44)
#define V4_TD_IVSMusicalControl ((void *)0x005CAEC4)

/* ------------------------------------------------------ Vsq4.dll objects -- */
/* All interface methods are __thiscall; index = byte offset / 4. */
#define VT(obj)            (*(void ***)(obj))
/* call a no-argument interface method: VCALL0(int, note, VSNOTE_POS) */
#define VCALL0(T, obj, off) ((T (THISCALL *)(void *))VT(obj)[(off) / 4])(obj)
/* fetch a method pointer with an explicit signature */
#define VMETHOD(FnT, obj, off) ((FnT)VT(obj)[(off) / 4])

/* IVSVsqObject / IVSMusicalNote / IVSMusicalPart / IVSNodeList */
#define VSOBJ_ID             0x00    /* unsigned GetObjectID() */
#define VSNOTE_POS           0x14    /* int, ticks relative to the part */
#define VSNOTE_DUR           0x18
#define VSNOTE_NOTENUM       0x1C
#define VSNOTE_VELOCITY      0x20
#define VSNOTE_LYRIC         0x24    /* const wchar_t* */
#define VSNOTE_PHCOUNT       0x28
#define VSNOTE_PHONEME       0x2C    /* const wchar_t* (unsigned i) */
#define VSNOTE_PHLOCK        0x30
#define VSNOTE_STYLEATTR     0x34    /* IVSNoteStyleAttrib* */
#define VSATTR_GET           0x14    /* BOOL (const wchar_t *name, int *out) */
#define VSPART_POS           0x14    /* absolute tick */
#define VSPART_NOTES         0x38    /* IVSNodeList* of notes */
#define VSPART_CONTROLS      0x3C    /* IVSNodeList* (int type) - see V4_CTRL_* */
#define VSCTRL_POS           0x14    /* int, ticks relative to the part */
#define VSCTRL_VALUE         0x1C    /* short */

/* control types (order of the editor's own DYN..GWL table) */
enum { V4_CTRL_DYN, V4_CTRL_BRE, V4_CTRL_BRI, V4_CTRL_CLE, V4_CTRL_GEN,
       V4_CTRL_PIT, V4_CTRL_PBS, V4_CTRL_POR, V4_CTRL_XSY, V4_CTRL_GWL };
#define VSLIST_COUNT         0x14
#define VSLIST_ITEM          0x18    /* IVSVsqObject* (unsigned i) */

/* IVSVsqEditorIF (vtable of CVSVsqEditorIF in Vsq4.dll) */
#define VSED_COMMIT          0x00    /* BOOL () */
#define VSED_ROLLBACK        0x04    /* void () */
#define VSED_LASTERROR       0x08    /* unsigned () */
#define VSED_UPDATE_NOTE     0x90    /* BOOL (unsigned noteId, const VSNoteData*) */
#define VSED_INSERT_NOTE     0x9C    /* BOOL (unsigned listId, const VSNoteData*, const int expr[8], const int vib[4]) */
#define VSED_REMOVE_NOTE     0xAC    /* BOOL (unsigned noteId) */
#define VSED_UPSERT_CONTROL  0x6C    /* BOOL (unsigned controlListId, const VSControlData*) - sets value if an event exists at pos */
#define VSED_REMOVE_CONTROL  0x70    /* BOOL (unsigned controlId) */

/* MSVC 2012 std::wstring / std::vector<std::wstring> as Vsq4 expects them */
typedef struct {
    union { wchar_t buf[8]; wchar_t *ptr; } u;
    unsigned size, cap;
} MsWString;

typedef struct { MsWString *first, *last, *end; } MsWStringVec;

typedef struct {
    int pos, dur, noteNum, velocity, phLock;
    MsWString lyric;
    MsWStringVec phonemes;
} VSNoteData; /* 0x38 bytes */

typedef struct { int pos, type, value; } VSControlData;

static inline void mswstr_set(MsWString *s, const wchar_t *src)
{
    unsigned n = (unsigned)lstrlenW(src);
    if (n < 8) {
        memcpy(s->u.buf, src, (n + 1) * sizeof(wchar_t));
        s->cap = 7;
    } else {
        s->u.ptr = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(wchar_t));
        memcpy(s->u.ptr, src, (n + 1) * sizeof(wchar_t));
        s->cap = n;
    }
    s->size = n;
}

static inline void mswstr_free(MsWString *s)
{
    if (s->cap >= 8) HeapFree(GetProcessHeap(), 0, s->u.ptr);
    memset(s, 0, sizeof *s);
}

static inline const wchar_t *mswstr_c(const MsWString *s)
{
    return s->cap >= 8 ? s->u.ptr : s->u.buf;
}
