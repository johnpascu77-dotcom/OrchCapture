# Unified Keyswitch / Articulation Flow — Scoping

Date: 2026-08-30
Status: scoping only. **No plugin source was modified.** Read-only analysis of
OrchNoteFilter, OrchNoteMapper, OrchGate, OrchCapture, OrchConductor docs, MPL and MC.

---

## 1. Problem statement

Keyswitch (KS) notes reach OrchCapture at the tail of the chain, where they are already each
library's *destination* KS notes. In Iconica Sketch, Double Bass and Contrabassoon put their KS zone
in a high octave while other instruments put theirs low, so at OrchCapture's position **there is no
single note-range threshold, and no tidy per-family rule, that separates keyswitches from
performance notes across ~25 tracks.**

The goal is not to reject keyswitches — a future music21/MusicXML pass will convert them into real
notated articulations. The goal is to capture them somewhere they are *identifiable*.

The finding of this pass: **the unified articulation vocabulary is real, it exists at exactly one
place in the chain, and it is not where OrchCapture currently sits.** It exists between
OrchNoteFilter's output and OrchNoteMapper's input. Two other things turned up along the way that
are arguably more urgent than the capture problem itself (§4.1 and §5.2) — both are silent
correctness bugs caused by the same over-broad default KS window, and both are config fixes.

---

## 2. End-to-end keyswitch trace

Chain (confirmed in `OrchCapture/Docs/OrchCapture_Design.md:34`):

```
Note source (MPL / clip) → [Bitwig Randomize] → OrchNoteFilter → OrchNoteMapper → OrchGate → OrchCapture → instrument
```

| # | Stage | What it does to a KS note | What it checks | Can it alter / drop / snap? |
|---|---|---|---|---|
| 0 | **MPL** | Nothing — MPL has no keyswitch concept at all. A grep of `C:\Users\Asus\Documents\JUCE\Projects\NewProject\Source\` for `keyswitch` / `articul` returns **zero hits**. | — | No. The 6 MPL-fed tracks emit **performance notes only**; every KS in the system originates in an authored clip. |
| 1 | **Bitwig Randomize ("+6")** | Transposes the clip's KS marker up into OrchNoteMapper's unified zone. **Host-native device — no code in any Orch repo reads, controls, or is aware of it.** | — | Yes, and uncontrollably. See §7 item 1. |
| 2 | **OrchNoteFilter** | **Passes it through byte-identical.** Early-out: `OrchNoteFilterProcessor.cpp:466-473`. | `passKeyswitches` (default **true**, `:179`), `keyswitchMin` (default **0**, `:181`), `keyswitchMax` (default **35**, `:183`) | **No** — see §3. The early-out precedes every mutating path. |
| 3 | **OrchNoteMapper** | **Remaps it** to the library's destination KS note, or passes it unchanged, depending on which of three branches it falls into (`PluginProcessor.cpp:746-760`). | `keyswitchMode`, `lowKsProtect`, `lowKsSourceMin/Max`, `lowKsMax`, and the high-side equivalents | **Yes** — this is the one intentional alteration. Also the one place a KS can silently *fail* to be remapped (§4.2). |
| 4 | **OrchGate** | Passes it through unchanged **if it is inside OrchGate's own KS window** (`OrchGateProcessor.cpp:116-125`), which is checked *before* the gate. | `passKeyswitches` (default **true**, `:332`), `keyswitchMin` (default **0**, `:339`), `keyswitchMax` (default **35**, `:346`) | **Yes — it can drop the note.** A destination-KS note outside 0..35 falls through to the gate and can be muted or thinned. See §5.2. |
| 5 | **OrchCapture** | Records it as an ordinary note. MIDI is never modified — `processBlock` has no write path (`OrchCaptureProcessor.cpp:101-105`, note-capture at `:173-206`). | Nothing. No pitch classification exists. | No alteration; but no classification either. `normalizeTake` only range-checks 0..127 (`OrchCaptureTakeLogic.cpp:16-17`). |

### Concrete note numbers

Code-derived defaults (these are the *defaults*, not necessarily what the user's 25 tracks are set
to — see §7 item 2):

- **Unified KS source zone** = OrchNoteMapper `lowKsSourceMin` **12** … `lowKsSourceMax` **23**
  (`PluginProcessor.cpp:170-184`).
- **Destination base** = `lowKsDestinationMin` **24** (`:186-192`), *overridden* whenever
  `ksDestinationPreset` is not "Custom", in which case base = `(presetIndex - 1) * 12`
  (`:526-536`) — i.e. the chosen chromatic octave band from the list at `:38-49`.
- **Map math**: `outputNote = destinationMin + (inputNote - sourceMin)`, clamped 0..127
  (`:551-553` low, `:688-690` high). A straight parallel shift — no wrap, no fold, no modulo.
- **High-side unified zone** = `highKsSourceMin` **96** … `highKsSourceMax` **107** (`:209-223`).
- **Protect windows are separate params**: `note <= lowKsMax` (default **35**) or
  `note >= highKsMin` (default **96**) — `:692-714`.

So with defaults and `keyswitchMode = "Map Low to Destination"`, a KS note at 12 leaves
OrchNoteMapper at 24; at 15 → 27; at 23 → 35.

---

## 3. Finding: OrchNoteFilter's KS protection is already complete

**Answer to "is ONF KS-aware, and is that enough?" — yes on protection, and it is airtight.**

`handleNoteOn` (`OrchNoteFilterProcessor.cpp:422`) runs in this order:

1. `enable` check → if off, everything passes raw (`:453-458`).
2. **KS early-out** (`:460-473`) — `trackNote(inputNote)`, emit the original message unchanged, `return`.
3. Probability roll (`:475-483`).
4. Field-morph state machine (`:491-528`).
5. `resolveNote` — the actual snap/drop (`:534`).

Because the early-out is step 2 and returns, **no** downstream path can touch a note inside
`[ksMin, ksMax]`:

- Not `probability < 100` — the roll is at `:476`, after the return.
- Not the broadcast mask CC — `handleControlCc` (`:307-420`) only writes *parameters*; the note path
  is unaffected, and a masked field still never reaches a KS note.
- Not field morph — `:491-528`, after the return.
- Not Field Width — applied inside `buildFieldConfig` (`:283`), only reached at `:522/:527`.
- Not Filter / Keep / Constrain / **Solo** — all four live in `resolveNote`
  (`OrchNoteFilterFieldLogic.cpp:134-165`), reached only at `:534`. Solo is the one worth naming
  explicitly: Solo *drops in-field notes* (`FieldLogic.cpp:162-163`), so without the early-out a KS
  note whose pitch class happened to be in-field would vanish. It doesn't, because of the early-out.

Note-off is symmetric: the KS note-on was entered into `activeNotes` with
`outputNote == inputNote` (`:468`), so `handleNoteOff` emits the original message (`:583-584`).

**But three things are missing or wrong:**

### 3.1 The KS zone is not CC-controllable

`handleControlCc` (`:307-420`) dispatches on Field Preset, Root, Shift, Mode, Probability, Field
Width, and the packed mask base. There is **no** `ccKeyswitch*` parameter in the layout
(`:178-206`) and no case for one. So the KS window cannot be broadcast from MC and cannot be kept in
sync with OrchNoteMapper's unified zone. Today it is 25 manual settings.

### 3.2 The default window is far too wide, and it silently disables the filter's own job

`ksMax` defaults to **35** (`:183`). Every performance note ≤ 35 therefore bypasses the pitch-class
field entirely. Cross-referencing OrchNoteMapper's practical instrument minima
(`OrchNoteMapper/Source/PluginProcessor.cpp:371-396`):

| Instrument | preset min | notes below 36 that ONF silently stops filtering |
|---|---|---|
| Contrabassoon | 22 | 22–35 |
| Double Bass | 28 | 28–35 |
| Tuba | 28 | 28–35 |
| Bassoon | 34 | 34–35 |
| Bass Trombone | 34 | 34–35 |
| French Horn | 35 | 35 |

On those six tracks the bottom of the register is exempt from the harmonic field. This is a real
harmony bug independent of the keyswitch question, and it is a settings fix, not a code fix.

### 3.3 It's per-instance and hand-maintained across ~25 tracks

Nothing enforces that all 25 ONF instances agree on the zone, or that they agree with OrchNoteMapper.

---

## 4. Finding: OrchNoteMapper's KS mechanism, precisely

`processMidiAndClearAudio` note-on branch, `PluginProcessor.cpp:736-793`, evaluated in this order:

```cpp
if      (isLowKeyswitchSourceNote (inputNote))  outputNote = mapLowKeyswitchNoteToDestination (inputNote);
else if (isHighKeyswitchSourceNote (inputNote)) outputNote = mapHighKeyswitchNoteToDestination (inputNote);
else if (isProtectedKeyswitchNote (inputNote))  outputNote = inputNote;   // pass through UNMAPPED
else                                            /* performance path: Clamp or Octave Fold */
```

### Parameters (all `AudioParameterInt` 0..127 unless noted)

| ID | Name | Choices / range | Default | Role |
|---|---|---|---|---|
| `keyswitchMode` | Keyswitch Mode | Off / Protect Only / Map Low to Destination / Map High to Destination / Map Low + High to Destination (`:31-36`) | **Off** (`:145`) | Master switch |
| `lowKsProtect` | Low KS Protect | Off / On | **Off** (`:159`) | Doubles as the *mapping enable* — see 4.1 |
| `lowKsMax` | Low KS Max | 0..127 | **35** | Protect window upper bound |
| `lowKsSourceMin` | Low KS Source Min | 0..127 | **12** | Unified zone lower bound |
| `lowKsSourceMax` | Low KS Source Max | 0..127 | **23** | Unified zone upper bound |
| `lowKsDestinationMin` | Low KS Destination Min | 0..127 | **24** | Destination base (fallback) |
| `highKsProtect` / `highKsMin` / `highKsSourceMin` / `highKsSourceMax` / `highKsDestinationMin` | — | 0..127 | Off / **96** / **96** / **107** / **24** | High-side equivalents (`:194-231`) |
| `ksDestinationPreset` | KS Destination | Custom, then 10 chromatic octave bands 0-11 … 108-119 (`:38-49`) | **Custom** (`:152`) | Overrides both destination bases when ≠ Custom |

**Which input range is the "unified KS zone"?** `[lowKsSourceMin, lowKsSourceMax]` = **12..23** by
default. **Is it protect-only, octave-map, or both?** Both, as separate windows with separate
params: `[0, lowKsMax]` is the *protect* window, `[lowKsSourceMin, lowKsSourceMax]` is the *map*
window, and they are independently configurable and by default overlapping (12..23 sits inside
0..35).

### 4.1 Gotcha: the Protect toggle is also the mapping enable

`isLowKeyswitchSourceNote` returns false unless **both** `keyswitchMode ∈ {2, 4}` **and**
`lowKsProtect == On` (`:504-514`). Same on the high side (`:652-662`). So a track set to "Map Low to
Destination" with `Low KS Protect = Off` does **no mapping at all** — the KS falls through to the
performance path and gets range-clamped into the instrument's playing range, where it sounds as a
wrong note. This is non-obvious from the parameter names and is a plausible cause of "some KS don't
arrive".

### 4.2 Gotcha: a KS just outside the source window passes through unmapped

This is the exact failure shape the user describes. With defaults and `keyswitchMode = 2`,
`lowKsProtect = On`:

| Input note | Branch taken | Output | Result at the instrument |
|---|---|---|---|
| 11 | `isProtectedKeyswitchNote` (`:756`, since 11 ≤ lowKsMax 35) | **11**, unmapped | Wrong KS or no KS |
| 12 | `isLowKeyswitchSourceNote` | 24 | Correct |
| 23 | `isLowKeyswitchSourceNote` | 35 | Correct |
| 24–35 | `isProtectedKeyswitchNote` | **unchanged**, unmapped | Wrong KS or no KS |

So a KS marker that lands *just* below or above the unified window is not dropped and not an error —
it is silently passed at the wrong pitch. Given that the Bitwig Randomize stage is what lifts the
markers into the window (§7 item 1), and Randomize is stochastic, this is exactly the mechanism that
would produce "**most** KS now arrive well" rather than "all".

### 4.3 Gotcha: Map Low + High share one destination base

`getEffectiveKeyswitchDestinationMin` (`:526-536`) is called by **both**
`mapLowKeyswitchNoteToDestination` (`:549`) and `mapHighKeyswitchNoteToDestination` (`:686`). When
`ksDestinationPreset` is anything other than "Custom", both windows resolve to the *same* base, so in
mode 4 ("Map Low + High") the low and high KS blocks collide on top of each other. Only "Custom"
gives them independent destinations. Probably not biting today (mode 4 is unlikely to be in use), but
it makes the preset list unusable for any library that has both a low and a high KS block.

### 4.4 The over-broad-protect problem again, worse here

With any `keyswitchMode ≠ Off` and `lowKsProtect = On`, **every** note ≤ `lowKsMax` (35) skips the
range mapper (`:704-705`). On Contrabassoon (min 22), Double Bass (28), Tuba (28), Bassoon (34), Bass
Trombone (34), French Horn (35), that means OrchNoteMapper is not doing its one job across the bottom
of the register. Symmetrically on the high side: with `highKsProtect = On`, notes ≥ 96 bypass mapping
— relevant to Piccolo (max 108), Violin (105), Glockenspiel (108), Xylophone (108).

### 4.5 No CC control

OrchNoteMapper has no CC input path at all. Its only controller handling is `blockControlCcs`
(`:847-858`), which *scrubs* CC 20-54 or 20-64 outbound. The unified zone cannot be broadcast to or
from it.

---

## 5. Finding: OrchGate

### 5.1 Does it pass KS untouched? Yes — when the note is in *its* window

`OrchGateProcessor.cpp:102-125`. The KS check runs **before** the gate-state logic (`:127-141`) and
before the note-on/note-off handling (`:143-173`), and covers note-**off** as well as note-on
(`:116`). A KS note in `[ksMin, ksMax]` is emitted verbatim and never enters `activeNotes`, so
`closeGateSafely` (`:352-367`) will not chase it with a note-off. Internally consistent.

### 5.2 But its default window is a *pre-mapper* window, and OrchGate sits *post-mapper*

This is the second silent bug. OrchGate's `keyswitchMin`/`keyswitchMax` default to **0..35**
(`:334-346`) — the same numbers as ONF, which are correct for the *unified* zone upstream. But
OrchGate is downstream of OrchNoteMapper and therefore sees **destination** KS notes.

Two consequences, in opposite directions:

- **High-KS instruments lose their keyswitches to the gate.** For Double Bass and Contrabassoon,
  whose Iconica KS zone is high, the destination KS note is well above 35, so it is *not* caught by
  `:120`, falls through to `:143-158`, and is dropped whenever the gate is closed or participation
  thins it. Reopening the gate then replays notes under whatever articulation was last successfully
  set — a stale-articulation bug that would present as "that instrument is in the wrong articulation
  after a tacet section".
- **Low-register performance notes cannot be muted.** On those same low instruments, real
  performance notes ≤ 35 hit `:120` and bypass the gate entirely. OrchGate cannot silence the bottom
  of Double Bass, Contrabassoon or Tuba, and `closeGateSafely` will not note-off them (they were
  never tracked at `:154`).

Both are fixed by setting OrchGate's window per track to that instrument's **destination** KS range
rather than leaving the upstream-shaped 0..35 default.

---

## 6. Finding: where the clean unified articulation vocabulary lives

**Between OrchNoteFilter's output and OrchNoteMapper's input.**

At that single point, and nowhere else in the chain:

- Every track's KS notes are at the **same** unified numbers (nominally 12..23). ONF passed them
  through byte-identical (§3), and OrchNoteMapper has not yet applied its per-instrument remap (§4).
  One global zone setting separates KS from performance notes on all ~25 tracks.
- Performance pitches are **harmonically** as clean as they get — post field-snap, pre-range-fold.

**The honest caveats, which decide the recommendation:**

- "Clean" here means clean *for keyswitch identification and pitch class*. It does **not** mean clean
  for notation. At this point performance notes have not been range-mapped, so they can sit far
  outside the instrument's playable range (that is precisely what OrchNoteMapper exists to fix —
  `PluginProcessor.cpp:365-429`), and gate participation has not run, so the stream contains notes
  that **never sound** (`OrchGateProcessor.cpp:148-158`).
- Randomize ran *upstream* of this tap, so the performance pitches here are already randomized. The
  tap is not a way to recover the pre-Randomize intent.

So this point is the right tap for the **articulation** stream and the wrong tap for the
**performance** stream. That is the whole argument for two taps rather than moving the existing one.

---

## 7. What "KS-aware ONF" would concretely mean

ONF is already KS-aware in the sense that matters (§3). Only two small things are missing, and one of
them is not code:

1. **Narrow the default / per-track KS window** from 0..35 to the actual unified zone. **Config
   change, zero code.** This is the fix for §3.2 and should happen first regardless of everything
   else in this document.
2. **Make `keyswitchMin` / `keyswitchMax` CC-addressable.** Add a `ccKeyswitchZoneNumber` param
   alongside the existing CC# params (`OrchNoteFilterProcessor.cpp:187-206`) and a case in
   `handleControlCc`, using the same guarded-write pattern as `ccFieldWidthNumber`
   (`:360-366`) — write the visible parameter, no hidden override. Roughly 20 lines. Only worth doing
   once something is actually broadcasting the zone.
3. *(Optional)* A status readout for "a performance note fell inside the KS window", so §3.2-class
   mistakes are visible rather than silent. ONF already has the split perf/KS readout infrastructure
   (`:49-53`).

Nothing else in ONF needs to change for articulation to flow correctly.

---

## 8. Recommendation for OrchCapture: two taps, one binary

**Two-tap, not one.** §6 establishes that no single position has both properties. But this needs
**one plugin binary** — the two roles differ only by parameter values.

| | **Performance** role | **Articulation** role |
|---|---|---|
| Position | tail, after OrchGate (unchanged from today) | between OrchNoteFilter and OrchNoteMapper |
| Records | what actually sounds — the Dorico notation source | the unified articulation stream |
| KS zone | that instrument's **destination** range (per-track) | the **unified** zone (same on all tracks) |
| KS export mode | **Inline** (record everything; don't pretend to classify) | **KS Only** |

The Performance instance defaults to Inline deliberately: at the tail there is no reliable global
rule, which is the user's original observation. Classification there would need a per-instrument
destination window, which is the same 25-settings maintenance burden — so don't build the workflow on
it. Let the Articulation tap own classification, where one global number pair is correct.

### Proposed parameters

Add to `createParameterLayout` (`OrchCaptureProcessor.cpp:24-35`, currently just `enable` and
`resetOnPlay`):

| ID | Type | Default | Notes |
|---|---|---|---|
| `tapRole` | Choice: Performance / Articulation | Performance | Drives defaults and the coordinator's grouping |
| `ksZoneMin` | Int 0..127 | 0 | |
| `ksZoneMax` | Int 0..127 | 35 | |
| `ksExportMode` | Choice: Inline / Separate Track / Exclude / KS Only | Inline | |
| `ccKsZoneNumber` | Int 0..127 | 0 (off) | Optional, Phase C |

**Use min/max, not base/count.** Base/count is OrchNoteMapper *roadmap* language (Phase 7B,
`OrchNoteMapper/Docs/ROADMAP.md:910-920`), but every plugin that actually shipped a KS window uses
min/max: ONF `keyswitchMin`/`keyswitchMax` (`OrchNoteFilterProcessor.cpp:180-183`), OrchGate
`keyswitchMin`/`keyswitchMax` (`OrchGateProcessor.cpp:334-346`), OrchNoteMapper
`lowKsSourceMin`/`lowKsSourceMax` (`PluginProcessor.cpp:170-184`). Introducing a third shape would be
gratuitous. One vocabulary, one number pair, four plugins.

**KS export mode = Separate Track is the one that matters for music21.** Writing keyswitches to their
own SMF track named `<trackname> KS` means the downstream script reads them by track name and never
has to guess a threshold — which is the entire problem this document is about. `writeTakeMidi`
(`OrchCaptureTakeLogic.cpp:56-95`) already emits Format-1 with a meta track plus one note track;
adding a third track is a small extension of the same function. `CapturedNote` would gain an
`isKeyswitch` flag set at capture time.

### Phase 2 coordinator implications

- Instances register `{trackName, tapRole}`. **The key must be the pair** — two OrchCapture instances
  on the same Bitwig track report the *same* host track name from `updateTrackProperties`
  (`OrchCaptureProcessor.cpp:221-229`), so trackName alone collides.
- The coordinator groups by `trackName` and emits, per instrument, one performance track and (where
  an Articulation instance exists) one paired KS track. "Export All" then produces a single SMF whose
  track pairs are already matched by name — no post-hoc alignment.
- It must tolerate an Articulation instance with no Performance partner and vice versa (the user will
  not put both on all 25 tracks on day one).
- The multi-lane UI gains a role column: `Vln I — Performance`, `Vln I — Articulation`.

**Does the same binary work at both positions with just a mode/param difference? Yes.** OrchCapture
is already a pure observer (`OrchCaptureProcessor.cpp:101-105`) with no assumptions about its chain
position; nothing about the capture logic cares whether it sits before or after OrchNoteMapper.

---

## 9. Notation-tap gap: the clip-fed wash tracks

**The pre-OrchNoteMapper stream is not adequate as the Dorico notation source for the wash tracks.**
Two disqualifying reasons, both established above:

1. Notes there have not been range-mapped, so they can be outside the instrument's playable range
   (`PluginProcessor.cpp:365-429`). Notating them produces unplayable parts.
2. Gate participation has not run (`OrchGateProcessor.cpp:148-158`), so the stream contains notes
   that never sounded. The whole point of the tail position was that the file matches what is heard
   (`OrchCapture/Docs/OrchCapture_Design.md:37-38`).

So for the wash tracks, **the tail capture remains the notation source**; the upstream Articulation
tap supplies only the articulation events, and the music21 script aligns the two by track name and
onset ppq.

**No separate pre-Randomize capture is needed for the wash tracks.** Pre-Randomize on a clip-fed track
is just the authored clip, which the user already has in Bitwig — capturing it adds nothing. (This is
different from the 6 MPL-fed instruments, where MC's Score View drag-out gives the pre-Randomize
*structural* stream. That is a complementary artifact for a different purpose, and per the design
doc's own reasoning it is not the notation source either — `OrchCapture_Design.md:12-19`.)

---

## 10. Phased proposal

### Phase A — configuration only, no code. Do this first.

Independent of everything else, and it fixes two live bugs.

- **A1.** Narrow ONF `keyswitchMin`/`keyswitchMax` on every track from 0..35 to the actual unified
  zone. Fixes the silent low-register field bypass (§3.2) on Contrabassoon, Double Bass, Tuba,
  Bassoon, Bass Trombone, French Horn.
- **A2.** Set OrchGate's `keyswitchMin`/`keyswitchMax` **per track** to that instrument's
  *destination* KS range, not the upstream 0..35 default. Fixes gate-droppable keyswitches on
  Double Bass and Contrabassoon, and restores OrchGate's ability to mute the low register (§5.2).
- **A3.** Audit each OrchNoteMapper: confirm `keyswitchMode = Map Low to Destination` **and**
  `Low KS Protect = On` (§4.1 — the toggle is the mapping enable), that
  `lowKsSourceMin`/`lowKsSourceMax` bracket the post-Randomize marker range *exactly*, and that
  `lowKsMax` is narrowed so it does not swallow the low performance register (§4.4).

Expected result: the residual "some KS don't arrive" cases (§4.2) become visible as either fixed or
genuinely out-of-window, rather than being masked.

### Phase B — OrchCapture, small and self-contained

- **B1.** Add `tapRole`, `ksZoneMin`, `ksZoneMax`, `ksExportMode` (§8). Tag `CapturedNote` with
  `isKeyswitch` at capture time; honour `ksExportMode` in `writeTakeMidi`, including the
  Separate-Track path. Extend `OrchCaptureTakeLogicCheck` to cover it.
- **B2.** Editor: role selector, KS-zone fields, KS mode, and an `N notes / K keyswitches` readout.
- **B3.** Live-test the second instance at the ONF→OrchNoteMapper position on one track before
  rolling out to 25.

This is what makes the two-tap workflow possible, with one binary.

### Phase C — synchronisation. Only once the zone actually needs to change.

- **C1.** `ccKeyswitchZoneNumber` in ONF and OrchCapture (§7 item 2), same guarded-write pattern.
- **C2.** MC broadcasts the unified zone. **Defer.** It only pays off if the zone is being changed at
  runtime, which today it is not — the unified zone is a fixed rig constant. Hand-set values in
  Phase A are sufficient until proven otherwise.

### Phase D — defer

- **D1.** OrchNoteMapper Phase 7D articulation-slot mapping
  (`OrchNoteMapper/Docs/ROADMAP.md:981-1017`) — the real answer for libraries whose articulation
  *order* differs, not just its octave. Worth building only once the music21 script exists and the
  unified slot vocabulary is pinned down; doing it earlier bakes in guesses.
- **D2.** Fix the shared `getEffectiveKeyswitchDestinationMin` (§4.3) so Map Low + High can have
  independent destinations. Low priority — only blocks libraries using both blocks at once.
- **D3.** The music21 / MusicXML converter itself (outside these repos).

---

## 11. Needs user confirmation

1. **The "+6 Randomize" mechanism.** From the docs it is the **Bitwig-native Randomize device**, one
   per track — `OrchConductor/Docs/OrchConductor_Phase1I_Template_Experiment.md:16` and `:30`,
   `OrchConductor/Docs/OrchGate_OrchConductor_Roadmap_Updated.md:425-426` ("Bitwig Randomize device
   used per track. Randomized keyswitches used per track."), and the ONF source comment at
   `OrchNoteFilterProcessor.h:110-112` ("the +6 Randomize collapses distinct MPL pitches onto one
   value"). **No code in any repo reads, controls, or is aware of it.** Still needed:
   - Does "+6" mean a fixed +6 transpose, a random 0..+6 offset, or ±6?
   - Is it applied to KS markers and performance notes alike, or is there a note-range restriction /
     a second Randomize instance?
   This determines whether §4.2's out-of-window failure mode is systematic or stochastic.
2. **The actual unified KS note numbers in use.** Code *defaults* say 12..23. The user says clips
   author markers "around C-1" and Randomize lifts them into the zone. Whether C-1 displays as MIDI
   **0** or MIDI **12** in their Bitwig setup decides whether the post-Randomize markers land inside
   12..23 (→ correctly mapped) or in 0..11 (→ protected but **unmapped**, §4.2). Needed: the real
   numbers, and the real per-track OrchNoteMapper settings — this document reasons from defaults.
3. **How many distinct articulations** are in play, i.e. how wide the unified zone actually needs to
   be, and whether the clips currently distinguish more than one.
4. **Are OrchGate's KS windows still at the 0..35 default on all tracks?** If yes, Double Bass and
   Contrabassoon have gate-droppable keyswitches today (§5.2) and A2 is urgent.
5. **Should the wash tracks be notated at all,** or only the 6 MPL-fed instruments? This decides
   whether the tail tap ever needs KS exclusion, or whether Inline is permanently fine there.

---

## 12. Source-vs-doc drift noticed in passing

Not acted on; noted so it doesn't mislead later readers.

- `OrchNoteFilter/Docs/OrchNoteFilter_Design.md:85-88` (§8 "Note-off safety") describes an
  `activeNoteMap[16][128]` with sentinel values. The shipped code uses a
  `std::vector<TrackedNote>` FIFO instead (`OrchNoteFilterProcessor.h:113-119`,
  `.cpp:428-436`, `:558-587`), changed deliberately because the wash feeder produces overlapping
  identical input notes. The doc was not updated.
