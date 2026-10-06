/*
 * Note Tools (Mods > Note Tools): lyric, phoneme and timing edits on the
 * selected notes. Each one is a single Undo step, made through the editor's
 * own note interface like the Slice Tool's cuts.
 *
 *   Quantize to grid         starts and ends onto the QUANTIZE grid
 *   Rolled R                 a run of short flap notes ([4]) leads into each note
 *   Whisper (_0) / Unwhisper devoiced vowels: "_0" on every vowel phoneme
 *   Breaths before phrases   a breath note ([br1]) in the gap before each phrase
 *   Backing vocals           ooh / aah / hum: the notes sing one plain vowel
 *   Lock / Unlock phonemes   keep the phonemes when the lyric changes, or not
 *
 * Included by pitchtools.c (after guides.c: it uses the toast).
 */

enum { LANG_JP, LANG_EN };
static int g_ntLang = LANG_JP;
static int g_rollCount = 6, g_rollTicks = 20;
static UINT g_cmdLang[2], g_cmdRollN[4], g_cmdRollT[4];
static const int kRollCounts[4] = { 4, 6, 8, 12 }, kRollTicks[4] = { 15, 20, 30, 40 };

/* ------------------------------------------------------- note data -- */

static void nt_read(void *note, VSNoteData *d)
{
    memset(d, 0, sizeof *d);
    d->pos = VCALL0(int, note, VSNOTE_POS);
    d->dur = VCALL0(int, note, VSNOTE_DUR);
    d->noteNum = VCALL0(int, note, VSNOTE_NOTENUM);
    d->velocity = VCALL0(int, note, VSNOTE_VELOCITY);
    d->phLock = VCALL0(int, note, VSNOTE_PHLOCK);
    const wchar_t *ly = VCALL0(const wchar_t *, note, VSNOTE_LYRIC);
    mswstr_set(&d->lyric, ly ? ly : L"a");
    unsigned n = VCALL0(unsigned, note, VSNOTE_PHCOUNT);
    if (n > 32) n = 32;
    PhonemeAt_t ph = VMETHOD(PhonemeAt_t, note, VSNOTE_PHONEME);
    if (n) {
        d->phonemes.first = (MsWString *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n * sizeof(MsWString));
        for (unsigned i = 0; i < n; i++) {
            const wchar_t *p = ph(note, i);
            mswstr_set(&d->phonemes.first[i], p ? p : L"");
        }
        d->phonemes.last = d->phonemes.end = d->phonemes.first + n;
    }
}

static void nt_free(VSNoteData *d)
{
    mswstr_free(&d->lyric);
    for (MsWString *s = d->phonemes.first; s && s < d->phonemes.last; s++) mswstr_free(s);
    if (d->phonemes.first) HeapFree(GetProcessHeap(), 0, d->phonemes.first);
    memset(d, 0, sizeof *d);
}

/* replace lyric and phonemes ("a" -> lyric a, phoneme a; several phonemes space-separated) */
static void nt_set_sound(VSNoteData *d, const wchar_t *lyric, const wchar_t *phonemes)
{
    for (MsWString *s = d->phonemes.first; s && s < d->phonemes.last; s++) mswstr_free(s);
    if (d->phonemes.first) HeapFree(GetProcessHeap(), 0, d->phonemes.first);
    d->phonemes.first = d->phonemes.last = d->phonemes.end = NULL;
    mswstr_free(&d->lyric);
    mswstr_set(&d->lyric, lyric);
    wchar_t buf[64];
    lstrcpynW(buf, phonemes, 64);
    int n = 0;
    MsWString tmp[8];
    for (wchar_t *t = buf; *t && n < 8;) {
        while (*t == L' ') t++;
        if (!*t) break;
        wchar_t *e = t;
        while (*e && *e != L' ') e++;
        wchar_t c = *e;
        *e = 0;
        memset(&tmp[n], 0, sizeof tmp[n]);
        mswstr_set(&tmp[n++], t);
        if (!c) break;
        t = e + 1;
    }
    if (n) {
        d->phonemes.first = (MsWString *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n * sizeof(MsWString));
        memcpy(d->phonemes.first, tmp, n * sizeof(MsWString));
        d->phonemes.last = d->phonemes.end = d->phonemes.first + n;
    }
    d->phLock = 1;                    /* or the editor would redo them from the lyric */
}

static BOOL nt_vowel(const wchar_t *p)
{
    static const wchar_t *const jp[] = { L"a", L"i", L"M", L"e", L"o", NULL };
    static const wchar_t *const en[] = { L"@", L"V", L"e", L"I", L"i:", L"{", L"O:", L"Q", L"U", L"u:", L"@r", L"eI", L"aI", L"OI", L"@U", L"aU",
                                         L"I@", L"e@", L"U@", L"O@", L"Q@", NULL };
    for (const wchar_t *const *v = g_ntLang == LANG_JP ? jp : en; *v; v++) if (!lstrcmpW(p, *v)) return TRUE;
    return FALSE;
}

/* --------------------------------------------------------- targets -- */

typedef struct { NoteRef r; BOOL target; } NtNote;
static NtNote g_nt[8192];

static int cmp_nt(const void *a, const void *b) { return ((const NtNote *)a)->r.pos - ((const NtNote *)b)->r.pos; }

/* every note of the part, sorted, with the selected ones marked (all of
 * them if nothing is selected and allowAll); returns the count, 0 = nothing to do */
static int nt_collect(PartRef *pr, BOOL allowAll, int *ntargets)
{
    static NoteRef refs[8192];
    static unsigned sel[4096];
    *ntargets = 0;
    if (!open_part(pr)) return 0;
    int n = part_notes(pr, refs, 8192), ns = selected_ids(sel, 4096);
    for (int i = 0; i < n; i++) {
        g_nt[i].r = refs[i];
        g_nt[i].target = ns == 0 && allowAll;
        for (int k = 0; k < ns && !g_nt[i].target; k++) g_nt[i].target = sel[k] == refs[i].id;
        if (g_nt[i].target) (*ntargets)++;
    }
    qsort(g_nt, n, sizeof g_nt[0], cmp_nt);
    return n;
}

static void *nt_begin(PartRef *pr)
{
    void *ed = pGetVsqEditor(pr->mgr, 2);
    if (!ed) toast_show(L"Busy - try again in a moment");
    return ed;
}

/* commit or roll back; returns TRUE when committed */
static BOOL nt_end(PartRef *pr, void *ed, BOOL ok, int changed)
{
    if (ok && changed) ok = VCALL0(BOOL, ed, VSED_COMMIT);
    else ok = FALSE;
    if (!ok) VCALL0(void, ed, VSED_ROLLBACK);
    pReleaseVsqEditor(pr->mgr, &ed);
    return ok;
}

static BOOL nt_update(void *ed, unsigned id, VSNoteData *d)
{
    UpdateNote_t update = VMETHOD(UpdateNote_t, ed, VSED_UPDATE_NOTE);
    BOOL ok = update(ed, id, d);
    if (!ok) A->Log("notetools: update of note %u failed, error %08x", id, VCALL0(unsigned, ed, VSED_LASTERROR));
    return ok;
}

static BOOL nt_insert(void *ed, unsigned listId, VSNoteData *d)
{
    static const int expr[8] = { 8, 0, 50, 50, 50, 50, 127, 0 }, vib[4] = { 0, 0, 0, 0 };
    InsertNote_t insert = VMETHOD(InsertNote_t, ed, VSED_INSERT_NOTE);
    BOOL ok = insert(ed, listId, d, expr, vib);
    if (!ok) A->Log("notetools: insert failed, error %08x", VCALL0(unsigned, ed, VSED_LASTERROR));
    return ok;
}

static void nt_report(BOOL ok, int changed, const wchar_t *what, const wchar_t *none)
{
    wchar_t t[128];
    if (!changed) lstrcpynW(t, none, 128);
    else if (!ok) lstrcpyW(t, L"The editor refused the change (see the log)");
    else wsprintfW(t, what, changed, changed == 1 ? L"" : L"s");
    toast_show(t);
}

/* ------------------------------------------------------------ tools -- */

static void __cdecl tool_quantize(void *c)
{
    (void)c;
    PartRef pr;
    int nt, n = nt_collect(&pr, TRUE, &nt), changed = 0;
    long q = g_view ? quantize_ticks() : 0;
    if (!n || q <= 1) { toast_show(q <= 1 ? L"Set a QUANTIZE grid first" : L"No notes"); return; }
    void *ed = nt_begin(&pr);
    if (!ed) return;
    BOOL ok = TRUE;
    for (int i = 0; i < n && ok; i++) {
        if (!g_nt[i].target) continue;
        long a = pr.partPos + g_nt[i].r.pos, b = a + g_nt[i].r.dur;
        long na = (a + q / 2) / q * q, nb = (b + q / 2) / q * q;
        if (nb <= na) nb = na + q;
        if (na == a && nb == b) continue;
        VSNoteData d;
        nt_read(g_nt[i].r.note, &d);
        d.pos = (int)(na - pr.partPos);
        d.dur = (int)(nb - na);
        ok = d.pos >= 0 && nt_update(ed, g_nt[i].r.id, &d);
        nt_free(&d);
        if (ok) changed++;
    }
    ok = nt_end(&pr, ed, ok, changed);
    nt_report(ok, changed, L"Quantized %d note%s", L"Already on the grid");
}

static void __cdecl tool_roll(void *c)
{
    (void)c;
    PartRef pr;
    int nt, n = nt_collect(&pr, FALSE, &nt), changed = 0;
    if (!nt) { toast_show(L"Select the notes to roll an R into"); return; }
    void *ed = nt_begin(&pr);
    if (!ed) return;
    BOOL ok = TRUE;
    int total = g_rollCount * g_rollTicks;
    for (int i = 0; i < n && ok; i++) {
        if (!g_nt[i].target || g_nt[i].r.dur < total + 30) continue;     /* room for the roll and some vowel */
        VSNoteData d;
        nt_read(g_nt[i].r.note, &d);
        /* the note itself starts after the roll... */
        d.pos += total;
        d.dur -= total;
        ok = nt_update(ed, g_nt[i].r.id, &d);
        /* ...and the flaps lead into it */
        for (int k = 0; k < g_rollCount && ok; k++) {
            VSNoteData f;
            memset(&f, 0, sizeof f);
            f.pos = g_nt[i].r.pos + k * g_rollTicks;
            f.dur = g_rollTicks;
            f.noteNum = d.noteNum;
            f.velocity = d.velocity;
            nt_set_sound(&f, L"r", g_ntLang == LANG_JP ? L"4" : L"r");
            ok = nt_insert(ed, pr.listId, &f);
            nt_free(&f);
        }
        nt_free(&d);
        if (ok) changed++;
    }
    ok = nt_end(&pr, ed, ok, changed);
    nt_report(ok, changed, L"Rolled the R on %d note%s", L"The selected notes are too short to roll");
}

/* whisper: add (on) or remove (off) "_0" on the vowel phonemes */
static void nt_whisper(BOOL on)
{
    PartRef pr;
    int nt, n = nt_collect(&pr, FALSE, &nt), changed = 0;
    if (!nt) { toast_show(on ? L"Select the notes to whisper" : L"Select the notes to unwhisper"); return; }
    void *ed = nt_begin(&pr);
    if (!ed) return;
    BOOL ok = TRUE;
    for (int i = 0; i < n && ok; i++) {
        if (!g_nt[i].target) continue;
        VSNoteData d;
        nt_read(g_nt[i].r.note, &d);
        BOOL any = FALSE;
        for (MsWString *p = d.phonemes.first; p && p < d.phonemes.last; p++) {
            wchar_t s[24];
            lstrcpynW(s, mswstr_c(p), 20);
            int len = lstrlenW(s);
            BOOL has = len > 2 && !lstrcmpW(s + len - 2, L"_0");
            if (on && !has && nt_vowel(s)) { lstrcatW(s, L"_0"); any = TRUE; }
            else if (!on && has) { s[len - 2] = 0; any = TRUE; }
            else continue;
            mswstr_free(p);
            mswstr_set(p, s);
        }
        if (any) {
            d.phLock = 1;
            ok = nt_update(ed, g_nt[i].r.id, &d);
            if (ok) changed++;
        }
        nt_free(&d);
    }
    ok = nt_end(&pr, ed, ok, changed);
    nt_report(ok, changed, on ? L"Whispered %d note%s" : L"Unwhispered %d note%s", on ? L"No vowels to whisper" : L"Nothing was whispered");
}

static void __cdecl tool_whisper(void *c) { (void)c; nt_whisper(TRUE); }
static void __cdecl tool_unwhisper(void *c) { (void)c; nt_whisper(FALSE); }

static void __cdecl tool_breaths(void *c)
{
    (void)c;
    PartRef pr;
    int nt, n = nt_collect(&pr, TRUE, &nt), changed = 0;
    if (!n) { toast_show(L"No notes"); return; }
    void *ed = nt_begin(&pr);
    if (!ed) return;
    BOOL ok = TRUE;
    for (int i = 0; i < n && ok; i++) {
        if (!g_nt[i].target) continue;
        int start = g_nt[i].r.pos, prevEnd = i ? g_nt[i - 1].r.pos + g_nt[i - 1].r.dur : -100000;
        int gap = start - prevEnd;
        if (gap < 120) continue;                       /* not a phrase start: no room to breathe */
        if (i && !lstrcmpiW(VCALL0(const wchar_t *, g_nt[i - 1].r.note, VSNOTE_LYRIC), L"br1")) continue;
        int len = gap >= 300 ? 240 : gap - 60;         /* an eighth, or what fits */
        if (start - len < 0) len = start;
        if (len < 60) continue;
        VSNoteData b;
        memset(&b, 0, sizeof b);
        b.pos = start - len;
        b.dur = len;
        b.noteNum = g_nt[i].r.num;
        b.velocity = 64;
        nt_set_sound(&b, L"br1", L"br1");
        ok = nt_insert(ed, pr.listId, &b);
        nt_free(&b);
        if (ok) changed++;
    }
    ok = nt_end(&pr, ed, ok, changed);
    nt_report(ok, changed, L"Added %d breath%s", L"No phrase has a gap to breathe in");
}

/* backing vocals: every selected note sings one plain sound */
static void nt_backing(int kind)
{
    static const wchar_t *const lyr[2][3] = { { L"\x3046", L"\x3042", L"\x3093" }, { L"oo", L"ah", L"n" } };   /* う あ ん */
    static const wchar_t *const ph[2][3] = { { L"M", L"a", L"N\\" }, { L"u:", L"Q", L"n" } };
    static const wchar_t *const names[3] = { L"ooh", L"aah", L"hum" };
    PartRef pr;
    int nt, n = nt_collect(&pr, FALSE, &nt), changed = 0;
    if (!nt) { toast_show(L"Select the notes for the backing vocals"); return; }
    void *ed = nt_begin(&pr);
    if (!ed) return;
    BOOL ok = TRUE;
    for (int i = 0; i < n && ok; i++) {
        if (!g_nt[i].target) continue;
        VSNoteData d;
        nt_read(g_nt[i].r.note, &d);
        nt_set_sound(&d, lyr[g_ntLang][kind], ph[g_ntLang][kind]);
        ok = nt_update(ed, g_nt[i].r.id, &d);
        nt_free(&d);
        if (ok) changed++;
    }
    ok = nt_end(&pr, ed, ok, changed);
    wchar_t what[64];
    wsprintfW(what, L"Backing vocals (%s) on %%d note%%s", names[kind]);
    nt_report(ok, changed, what, L"Nothing to change");
}

static void __cdecl tool_ooh(void *c) { (void)c; nt_backing(0); }
static void __cdecl tool_aah(void *c) { (void)c; nt_backing(1); }
static void __cdecl tool_hum(void *c) { (void)c; nt_backing(2); }

static void nt_lock(BOOL lock)
{
    PartRef pr;
    int nt, n = nt_collect(&pr, FALSE, &nt), changed = 0;
    if (!nt) { toast_show(L"Select the notes first"); return; }
    void *ed = nt_begin(&pr);
    if (!ed) return;
    BOOL ok = TRUE;
    for (int i = 0; i < n && ok; i++) {
        if (!g_nt[i].target) continue;
        VSNoteData d;
        nt_read(g_nt[i].r.note, &d);
        if (!d.phLock == !lock) { nt_free(&d); continue; }
        d.phLock = lock;
        ok = nt_update(ed, g_nt[i].r.id, &d);
        nt_free(&d);
        if (ok) changed++;
    }
    ok = nt_end(&pr, ed, ok, changed);
    nt_report(ok, changed, lock ? L"Locked the phonemes of %d note%s" : L"Unlocked the phonemes of %d note%s", lock ? L"Already locked" : L"Already unlocked");
}

static void __cdecl tool_lock(void *c) { (void)c; nt_lock(TRUE); }
static void __cdecl tool_unlock(void *c) { (void)c; nt_lock(FALSE); }

/* ---------------------------------------------------------- settings -- */

static void nt_checks(void)
{
    for (int i = 0; i < 2; i++) A->CheckMenuItem(g_cmdLang[i], g_ntLang == i);
    for (int i = 0; i < 4; i++) A->CheckMenuItem(g_cmdRollN[i], g_rollCount == kRollCounts[i]);
    for (int i = 0; i < 4; i++) A->CheckMenuItem(g_cmdRollT[i], g_rollTicks == kRollTicks[i]);
}

static void nt_save(void)
{
    vh_ini_set_int(g_ini, L"NoteTools", L"Language", g_ntLang);
    vh_ini_set_int(g_ini, L"NoteTools", L"RollCount", g_rollCount);
    vh_ini_set_int(g_ini, L"NoteTools", L"RollTicks", g_rollTicks);
    nt_checks();
}

static void __cdecl menu_lang(void *c) { g_ntLang = (int)(INT_PTR)c; nt_save(); }
static void __cdecl menu_rolln(void *c) { g_rollCount = kRollCounts[(int)(INT_PTR)c]; nt_save(); }
static void __cdecl menu_rollt(void *c) { g_rollTicks = kRollTicks[(int)(INT_PTR)c]; nt_save(); }

static void notetools_menus(void)
{
    static const wchar_t *const rollN[4] = { L"Rolled R: 4 flaps", L"Rolled R: 6 flaps", L"Rolled R: 8 flaps", L"Rolled R: 12 flaps" };
    static const wchar_t *const rollT[4] = { L"Rolled R: very fast", L"Rolled R: fast", L"Rolled R: medium", L"Rolled R: slow" };
    const wchar_t *m = L"Note Tools";
    vh_menu_add(A, m, L"Quantize to grid", tool_quantize, NULL);
    vh_menu_add(A, m, L"Rolled R (selected notes)", tool_roll, NULL);
    vh_menu_add(A, m, L"Whisper (_0) selected notes", tool_whisper, NULL);
    vh_menu_add(A, m, L"Unwhisper selected notes", tool_unwhisper, NULL);
    vh_menu_add(A, m, L"Breaths before phrases", tool_breaths, NULL);
    vh_menu_add(A, m, L"Backing vocals: ooh", tool_ooh, NULL);
    vh_menu_add(A, m, L"Backing vocals: aah", tool_aah, NULL);
    vh_menu_add(A, m, L"Backing vocals: hum", tool_hum, NULL);
    vh_menu_add(A, m, L"Lock phonemes", tool_lock, NULL);
    vh_menu_add(A, m, L"Unlock phonemes", tool_unlock, NULL);
    for (int i = 0; i < 4; i++) g_cmdRollN[i] = vh_menu_add(A, m, rollN[i], menu_rolln, (void *)(INT_PTR)i);
    for (int i = 0; i < 4; i++) g_cmdRollT[i] = vh_menu_add(A, m, rollT[i], menu_rollt, (void *)(INT_PTR)i);
    g_cmdLang[LANG_JP] = vh_menu_add(A, m, L"Phonemes: Japanese", menu_lang, (void *)(INT_PTR)LANG_JP);
    g_cmdLang[LANG_EN] = vh_menu_add(A, m, L"Phonemes: English", menu_lang, (void *)(INT_PTR)LANG_EN);
    g_ntLang = vh_ini_int(g_ini, L"NoteTools", L"Language", LANG_JP) == LANG_EN ? LANG_EN : LANG_JP;
    g_rollCount = vh_ini_int(g_ini, L"NoteTools", L"RollCount", 6);
    g_rollTicks = vh_ini_int(g_ini, L"NoteTools", L"RollTicks", 20);
    if (g_rollCount < 2 || g_rollCount > 16) g_rollCount = 6;
    if (g_rollTicks < 10 || g_rollTicks > 60) g_rollTicks = 20;
    nt_checks();
}
