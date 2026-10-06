/*
 * Pitch lab (developer tool, enabled with [PitchTools] DevTools=1).
 * Generates note transitions with chosen expression parameters, renders each
 * with VOCALOID, and writes the rendered pitch layers to a CSV so the
 * real-time pitch model can be fitted and checked against the real thing.
 * Every case is undone afterwards; the open part is left as it was.
 */
#include <stdio.h>

typedef struct {
    int n1, n2;            /* note numbers; n1 < 0 = single note */
    int pos1, dur1, pos2, dur2;
    int e1[8], e2[8];      /* bendDep, bendLen, risePort, fallPort, decay, accent, opening */
    int vib[4];            /* {on, len%, type, 0} for both notes */
    int tempo;             /* bpm*100, 0 = leave */
    int npit;              /* PIT events (relative ticks, value) with PBS pbs */
    int pitPos[8], pitVal[8], pbs;
} LabCase;

#define V4_TD_IVSTempo ((void *)0x00598328)
typedef void *(THISCALL *GetSeq_t)(void *mgr);

/* id of the first tempo event of the master track */
static unsigned lab_tempo_id(void *mgr)
{
    GetSeq_t getSeq = (GetSeq_t)GetProcAddress(GetModuleHandleA("vsq4.dll"), "?vsqGetCurrentSequence@CVSVsqManagerIF@@QAEPAVIVSVsqRoot@@XZ");
    void *root = getSeq ? getSeq(mgr) : NULL;
    void *seq = root ? VCALL0(void *, root, 0x14) : NULL;
    void *master = seq ? VCALL0(void *, seq, 0x1C) : NULL;
    void *list = master ? VCALL0(void *, master, 0x30) : NULL;
    if (!list || VCALL0(int, list, VSLIST_COUNT) < 1) return 0;
    void *obj = VMETHOD(ListItem_t, list, VSLIST_ITEM)(list, 0);
    void *tempo = obj ? pDynCast(obj, 0, V4_TD_IVSVsqObject, V4_TD_IVSTempo, 0) : NULL;
    return tempo ? VCALL0(unsigned, tempo, VSOBJ_ID) : 0;
}

static const int kDefExpr[8] = { 8, 0, 50, 50, 50, 50, 127, 0 };

static void lab_note(VSNoteData *d, int pos, int dur, int num)
{
    memset(d, 0, sizeof *d);
    d->pos = pos; d->dur = dur; d->noteNum = num; d->velocity = 64;
    mswstr_set(&d->lyric, L"a");
    d->phonemes.first = (MsWString *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MsWString));
    mswstr_set(&d->phonemes.first[0], L"a");
    d->phonemes.last = d->phonemes.end = d->phonemes.first + 1;
}

static void lab_free(VSNoteData *d)
{
    mswstr_free(&d->lyric);
    mswstr_free(&d->phonemes.first[0]);
    HeapFree(GetProcessHeap(), 0, d->phonemes.first);
}

static BOOL lab_run(FILE *f, int id, const LabCase *c)
{
    PartRef pr;
    if (!open_part(&pr)) return FALSE;
    static NoteRef notes[4096];
    int nn = part_notes(&pr, notes, 4096);
    ControlList pit, pbs;
    static ControlRef ev[65536];
    void *ed = pGetVsqEditor(pr.mgr, 2);
    if (!ed) return FALSE;
    BOOL ok = TRUE;
    typedef BOOL (THISCALL *RemoveNote_t)(void *ed, unsigned id);
    RemoveNote_t removeNote = VMETHOD(RemoveNote_t, ed, VSED_REMOVE_NOTE);
    RemoveControl_t removeCtl = VMETHOD(RemoveControl_t, ed, VSED_REMOVE_CONTROL);
    InsertNote_t insert = VMETHOD(InsertNote_t, ed, VSED_INSERT_NOTE);
    for (int i = 0; i < nn && ok; i++) ok = removeNote(ed, notes[i].id);
    memset(&pit, 0, sizeof pit); memset(&pbs, 0, sizeof pbs);
    for (int t = 0; t < 2 && ok; t++) {
        if (!open_controls(&pr, t ? V4_CTRL_PBS : V4_CTRL_PIT, t ? &pbs : &pit)) continue;
        int n = control_events(t ? &pbs : &pit, ev, 65536);
        for (int i = 0; i < n && ok; i++) ok = removeCtl(ed, ev[i].id);
    }
    VSNoteData a, b;
    if (c->n1 >= 0) { lab_note(&a, c->pos1, c->dur1, c->n1); if (ok) ok = insert(ed, pr.listId, &a, c->e1, c->vib); lab_free(&a); }
    lab_note(&b, c->pos2, c->dur2, c->n2);
    if (ok) ok = insert(ed, pr.listId, &b, c->e2, c->vib);
    lab_free(&b);
    if (ok && c->tempo) {
        typedef BOOL (THISCALL *UpdTempo_t)(void *ed, unsigned id, const int *d);
        unsigned tid = lab_tempo_id(pr.mgr);
        int td[2] = { 0, c->tempo };
        ok = tid && VMETHOD(UpdTempo_t, ed, 0x24)(ed, tid, td);
    }
    if (ok && c->npit) {
        UpsertControl_t upsert = VMETHOD(UpsertControl_t, ed, VSED_UPSERT_CONTROL);
        VSControlData d = { 0, V4_CTRL_PBS, c->pbs };
        ok = upsert(ed, pbs.listId, &d);
        for (int i = 0; i < c->npit && ok; i++) {
            VSControlData e = { c->pitPos[i], V4_CTRL_PIT, c->pitVal[i] };
            ok = upsert(ed, pit.listId, &e);
        }
    }
    if (ok) ok = VCALL0(BOOL, ed, VSED_COMMIT);
    if (!ok) { A->Log("lab: case %d failed %08x", id, VCALL0(unsigned, ed, VSED_LASTERROR)); VCALL0(void, ed, VSED_ROLLBACK); }
    pReleaseVsqEditor(pr.mgr, &ed);
    if (!ok) return FALSE;

    render_now();
    fprintf(f, "#case,%d,n1,%d,n2,%d,pos1,%d,dur1,%d,pos2,%d,dur2,%d,partPos,%d", id, c->n1, c->n2, c->pos1, c->dur1, c->pos2, c->dur2, pr.partPos);
    for (int i = 0; i < 7; i++) fprintf(f, ",e1_%d,%d", i, c->e1[i]);
    for (int i = 0; i < 7; i++) fprintf(f, ",e2_%d,%d", i, c->e2[i]);
    fprintf(f, ",vib,%d,%d,%d,renderMs,%lu\n", c->vib[0], c->vib[1], c->vib[2], g_lastRenderMs);
    RenderFrame *fb = *(RenderFrame **)(PANE() + PRP_RENDER_BEGIN), *fe = *(RenderFrame **)(PANE() + PRP_RENDER_END);
    long from = pr.partPos + (c->n1 >= 0 ? c->pos1 : c->pos2) - 960, to = pr.partPos + c->pos2 + c->dur2 + 480;
    for (RenderFrame *r = fb; r && r < fe; r++) {
        if ((long)r->tick < from || (long)r->tick > to || r->note < -1e30f) continue;
        fprintf(f, "%d,%u,%.3f,%.3f,%.3f,%.3f,%.4f\n", id, r->tick, r->note, r->porta, r->pit, r->final, r->dyn);
    }
    typedef int (THISCALL *MgrUndo_t2)(void *mgr);
    MgrUndo_t2 undo = (MgrUndo_t2)GetProcAddress(GetModuleHandleA("vsq4.dll"), "?editUndo@CVSVsqManagerIF@@QAEHXZ");
    undo(pr.mgr);
    return TRUE;
}

static void lab_base(LabCase *c, int n1, int n2, int pos1, int dur1, int pos2, int dur2)
{
    memset(c, 0, sizeof *c);
    c->n1 = n1; c->n2 = n2; c->pos1 = pos1; c->dur1 = dur1; c->pos2 = pos2; c->dur2 = dur2;
    memcpy(c->e1, kDefExpr, sizeof c->e1); memcpy(c->e2, kDefExpr, sizeof c->e2);
}

static void lab_set2(FILE *f, int *id)
{
    LabCase c;
    static const int durs[] = { 240, 480, 960, 1920 };
    /* F: bendLen fine sweep, depth 50, four note lengths (single notes) */
    for (int d = 0; d < 4; d++) for (int l = 0; l <= 100; l += 10) {
        lab_base(&c, -1, 62, 0, 0, 1920, durs[d]); c.e2[0] = 50; c.e2[1] = l; lab_run(f, (*id)++, &c);
    }
    /* G: tempo dependence */
    static const int tempos[] = { 6000, 12000, 24000 };
    for (int t = 0; t < 3; t++) {
        lab_base(&c, -1, 62, 0, 0, 1920, 960); c.e2[0] = 50; c.e2[1] = 50; c.tempo = tempos[t]; lab_run(f, (*id)++, &c);
        lab_base(&c, 60, 67, 1920, 960, 2880, 960); c.tempo = tempos[t]; lab_run(f, (*id)++, &c);
        lab_base(&c, -1, 62, 0, 0, 1920, 1920); c.vib[0] = 1; c.vib[1] = 100; c.vib[2] = 1; c.tempo = tempos[t]; lab_run(f, (*id)++, &c);
    }
    /* H: transitions - risePort fine sweep x interval, no bend */
    static const int iv[] = { 1, 2, 5, 12, -1, -2, -5, -12 };
    for (unsigned i = 0; i < 8; i++) for (int p = 0; p <= 100; p += 10) {
        lab_base(&c, 60, 60 + iv[i], 1920, 960, 2880, 960);
        c.e1[0] = c.e2[0] = 0; c.e2[2] = p; c.e2[3] = p; lab_run(f, (*id)++, &c);
    }
    /* H2: transitions - bendLen and bendDep of the arriving note, +5 and -5 */
    for (int s = 0; s < 2; s++) for (int dd = 0; dd <= 100; dd += 25) for (int l = 0; l <= 100; l += 25) {
        lab_base(&c, 60, s ? 55 : 65, 1920, 960, 2880, 960); c.e2[0] = dd; c.e2[1] = l; lab_run(f, (*id)++, &c);
    }
    /* I: vibrato types at three note lengths, full length */
    static const int vd[] = { 480, 960, 3840 };
    for (int t = 1; t <= 16; t++) for (int d = 0; d < 3; d++) {
        lab_base(&c, -1, 62, 0, 0, 1920, vd[d]); c.vib[0] = 1; c.vib[1] = 100; c.vib[2] = t; lab_run(f, (*id)++, &c);
    }
    /* J: PIT semantics - constant, step and ramp */
    lab_base(&c, -1, 62, 0, 0, 1920, 1920); c.e2[0] = 0; c.pbs = 2; c.npit = 1; c.pitPos[0] = 0; c.pitVal[0] = 4096; lab_run(f, (*id)++, &c);
    lab_base(&c, -1, 62, 0, 0, 1920, 1920); c.e2[0] = 0; c.pbs = 4; c.npit = 3; c.pitPos[0] = 0; c.pitVal[0] = 0;
    c.pitPos[1] = 2400; c.pitVal[1] = 8191; c.pitPos[2] = 2880; c.pitVal[2] = -8192; lab_run(f, (*id)++, &c);
    lab_base(&c, -1, 62, 0, 0, 1920, 1920); c.e2[0] = 0; c.pbs = 2; c.npit = 5;
    for (int i = 0; i < 5; i++) { c.pitPos[i] = 2400 + i * 5; c.pitVal[i] = i * 2000; }
    lab_run(f, (*id)++, &c);
}

static void __cdecl menu_lab(void *ctx)
{
    (void)ctx;
    wchar_t path[MAX_PATH];
    GetEnvironmentVariableW(L"APPDATA", path, MAX_PATH);
    lstrcatW(path, L"\\Vocahansments\\pitchlab.csv");
    FILE *f = _wfopen(path, L"w");
    if (!f) return;
    g_suppress = TRUE;
    int id = 0;
    LabCase c;
    static const int ints[] = { -12, -7, -4, -2, -1, 1, 2, 4, 7, 12 };
    static const int lv[] = { 0, 50, 100 };
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));

    if (vh_ini_int(g_ini, L"PitchTools", L"DevSet", 1) == 2) { lab_set2(f, &id); goto finish; }
    /* A: same expression on both notes, adjacent notes */
    for (unsigned i = 0; i < sizeof ints / sizeof ints[0]; i++)
        for (int d = 0; d < 3; d++) for (int l = 0; l < 3; l++) for (int p = 0; p < 3; p++) {
            memset(&c, 0, sizeof c);
            c.n1 = 60; c.n2 = 60 + ints[i]; c.pos1 = 1920; c.dur1 = 960; c.pos2 = 2880; c.dur2 = 960;
            memcpy(c.e1, kDefExpr, sizeof c.e1);
            c.e1[0] = lv[d]; c.e1[1] = lv[l]; c.e1[2] = lv[p]; c.e1[3] = lv[p];
            memcpy(c.e2, c.e1, sizeof c.e2);
            lab_run(f, id++, &c);
        }
    /* B: one parameter on one note only (who owns the transition?) */
    for (int iv = 0; iv < 2; iv++) for (int note = 0; note < 2; note++) for (int prm = 0; prm < 4; prm++) for (int v = 0; v < 3; v++) {
        memset(&c, 0, sizeof c);
        c.n1 = 60; c.n2 = iv ? 53 : 67; c.pos1 = 1920; c.dur1 = 960; c.pos2 = 2880; c.dur2 = 960;
        memcpy(c.e1, kDefExpr, sizeof c.e1); memcpy(c.e2, kDefExpr, sizeof c.e2);
        (note ? c.e2 : c.e1)[prm] = lv[v];
        lab_run(f, id++, &c);
    }
    /* C: gaps and note lengths */
    static const int gaps[] = { 0, 60, 240, 960 }, lens[] = { 120, 240, 480, 1920 };
    for (int g = 0; g < 4; g++) for (int ln = 0; ln < 4; ln++) {
        memset(&c, 0, sizeof c);
        c.n1 = 60; c.n2 = 65; c.pos1 = 1920; c.dur1 = lens[ln]; c.pos2 = 1920 + lens[ln] + gaps[g]; c.dur2 = lens[ln];
        memcpy(c.e1, kDefExpr, sizeof c.e1); memcpy(c.e2, kDefExpr, sizeof c.e2);
        lab_run(f, id++, &c);
    }
    /* D: single notes (attack / release only) */
    for (int d = 0; d < 3; d++) for (int l = 0; l < 3; l++) {
        memset(&c, 0, sizeof c);
        c.n1 = -1; c.n2 = 62; c.pos2 = 1920; c.dur2 = 960;
        memcpy(c.e2, kDefExpr, sizeof c.e2); c.e2[0] = lv[d]; c.e2[1] = lv[l];
        lab_run(f, id++, &c);
    }
    /* E: vibrato types and lengths on a single long note */
    for (int type = 1; type <= 16; type++) for (int len = 0; len < 3; len++) {
        memset(&c, 0, sizeof c);
        c.n1 = -1; c.n2 = 62; c.pos2 = 1920; c.dur2 = 1920;
        memcpy(c.e2, kDefExpr, sizeof c.e2);
        c.vib[0] = 1; c.vib[1] = (len + 1) * 33; c.vib[2] = type;
        lab_run(f, id++, &c);
    }
finish:
    fclose(f);
    g_suppress = FALSE;
    SetCursor(old);
    A->Log("lab: %d cases written to %ls", id, path);
}
