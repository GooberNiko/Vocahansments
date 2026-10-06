p = 'C:/Users/slimm/Desktop/Claude/Toolz/Vocahansments/src/mods/uicolors/editor.c'
s = open(p).read()


def sub(old, new, count=1):
    global s
    if old not in s:
        raise SystemExit('pattern not found:\n' + old[:300])
    s = s.replace(old, new) if count == 0 else s.replace(old, new, count)


sub("static int g_area;          /* area being edited */\nstatic HWND g_wnd, g_list, g_themeCombo, g_areaCombo,",
    "static int g_area;          /* area being edited */\nstatic int g_kind;          /* KIND_ANY = every use */\n#define CUR_TBL TBL(g_area, g_kind)\nstatic HWND g_kindCombo;\nstatic HWND g_wnd, g_list, g_themeCombo, g_areaCombo,")
sub("""        if (g_area != AREA_ALL && ce[i].area != g_area) continue;""",
    """        if (g_area != AREA_ALL && ce[i].area != g_area) continue;
        if (g_kind != KIND_ANY && ce[i].kind != g_kind) continue;""")
sub("""    for (int i = 0; theme_entry(g_area, i, &o, &rep); i++) row_for(o);""",
    """    for (int i = 0; theme_entry(CUR_TBL, i, &o, &rep); i++) row_for(o);""")
sub("""    wsprintfW(s, L"%d colours in %s  -  theme: %s", g_nrows, kAreaNames[g_area], g_themeName[0] ? g_themeName : L"(none)");""",
    """    wsprintfW(s, L"%d colours in %s%s%s  -  theme: %s", g_nrows, kAreaNames[g_area], g_kind ? L" / " : L"",
              kKindNames[g_kind], g_themeName[0] ? g_themeName : L"(none)");""")
sub("""    cc.rgbResult = remap(g_area, r->orig);""", """    cc.rgbResult = remap(g_area, g_kind, r->orig);""")
sub("""    if (cc.rgbResult == r->orig) theme_clear_entry(g_area, r->orig);
    else theme_set(g_area, r->orig, cc.rgbResult);""", """    if (cc.rgbResult == r->orig) theme_clear_entry(CUR_TBL, r->orig);
    else theme_set(CUR_TBL, r->orig, cc.rgbResult);""")
sub("""        theme_clear_entry(g_area, g_rows[i].orig);
    autosave();""", """        theme_clear_entry(CUR_TBL, g_rows[i].orig);
    autosave();""")
sub("""    while (theme_entry(g_area, 0, &o, &r)) theme_clear_entry(g_area, o);""",
    """    while (theme_entry(CUR_TBL, 0, &o, &r)) theme_clear_entry(CUR_TBL, o);""")
sub("""        if (n == g_rows[i].orig) theme_clear_entry(g_area, g_rows[i].orig);
        else theme_set(g_area, g_rows[i].orig, n);""", """        if (n == g_rows[i].orig) theme_clear_entry(CUR_TBL, g_rows[i].orig);
        else theme_set(CUR_TBL, g_rows[i].orig, n);""")
sub("""            COLORREF now = remap(g_area == AREA_ALL ? area : g_area, g_rows[i].orig);
            if (now == px || g_rows[i].orig == px)""", """            int a = g_area == AREA_ALL ? area : g_area;
            BOOL hit = g_rows[i].orig == px;
            for (int k = 0; k < KIND_COUNT && !hit; k++) hit = remap(a, k, g_rows[i].orig) == px;
            if (hit)""")
sub("""            g_area = AREA_ALL; ComboBox_SetCurSel(g_areaCombo, AREA_ALL); build_rows();""",
    """            g_area = AREA_ALL; g_kind = KIND_ANY; ComboBox_SetCurSel(g_areaCombo, AREA_ALL);
            ComboBox_SetCurSel(g_kindCombo, 0); build_rows();""")
sub("""            COLORREF now = theme_lookup(g_area, r->orig);
            draw_swatch_cell(cd->nmcd.hdc, rc, now == NO_COLOR ? remap(AREA_ALL, r->orig) : now, now != NO_COLOR);""",
    """            COLORREF own = theme_lookup(CUR_TBL, r->orig);
            draw_swatch_cell(cd->nmcd.hdc, rc, own == NO_COLOR ? remap(g_area, g_kind, r->orig) : own, own != NO_COLOR);""")
sub("""        mk(L"BUTTON", L"Eyedropper", BS_PUSHBUTTON | WS_TABSTOP, 232, 39, 100, 26, ID_PICK);
        mk(L"STATIC", L"Double-click a colour to change it. Eyedropper: click anything in VOCALOID.", SS_CENTERIMAGE, 342, 40, 420, 24, -1);
        for (int a = 0; a < AREA_COUNT; a++) ComboBox_AddString(g_areaCombo, kAreaNames[a]);
        ComboBox_SetCurSel(g_areaCombo, g_area);""", """        g_kindCombo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 232, 40, 120, 300, ID_KIND);
        mk(L"BUTTON", L"Eyedropper", BS_PUSHBUTTON | WS_TABSTOP, 360, 39, 100, 26, ID_PICK);
        mk(L"STATIC", L"Double-click a colour to change it. Eyedropper: click anything in VOCALOID.", SS_CENTERIMAGE, 470, 40, 420, 24, -1);
        for (int a = 0; a < AREA_COUNT; a++) ComboBox_AddString(g_areaCombo, kAreaNames[a]);
        ComboBox_SetCurSel(g_areaCombo, g_area);
        ComboBox_AddString(g_kindCombo, L"Every use");
        ComboBox_AddString(g_kindCombo, L"Fills only");
        ComboBox_AddString(g_kindCombo, L"Lines only");
        ComboBox_AddString(g_kindCombo, L"Text only");
        ComboBox_SetCurSel(g_kindCombo, g_kind);""")
sub("enum { ID_THEME = 100, ID_SAVEAS,", "enum { ID_THEME = 100, ID_KIND, ID_SAVEAS,")
sub("""        case ID_AREA: if (HIWORD(wp) == CBN_SELCHANGE) { g_area = ComboBox_GetCurSel(g_areaCombo); fill_list(); } break;""",
    """        case ID_AREA: if (HIWORD(wp) == CBN_SELCHANGE) { g_area = ComboBox_GetCurSel(g_areaCombo); fill_list(); } break;
        case ID_KIND: if (HIWORD(wp) == CBN_SELCHANGE) { g_kind = ComboBox_GetCurSel(g_kindCombo); fill_list(); } break;""")
open(p, 'w').write(s)
print('editor ok')
