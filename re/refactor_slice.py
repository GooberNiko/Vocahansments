root = 'C:/Users/slimm/Desktop/Claude/Toolz/Vocahansments/'
p = root + 'src/mods/slicetool/slicetool.c'
L = open(p).read().split('\n')
head = L[0:41]
cut = L[173:379]
drop = ('#include "../../common/vh_api.h"', '#include "../../common/v4_internals.h"',
        '#include "../../common/vh_util.h"', 'static COLORREF themed(COLORREF c);')
head = [x for x in head if x not in drop]
head = [('static const VHApi *A;\n#include "../../common/v4_editor.h"' if x == 'static const VHApi *A;' else x) for x in head]
head = [('#include <windowsx.h>\n#include <commctrl.h>' if x == '#include <windowsx.h>' else x) for x in head]
tail = open(root + 're/slicetail.c').read()
open(p, 'w').write('\n'.join(head) + '\n\n' + '\n'.join(cut) + '\n' + tail)

q = root + 'src/common/v4_internals.h'
t = open(q).read()
old = "static const BYTE V4_SIG_OnLButtonDown[6] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 };"
assert old in t
t = t.replace(old, old + """
#define V4_MusicalPartWnd_OnMouseMove   0x0047C2E0   /* thiscall (UINT flags, int x, int y) */
static const BYTE V4_SIG_OnMouseMove[6] = { 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57 };
#define V4_CMD_PITCH_RENDER  1760   /* WM_COMMAND id of the "render pitch" button */

/* sent to the tool bar dialog when a mod tool switches on (wParam = its button id),
 * so other mod tools switch off */
#define V4_WM_MODTOOL (WM_APP + 0x51)""")
open(q, 'w').write(t)

b = root + 'build.sh'
s = open(b).read()
s = s.replace('SliceTool.dll src/mods/slicetool/slicetool.c -luser32 -lgdi32\n', 'SliceTool.dll src/mods/slicetool/slicetool.c -luser32 -lgdi32 -lcomctl32\n')
open(b, 'w').write(s)
print('ok')
