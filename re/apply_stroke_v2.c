/* The rendered pitch the editor draws lives on the piano-roll pane as
 * {abs tick, 5 floats}; f1 note, f2 +portamento, f3 +PIT, f4 final sung pitch
 * (with vibrato), in cents relative to A3 (MIDI 57). -FLT_MAX = no voice. */
typedef struct { unsigned tick; float note, porta, pit, final, dyn; } RenderFrame;
#define PRP_RENDER_BEGIN 0x224
#define PRP_RENDER_END   0x228

static BOOL rendered_pitch_at(long t, BOOL withVibrato, double *semis)
{
    RenderFrame *b = *(RenderFrame **)(PANE() + PRP_RENDER_BEGIN), *e = *(RenderFrame **)(PANE() + PRP_RENDER_END);
    if (!b || e <= b) return FALSE;
    int lo = 0, hi = (int)(e - b) - 1;
    if ((long)b[0].tick > t || (long)b[hi].tick < t) return FALSE;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if ((long)b[mid].tick <= t) lo = mid; else hi = mid - 1; }
    float v = withVibrato ? b[lo].final : b[lo].pit;
    if (v < -1e30f) return FALSE;
    *semis = 57.0 + v / 100.0;
    return TRUE;
}

static void render_now(void)
{
    HWND main = A->GetMainWindow();
    if (!main) return;
    DWORD t = GetTickCount();
    g_rendering = TRUE;
    SendMessageW(main, WM_COMMAND, V4_CMD_PITCH_RENDER, 0);
    g_rendering = FALSE;
    g_lastRenderMs = GetTickCount() - t;
}

typedef int (THISCALL *MgrUndo_t)(void *mgr);

typedef struct {
    PartRef pr;
    ControlList pit, pbs;
    ControlRef *pitEv, *pbsEv;   /* original events, sorted */
    int npit, npbs;
    long t0;
    int nslots;
    const BYTE *set;             /* sample has a note under it */
} StrokeCtx;

/* Write dev[] (semitones from the note) for every run of covered samples, as
 * one commit. PIT/PBS are restored right after each run. */
static BOOL write_pitch(StrokeCtx *c, const double *dev, int *eventsOut)
{
    void *ed = pGetVsqEditor(c->pr.mgr, 2);
    if (!ed) return FALSE;
    UpsertControl_t upsert = VMETHOD(UpsertControl_t, ed, VSED_UPSERT_CONTROL);
    RemoveControl_t removeCtl = VMETHOD(RemoveControl_t, ed, VSED_REMOVE_CONTROL);
    BOOL ok = TRUE;
    int written = 0;
    for (int s = 0; s < c->nslots && ok;) {
        if (!c->set[s]) { s++; continue; }
        int e = s;
        while (e + 1 < c->nslots && c->set[e + 1]) e++;
        int rA = (int)(c->t0 + (long)s * SAMPLE_STEP - c->pr.partPos);
        int rB = (int)(c->t0 + (long)e * SAMPLE_STEP - c->pr.partPos);
        double maxDev = 0;
        for (int i = s; i <= e; i++) if (fabs(dev[i]) > maxDev) maxDev = fabs(dev[i]);
        int pbsBefore = control_value_at(c->pbsEv, c->npbs, rA - 1, 2);
        int pbsAfter = control_value_at(c->pbsEv, c->npbs, rB, 2);
        int pitAfter = control_value_at(c->pitEv, c->npit, rB, 0);
        int need = (int)ceil(maxDev - 1e-6);
        int range = pbsBefore > need ? pbsBefore : need;
        if (range < 1) range = 1;
        if (range > 24) range = 24;

        for (int i = 0; i < c->npit && ok; i++) if (c->pitEv[i].pos >= rA && c->pitEv[i].pos <= rB) ok = removeCtl(ed, c->pitEv[i].id);
        for (int i = 0; i < c->npbs && ok; i++) if (c->pbsEv[i].pos > rA && c->pbsEv[i].pos <= rB) ok = removeCtl(ed, c->pbsEv[i].id);
        if (ok && range != pbsBefore) { VSControlData d = { rA, V4_CTRL_PBS, range }; ok = upsert(ed, c->pbs.listId, &d); }
        int last = 0x7FFFFFFF;
        for (int i = s; i <= e && ok; i++) {
            int v = (int)lround(dev[i] / range * 8192.0);
            if (v > 8191) v = 8191;
            if (v < -8192) v = -8192;
            if (v == last) continue;
            VSControlData d = { (int)(c->t0 + (long)i * SAMPLE_STEP - c->pr.partPos), V4_CTRL_PIT, v };
            ok = upsert(ed, c->pit.listId, &d);
            last = v;
            written++;
        }
        int after = rB + SAMPLE_STEP;
        if (ok && range != pbsAfter) { VSControlData d = { after, V4_CTRL_PBS, pbsAfter }; ok = upsert(ed, c->pbs.listId, &d); }
        if (ok && last != pitAfter) { VSControlData d = { after, V4_CTRL_PIT, pitAfter }; ok = upsert(ed, c->pit.listId, &d); }
        s = e + 1;
    }
    if (ok) ok = VCALL0(BOOL, ed, VSED_COMMIT);
    if (!ok) {
        A->Log("pitch: write failed, error %08x", VCALL0(unsigned, ed, VSED_LASTERROR));
        VCALL0(void, ed, VSED_ROLLBACK);
    }
    pReleaseVsqEditor(c->pr.mgr, &ed);
    if (eventsOut) *eventsOut = written;
    return ok;
}

static BOOL apply_stroke(void)
{
    if (g_npts < 1) return FALSE;
    StrokeCtx c;
    memset(&c, 0, sizeof c);
    if (!open_part(&c.pr)) return FALSE;

    /* rasterise the stroke onto a tick grid; later strokes over the same tick win */
    long t0 = g_pts[0].tick, t1 = g_pts[0].tick;
    for (int i = 1; i < g_npts; i++) { if (g_pts[i].tick < t0) t0 = g_pts[i].tick; if (g_pts[i].tick > t1) t1 = g_pts[i].tick; }
    t0 = (t0 / SAMPLE_STEP) * SAMPLE_STEP;
    if (t0 < c.pr.partPos) t0 = c.pr.partPos;
    if (t1 < t0) return FALSE;
    int nslots = (int)((t1 - t0) / SAMPLE_STEP) + 1;
    double *target = (double *)HeapAlloc(GetProcessHeap(), 0, nslots * sizeof(double));
    double *dev = (double *)HeapAlloc(GetProcessHeap(), 0, nslots * sizeof(double));
    int *base = (int *)HeapAlloc(GetProcessHeap(), 0, nslots * sizeof(int));
    BYTE *set = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, nslots);
    if (!target || !dev || !base || !set) return FALSE;
    for (int i = 0; i < g_npts; i++) {
        PenPoint a = g_pts[i], b = g_pts[i + 1 < g_npts ? i + 1 : i];
        long lo = a.tick < b.tick ? a.tick : b.tick, hi = a.tick < b.tick ? b.tick : a.tick;
        for (int s = (int)((lo - t0 + SAMPLE_STEP - 1) / SAMPLE_STEP); s < nslots; s++) {
            long t = t0 + (long)s * SAMPLE_STEP;
            if (t > hi) break;
            if (t < lo) continue;
            double f = hi == lo ? 0 : (double)(t - a.tick) / (double)(b.tick - a.tick);
            target[s] = a.pitch + (b.pitch - a.pitch) * f;
            set[s] = 1;
        }
    }
    if (g_npts == 1) { target[0] = g_pts[0].pitch; set[0] = 1; }

    /* only where a note is sounding: gaps between notes are left alone */
    static NoteRef notes[8192];
    int nn = part_notes(&c.pr, notes, 8192), covered = 0;
    for (int s = 0; s < nslots; s++) {
        if (!set[s]) continue;
        int rel = (int)(t0 + (long)s * SAMPLE_STEP - c.pr.partPos);
        base[s] = -1;
        for (int i = 0; i < nn; i++) if (rel >= notes[i].pos && rel < notes[i].pos + notes[i].dur) { base[s] = notes[i].num; break; }
        if (base[s] < 0) { set[s] = 0; continue; }
        dev[s] = g_erasing ? 0.0 : target[s] - base[s];
        covered++;
    }
    BOOL ok = FALSE;
    if (!covered) { A->Log("pitch: stroke is not over any note"); goto done; }

    if (!open_controls(&c.pr, V4_CTRL_PIT, &c.pit) || !open_controls(&c.pr, V4_CTRL_PBS, &c.pbs)) { A->Log("pitch: PIT/PBS lists not found"); goto done; }
    static ControlRef pitEv[65536], pbsEv[4096];
    c.pitEv = pitEv; c.pbsEv = pbsEv;
    c.npit = control_events(&c.pit, pitEv, 65536);
    c.npbs = control_events(&c.pbs, pbsEv, 4096);
    qsort(pitEv, c.npit, sizeof pitEv[0], cmp_ctrl);
    qsort(pbsEv, c.npbs, sizeof pbsEv[0], cmp_ctrl);
    c.t0 = t0; c.nslots = nslots; c.set = set;

    /* Correction passes: write, render, measure what VOCALOID really sings
     * (portamento, and vibrato unless the user keeps it), fold the error back
     * into PIT, undo the trial. The final write is the only commit left. */
    int passes = g_erasing ? 0 : g_exactPasses;
    void *mgr = c.pr.mgr;
    MgrUndo_t undo = (MgrUndo_t)GetProcAddress(GetModuleHandleA("vsq4.dll"), "?editUndo@CVSVsqManagerIF@@QAEHXZ");
    double worst = 0;
    g_suppress = TRUE;
    for (int p = 0; p < passes && undo; p++) {
        if (!write_pitch(&c, dev, NULL)) break;
        render_now();
        worst = 0;
        int measured = 0;
        for (int s = 0; s < nslots; s++) {
            double sung;
            if (!set[s] || !rendered_pitch_at(t0 + (long)s * SAMPLE_STEP, !g_keepVibrato, &sung)) continue;
            double err = target[s] - sung;
            dev[s] += err;
            if (dev[s] > 24) dev[s] = 24;
            if (dev[s] < -24) dev[s] = -24;
            if (fabs(err) > worst) worst = fabs(err);
            measured++;
        }
        undo(mgr);
        if (!measured) break;
        if (worst < 0.02) break;   /* within 2 cents */
    }
    g_suppress = FALSE;

    int written = 0;
    ok = write_pitch(&c, dev, &written);
    A->Log("pitch: %s %ld..%ld, %d PIT events, last error %.1f cents", g_erasing ? "erased" : "drew", t0, t1, written, worst * 100);
done:
    HeapFree(GetProcessHeap(), 0, target);
    HeapFree(GetProcessHeap(), 0, dev);
    HeapFree(GetProcessHeap(), 0, base);
    HeapFree(GetProcessHeap(), 0, set);
    return ok;
}

