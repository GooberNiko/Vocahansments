# Vocahansments 🔪🎤

**Enhancements for VOCALOID4. But spelled wrong. On purpose. Probably.**

It's 2016. Your VOCALOID4 editor looks like an Excel spreadsheet that gave up. You cannot cut a note in half. You draw pitch curves like a medieval monk illuminating a manuscript, one dot at a time, by candlelight. You have accepted this.

**Stop accepting this.**

Vocahansments is a pile of native DLL mods that crawl *inside* the VOCALOID4 Editor (`VOCALOID4.exe` 4.x, 32 bit) and give it a knife, a pitch pen, a live pitch curve, themes, and, for reasons nobody can explain, **gravity**. They are not Job plugins. They are much worse than Job plugins. They are *inside the walls*.

| Mod | What it does | Vibe |
|---|---|---|
| **Slice Tool** | A knife for the piano roll. Button next to the eraser, `K` key, Mods menu entry. | 🔪 cut note |
| **Pitch Tools** | Real-time pitch curve, VOCALOID's own exact render with no progress window, real phoneme timing straight from the synth engine, and a **pitch pen**: draw the pitch you want right onto the piano roll (`P`). Also fry, growl, scale guides and a bag of note tools. | 🎢 wheee |
| **UI Colors** | Recolours the whole editor with a live theme editor, presets, eyedropper, gradients, rainbow notes, spinning notes, jelly notes, voicebank portraits and a physics engine. Yes. A physics engine. | 🌈🌀🍮🍎⬇️ |

> **Not affiliated with Yamaha.** Yamaha does not know about this. Please do not tell Yamaha. You need your own copy of VOCALOID4; nothing from it is included here.

---

## Install (the 30-second version)

1. **Close VOCALOID4.** It's shy. It won't let you touch its files while it's awake.
2. Grab `Vocahansments-0.1.zip` from [Releases](../../releases), unzip it anywhere (or just use the `dist\` folder from this repo).
3. Double-click `install.cmd`.
4. It asks for your VOCALOID4 folder: the one containing `VOCALOID4.exe`, usually `C:\Program Files (x86)\Vocaloid4`. Type it, paste it, or **drag the folder into the window** like a raccoon dragging a pizza.
5. Say yes to the admin prompt. (It writes into `Program Files`. Windows gets nervous about that. Windows gets nervous about everything.)
6. Start VOCALOID4. You'll see a **Mods** menu before Help and a 🔪 knife button after the eraser. Congratulations, your editor is armed now.

Prefer typing? `install.cmd "D:\path\to\Vocaloid4"` skips the question.

**Uninstall:** run `uninstall.cmd` and give it the same folder. Everything goes away except your themes and settings in `%APPDATA%\Vocahansments`, which we keep because we're sentimental.

**If it gets the folder wrong 5 times in a row it gives up and goes home.** Make sure the folder you give it actually has `VOCALOID4.exe` in it, not the one above or below it.

---

## 🔪 Slice Tool

Finally, you can cut a note. Humanity has waited for this since 2014.

| Input | Action |
|---|---|
| Knife button / `K` / Mods > Slice Tool | Toggle the knife. Arrow, pencil, line or eraser switch back. `Esc` also gets you out. |
| Click a note | Cut it at the cursor, snapped to the QUANTIZE grid. |
| Shift + click | Cut without snapping. Freehand. Dangerous. |
| Ctrl + click | Cut **every** note crossing that point, across all pitches. The samurai move. |

Each cut is one Undo step. The second half keeps the note's velocity, expression (bendDep/Len, rise/fallPort, decay, accent, opening) and vibrato, so it doesn't come out of the cut as a different person.

**Mods > Slice Tool > New piece holds the vowel ("-")** is on by default: the new piece gets lyric `-` and the original's last phoneme, so the syllable keeps going across the cut. Turn it off to repeat the full syllable instead, for that sweet s-s-s-stutter effect.

---

## 🎢 Pitch Tools

### Real-time Pitch Curve

On by default (Mods > Real-time Pitch Curve). A fitted model of VOCALOID's pitch draws the curve **instantly**, even while you're dragging notes around. Like VOCALOID's own render it has two layers: pitch without vibrato (orange) and the vibrato on top (blue). Once a note has been rendered, its *real* vibrato is used.

Shortly after each committed change (edits, undo, redo, opening another part), **Mods > Pitch Curve > Exact engine render after edits** quietly runs VOCALOID's own pitch render in the background, **with no progress window**, and swaps in the exact curve. It never renders while you're holding a mouse button or while the song plays, because it has manners.

Rendering freezes the editor for as long as it takes (about 0.2 s for a 30-bar part). The mod times each render and waits longer on long parts (about 3× the last render time, up to 4 s), and for slow renders it waits until you stop fiddling. Base delay: `LiveRenderDelayMs` (default 350) under `[PitchTools]` in `%APPDATA%\Vocahansments\settings.ini`.

### Phoneme Timing

Mods > Phoneme Timing, on by default. A lane above each note shows the phonemes exactly as the engine (DSE4) schedules them: consonants (purple) usually start *before* the note, vowels (blue) start on it, e.g. `|s| a |` over a `sa` note. This is read from the engine during the render, frame by frame (~5.8 ms), using the voicebank's own phoneme boundaries. **It is not a guess.** We don't do guesses. (We do a little bit of guessing: until the part renders again, new or changed lyrics are estimated from the phoneme lengths seen so far. The lane follows notes while you drag them.)

### 🖊️ Pitch Pen

Pen-and-wave button, `P`, or Mods > Pitch Pen. Draw the pitch. VOCALOID sings the pitch. That's it. That's the feature.

| Input | Action |
|---|---|
| Drag | Draw the pitch you want. Height is real pitch in the piano roll's semitone rows. |
| Shift + drag | Straight line from the start point. For people with rulers. |
| Right-drag | Erase: pitch bend back to 0 under the stroke. |
| Esc | Exit the tool (or cancel a stroke in progress). |

- The pen writes wherever VOCALOID is singing: on notes, and on the consonant and slide just before a note. Bits of your stroke over silent gaps are ignored, and existing data there is left alone.
- **Exact mode** (on by default, Mods > Pitch Pen > Exact): takes what VOCALOID sings without PIT from the last exact render and writes the difference to your stroke as PIT, all at once, no waiting. VOCALOID's own portamento and the note's vibrato are cancelled under the stroke. If notes changed since the last render, the part is rendered once first (only when the exact engine render is on; otherwise the real-time model is used).
- **Mods > Pitch Pen > Keep note vibrato on top** keeps the wobble, wobbling around your line.
- PBS is raised automatically when the bend needs more range (up to 24 semitones), and PIT/PBS are restored right after each stroke segment.
- Each stroke is one Undo step.

### Note Tools

Mods > Note Tools. They work on the selected notes, each is one Undo step, and a little message at the top of the piano roll tells you what just happened.

| Tool | What it does |
|---|---|
| Quantize to grid | Snaps note starts and ends to the QUANTIZE grid (whole part if nothing is selected). |
| Rolled R | Leads each note with a run of short flaps (`[4]` in Japanese, `[r]` in English). 4 to 12 flaps, very fast to slow. Brrrrrr. |
| Whisper (_0) / Unwhisper | Adds or removes `_0` (devoiced) on every vowel and locks the phonemes. Japanese voicebanks devoice `i` and `M`; other vowels only whisper if the voicebank has them. |
| Breaths before phrases | Puts a `[br1]` breath note in the gap before each phrase. Your singer no longer needs to be a scuba diver. |
| Backing vocals: ooh / aah / hum | The selected notes sing one plain sound (う `[M]`, あ `[a]`, ん `[N\]`, or the English equivalents), for harmonies and pads. |
| Lock / Unlock phonemes | Keeps the phonemes when the lyric changes, or lets VOCALOID redo them. |

**Phonemes: Japanese / English** picks which phoneme symbols the tools write.

### Vocal Fry & Growl

Mods > Vocal Fry & Growl... on the selected notes. Cranks pitch sensitivity (PBS) to 24 semitones and draws a PIT zig-zag under each note:

- **Vocal Fry (down)**: pitch snaps from the note all the way down and back, again and again. Very creaky. Very door.
- **Vocal Fry (up)**: the same, but up.
- **Growl**: a fast, rough zig-zag around the note. Grr.

Shape it with depth (up to 24 semitones), speed, randomness (each dip a different depth) and fade in/out. Breathiness, brightness, clearness and dynamics each have a value and a tick box and follow the same fade. There's a live preview. **Apply** is one Undo step and puts PIT, PBS and the voice controls back right after each note. **Remove from selected** cleans it up.

### Scale Guide & Note Info

- **Scale Guide** (Mods > Scale Guide): pick a scale (Major, Minor, Harmonic minor, Dorian, Major/Minor pentatonic, Blues) and a key. Rows outside the scale are dimmed, the key's own rows glow gold, and **Mark notes outside the scale** puts a red frame around the notes that are lying to you.
- **Note Info on Hover** (on by default): hover a note to see its name (middle C is C3, as VOCALOID labels it), length (1/4, 3/8, 2 bars...), duration in seconds, velocity and lyric.

---

## 🌈 UI Colors

Open **Mods > UI Colors > Theme editor...** and make VOCALOID stop looking like a 2009 tax form.

- **Theme**: VOCALOID4 default, the presets *Midnight*, *Miku*, *Sakura* and *High Contrast*, plus your own.
- The list shows every colour the editor has drawn since startup, filtered by **area** (Musical, Track, Mixer, Bars, Other, Bitmaps, Sprites) and **use** (fills, lines, text). Known colours have names like "Piano roll: white-key rows" or "Notes: overlapping another note", and named colours are always listed, even before VOCALOID draws them.
- **Double-click** a colour to replace it. Changes apply live.
- **Eyedropper**: click anything in VOCALOID and its colour opens in the picker.
- **Gradient → / Gradient ↓**: select a colour, then pick what it fades to. Gradients apply to fills; lines and text use the start colour.
- **Note text (lyrics)**: recolours the lyric on notes (`a [a]`) without touching any other text. In a theme file this is `[NoteText]`, keyed by the note's fill colour: `B5DC56` normal, `0BE9F4` selected, `C9F06A` overlapping, e.g. `B5DC56=FFFFFF`.
- **Quick tint**: hue, saturation, brightness and invert light/dark for everything at once.
- Editing a preset forks it into your own theme in `%APPDATA%\Vocahansments\themes\`, so presets never get overwritten.

Theme files are plain ini with `original=replacement` colours in `RRGGBB`:

```ini
[Theme]
Name=My Theme
[All]            ; everywhere
D8D8D8=2B2F3A
[Musical.Lines]  ; only lines drawn in the MUSICAL EDITOR
333333=8A93A8
```

A gradient is `RRGGBB>RRGGBB` (left to right) or `RRGGBBvRRGGBB` (top to bottom), e.g. `B5DC56=FF4040>4040FF`. Lookup order is `[Area.Kind]`, then `[Area]`, then `[All.Kind]`, then `[All]`. **Mods > UI Colors > Export colour census** dumps every colour the editor used, handy for hand-made themes.

**Note symbols** (the vibrato squiggle, the attack mark and the line between them) are in the **Sprites** area. Changing `#000000` recolours the whole symbol; its darker greys follow along unless you set them too. In a theme file: `[Sprites]` `000000=RRGGBB`.

### 🖼️ Voicebank Portraits

Mods > Voicebank Portraits..., like Synthesizer V: your voicebank stands at the bottom of the piano roll, faded, behind the notes, judging your melodies. The picture is, in order:

1. the one chosen in the Voicebank Portraits window (pick the voicebank, **Choose image...**);
2. `portrait.png` / `portrait.jpg` (or `<voicebank name>.png`) next to its `.vvd`, e.g. `C:\Program Files (x86)\VoiceDB\MikuV4_ENG\BMLTD846MLYP2MEK\portrait.png`;
3. `%APPDATA%\Vocahansments\portraits\<voicebank name>.png` (no admin rights needed).

PNG transparency is kept. You get **Show portraits**, **Opacity**, which side she stands on, and per voicebank **Size** (20 to 300% of the roll's height), **Left / right** and **Down / up** offsets and **Reset position**. Scrolling repaints cleanly instead of smearing her across the screen like a ghost.

### The Silly Section™

None of these change your song. They're just for looking at. You're welcome.

- **Rainbow Notes**: **Gradient** paints one rainbow across the roll so each note gets its own slice; **Colour changing** makes every note cycle through the rainbow forever. Selected and overlapping notes keep their colours so you can still find them.
- **Spinning Notes**: the notes spin around their centre, lyric and all. **Always spin** or **Spin only while playing**, four speeds from Slow to **Silly**, and **Every note its own speed and direction** for maximum chaos. Off-screen repainting, so no flicker. We are professionals.
- **Jelly Notes**: notes squash, stretch and sway like jelly, each to its own rhythm. Stacks with spinning, rainbow and gradients. 🍮
- **Gravity**: a real little physics engine, like Google Gravity. Your notes become solid boxes that fall, tumble, bounce, stack and slide. The grey strip under the roll is the floor, the sides are walls, the top is a ceiling (so nothing you throw is lost forever). **Drag tool** (on by default): grab a note, carry it, throw it. **Always** or **Only while playing**, gravity from Moon to Jupiter, bounce from None to Super bouncy, and **Drop them again**. Also: **Gravity: Space (none)**, **Explode!** and **Earthquake**. The rigid-body engine follows Box2D-Lite (`src/mods/uicolors/physics.c`). Your song is untouched. The boxes stay where they landed on screen even if you scroll, because they're boxes now. That's their life.

---

## 🧠 How it works (for nerds)

- `version.dll` is a proxy for the system `version.dll`, which the editor imports. Windows loads it from the editor's folder first. Classic.
- `VOCALOID4.exe` is packed with ASPack. The loader hooks the end of the unpacker stub, so import hooks and code hooks are in place before the editor's first real instruction runs. MFC's imports are hooked too.
- The loader loads every DLL in `vhmods\`, calls each one's `VH_ModInit(const VHApi*)`, and hands it import hooks, verified code hooks, the Mods menu, a log and the VSQ manager. See `src/common/vh_api.h`. Several mods can hook the same function; the loader chains them.
- **Every hard-coded address is checked against the expected bytes first.** On a different editor build the affected mod disables itself instead of exploding.
- Slice Tool edits notes through Vsq4.dll's own `IVSVsqEditorIF` (update note + insert note + commit), which is why Undo works. Mouse position becomes tick and pitch via the editor's `CGridManager` and the piano-roll pane state. Offsets live in `src/common/v4_internals.h`.
- Pitch Tools writes PIT/PBS through the same editor interface (control upsert/remove). It notices edits by hooking the editor's commit (Vsq4 vtable) and `editUndo`/`editRedo`, then runs the editor's own render command. The exact pen and real-time curve read the rendered curve the editor keeps on the piano-roll pane (`{tick, note, +portamento, +PIT, final}` in cents from MIDI 69). Since PIT only adds on top, the curve without PIT stays exact until the notes themselves change.
- UI Colors hooks GDI. Brushes and pens are tracked by handle and swapped at use time per editor area. Built-in bitmaps are re-tinted in place, with masked sprites handled separately.
- Log file: `vhmods\vocahansments.log`, or `%TEMP%\vocahansments.log` if that folder is read-only. Crashes get logged with a symbolised stack, so when it breaks, it at least tells you *where* it broke.

## 🔨 Building

You need MSYS2 with the mingw32 (i686) GCC toolchain. 32-bit. Like it's 2016. Because it is, in there.

```sh
./build.sh      # -> dist/version.dll, dist/vhmods/*.dll
```

`run_lab.sh` deploys to a user-writable copy of the editor in `lab/V4` (not in this repo, bring your own) and launches it for testing. Reverse-engineering scripts, notes and screenshots live in `re/`. VOCALOID's own binaries and decompiled code are **not** included, because Yamaha's lawyers have more HP than we do.

## 🐛 Something broke

1. Check `vhmods\vocahansments.log`.
2. Make sure you're on the 32-bit VOCALOID4 editor (4.x). Other builds make the mods politely switch themselves off.
3. Open an issue with the log. Screenshots of spinning notes are also accepted, even if nothing is broken.
