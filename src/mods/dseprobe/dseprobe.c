/*
 * DSE probe (developer tool, lab only - not shipped).
 * Hooks DSE4's per-frame score export and logs the segment records the
 * engine keeps for every frame, while rendering a small known test phrase.
 * Used to reverse-engineer phoneme timing.
 */
#include <windows.h>
#include <stdio.h>
#include "../../common/vh_api.h"
static const VHApi *A;
#include "../../common/v4_editor.h"

#define DSE_EXPORT_FRAME_RVA 0x17280   /* FUN_10017280: thiscall(engine, events*, score*) */

typedef int (THISCALL *ExportFrame_t)(BYTE *eng, void *ev, float *score);
static ExportFrame_t o_ExportFrame;
static BOOL g_probe;
static FILE *g_f;
static DWORD g_last[3];

static BOOL readable(DWORD p, int n)
{
    MEMORY_BASIC_INFORMATION m;
    if (p < 0x10000 || !VirtualQuery((void *)p, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return FALSE;
    return p + n <= (DWORD)m.BaseAddress + m.RegionSize;
}

static void dump_ptr_string(DWORD v)
{
    if (!readable(v, 16)) return;
    const char *a = (const char *)v;
    int ok = 1;
    for (int i = 0; i < 2; i++) if (a[i] < 0x20 || a[i] > 0x7e) ok = 0;
    if (ok) { fprintf(g_f, " \"%.12s\"", a); return; }
    const wchar_t *w = (const wchar_t *)v;
    if (w[0] >= 0x20 && w[0] < 0x7f && w[1] >= 0x20 && w[1] < 0x7f) fprintf(g_f, " L\"%.12ls\"", w);
}

static void dump_block(const char *tag, DWORD p, int bytes)
{
    if (!readable(p, bytes)) { fprintf(g_f, "  %s %08lx (unreadable)\n", tag, p); return; }
    fprintf(g_f, "  %s %08lx:", tag, p);
    for (int i = 0; i < bytes / 4; i++) {
        DWORD v = ((DWORD *)p)[i];
        if (i % 8 == 0) fprintf(g_f, "\n    +%03x", i * 4);
        float fv = *(float *)&v;
        if (v && (v & 0x7f800000) && fv > -1e7f && fv < 1e7f && (fv > 1e-4f || fv < -1e-4f) && (v >> 24) != 0x10 && (v >> 16) > 0x3000)
            fprintf(g_f, " %08lx(%.3g)", v, fv);
        else
            fprintf(g_f, " %08lx", v);
    }
    fprintf(g_f, "\n   strings:");
    for (int i = 0; i < bytes / 4; i++) dump_ptr_string(((DWORD *)p)[i]);
    fprintf(g_f, "\n");
}

static int THISCALL h_ExportFrame(BYTE *eng, void *ev, float *score)
{
    int r = o_ExportFrame(eng, ev, score);
    if (!g_probe || !g_f) return r;
    int f = *(int *)(eng + 0x1d9c0) - 1;
    DWORD *R = *(DWORD **)(eng + 0x19e98);
    if (!readable((DWORD)R, 16) || !R[1]) return r;
    DWORD *e = (DWORD *)(R[0] + (DWORD)(f % (int)R[1]) * 12);
    if (!readable((DWORD)e, 12)) return r;
    if (score) fprintf(g_f, "F %d ms %.1f e %08lx %08lx %08lx sc %.1f %.1f %.1f %.1f %.3f\n", f, *(double *)(eng + 0x1d9b8) * 1000.0,
                       e[0], e[1], e[2], score[0], score[1], score[2], score[3], score[4]);
    int *seg = (int *)e[0];
    if ((seg[4] != -1 || seg[5] != -1) && seg[0] == f) {
        dump_block("seg", e[0], 0xf0);
        BYTE *dse = (BYTE *)GetModuleHandleA("DSE4.dll");
        DWORD vtART = (DWORD)dse + 0x347b44, vtU = (DWORD)dse + 0x347b8c, vtP = (DWORD)dse + 0x347bdc;
        DWORD ap = seg[0x18 / 4];
        dump_block("ARTp", ap, 0x208);
        { static const int offs[] = { 0x120, 0x13c, 0x168, 0x184, 0x1ac };
          for (int k = 0; k < 5; k++) { char t[16]; wsprintfA(t, "ARTp+%x", offs[k]); if (readable(ap, 0x208)) dump_block(t, *(DWORD *)(ap + offs[k]), 0x40); } }
        (void)vtP;
    }
    return r;
}

static BOOL install(void)
{
    if (o_ExportFrame) return TRUE;
    BYTE *dse = (BYTE *)GetModuleHandleA("DSE4.dll");
    if (!dse) { A->Log("probe: DSE4 not loaded"); return FALSE; }
    BYTE *at = dse + DSE_EXPORT_FRAME_RVA;
    if (at[0] != 0x6A || at[1] != 0xFF || at[2] != 0x68) { A->Log("probe: unexpected bytes"); return FALSE; }
    BYTE expect[7];
    memcpy(expect, at, 7);
    return A->HookCode(at, expect, 7, h_ExportFrame, (void **)&o_ExportFrame);
}

/* test phrase: (lyric, phonemes separated by spaces, pos, dur, note) */
typedef struct { const wchar_t *lyric, *ph; int pos, dur, num; } TestNote;

static void make_note(VSNoteData *d, const TestNote *t)
{
    memset(d, 0, sizeof *d);
    d->pos = t->pos; d->dur = t->dur; d->noteNum = t->num; d->velocity = 64; d->phLock = 1;
    mswstr_set(&d->lyric, t->lyric);
    wchar_t buf[64]; lstrcpynW(buf, t->ph, 64);
    int n = 0; wchar_t *parts[8]; wchar_t *ctx = NULL;
    for (wchar_t *tok = wcstok(buf, L" ", &ctx); tok && n < 8; tok = wcstok(NULL, L" ", &ctx)) parts[n++] = tok;
    d->phonemes.first = (MsWString *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n * sizeof(MsWString));
    for (int i = 0; i < n; i++) mswstr_set(&d->phonemes.first[i], parts[i]);
    d->phonemes.last = d->phonemes.end = d->phonemes.first + n;
}

static void free_note(VSNoteData *d)
{
    mswstr_free(&d->lyric);
    for (MsWString *s = d->phonemes.first; s && s < d->phonemes.last; s++) mswstr_free(s);
    if (d->phonemes.first) HeapFree(GetProcessHeap(), 0, d->phonemes.first);
}

static const TestNote kPhrase[] = {
    { L"sa", L"s a", 1920, 480, 60 },
    { L"ka", L"k a", 2400, 480, 62 },
    { L"ta", L"t a", 2880, 960, 64 },
    { L"a",  L"a",   4800, 480, 60 },
    { L"na", L"n a", 5280, 240, 60 },
    { L"shi", L"S i", 5520, 1440, 60 },
};

static BOOL g_insertOnly;
static void __cdecl menu_probe(void *c)
{
    (void)c;
    if (!g_insertOnly && !install()) return;
    PartRef pr;
    if (!open_part(&pr)) { A->Log("probe: no part open (move the mouse over the musical editor first)"); return; }
    void *ed = pGetVsqEditor(pr.mgr, 2);
    if (!ed) return;
    typedef BOOL (THISCALL *RemoveNote_t)(void *ed, unsigned id);
    static NoteRef notes[4096];
    int nn = part_notes(&pr, notes, 4096);
    BOOL ok = TRUE;
    for (int i = 0; i < nn && ok; i++) ok = VMETHOD(RemoveNote_t, ed, VSED_REMOVE_NOTE)(ed, notes[i].id);
    static const int expr[8] = { 8, 0, 50, 50, 50, 50, 127, 0 }, vib[4] = { 0, 0, 0, 0 };
    for (unsigned i = 0; i < sizeof kPhrase / sizeof kPhrase[0] && ok; i++) {
        VSNoteData d; make_note(&d, &kPhrase[i]);
        ok = VMETHOD(InsertNote_t, ed, VSED_INSERT_NOTE)(ed, pr.listId, &d, expr, vib);
        free_note(&d);
    }
    if (ok) ok = VCALL0(BOOL, ed, VSED_COMMIT);
    else VCALL0(void, ed, VSED_ROLLBACK);
    pReleaseVsqEditor(pr.mgr, &ed);
    if (!ok) { A->Log("probe: insert failed"); return; }
    if (g_insertOnly) return;

    wchar_t path[MAX_PATH];
    wsprintfW(path, L"%sdseprobe.txt", A->modDir);
    g_f = _wfopen(path, L"w");
    fprintf(g_f, "partPos %d\n", pr.partPos);
    for (unsigned i = 0; i < sizeof kPhrase / sizeof kPhrase[0]; i++)
        fprintf(g_f, "note %ls [%ls] pos %d dur %d num %d\n", kPhrase[i].lyric, kPhrase[i].ph, pr.partPos + kPhrase[i].pos, kPhrase[i].dur, kPhrase[i].num);
    memset(g_last, 0, sizeof g_last);
    g_probe = TRUE;
    SendMessageW(A->GetMainWindow(), WM_COMMAND, V4_CMD_PITCH_RENDER, 0);
    g_probe = FALSE;
    {
        typedef struct { unsigned tick; float v[5]; } RF;
        RF *b = *(RF **)(PANE() + 0x224), *e = *(RF **)(PANE() + 0x228);
        fprintf(g_f, "PANE %d frames\n", b ? (int)(e - b) : -1);
        for (RF *r = b; r && r < e; r++) if ((r - b) % 50 == 0 || (r > b && r[-1].v[0] < -1e30f && r->v[0] > -1e30f))
            fprintf(g_f, "P %d tick %u note %.1f final %.1f\n", (int)(r - b), r->tick, r->v[0], r->v[3]);
    }
    fclose(g_f); g_f = NULL;
    A->Log("probe: written %ls", path);
}

typedef void (THISCALL *MM_t)(void *, UINT, int, int);
static MM_t o_MouseMove;
static LRESULT CALLBACK view_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR r)
{
    (void)id; (void)r;
    if (m == WM_NCDESTROY) { g_view = NULL; g_viewHwnd = NULL; }
    return DefSubclassProc(h, m, w, l);
}
static void THISCALL h_MouseMove(void *self, UINT f, int x, int y)
{
    v4_attach_view((BYTE *)self, view_proc, 9);
    o_MouseMove(self, f, x, y);
}

static void __cdecl menu_insert(void *c) { g_insertOnly = TRUE; menu_probe(c); g_insertOnly = FALSE; }

static void __cdecl on_ui(HWND h, void *c)
{
    (void)h; (void)c;
    vh_menu_add(A, L"Dev", L"DSE phoneme probe", menu_probe, NULL);
    vh_menu_add(A, L"Dev", L"Insert test phrase", menu_insert, NULL);
}

__declspec(dllexport) int __cdecl VH_ModInit(const VHApi *api)
{
    A = api;
    if (!v4_resolve_imports()) return 0;
    api->HookCode((void *)V4_MusicalPartWnd_OnMouseMove, V4_SIG_OnMouseMove, 6, h_MouseMove, (void **)&o_MouseMove);
    api->OnUiReady(on_ui, NULL);
    return 1;
}
