# OrchCapture — Design

Date: 2026-08-30
Status: Phase 1 built + live-tested; Phase 1b (two-tap) built, not yet live-tested.
Repo: `C:\AudioDev\Repos\OrchCapture`. Plugin code `Ocap`, VST3, MIDI effect.
GitHub: `johnpascu77-dotcom/OrchCapture` (public, like the rest of the Orch family).

---

## 1. Purpose

"What I hear" MIDI. OrchCapture sits **last** on each instrument track — after MPL → fan-out →
Randomize → OrchNoteFilter → OrchNoteMapper → OrchGate — and records the exact note stream that
reaches the instrument, plus the track's own name. The recorded takes export as a Dorico-ready
Standard MIDI File so the whole rig translates into a full score with the right instrument on each
staff.

It is **not** a humanization stripper: it records the played notes, swing and all. (A
quantize-for-notation pass is a Phase 3 option, not the default.)

### Why a plugin and not Bitwig's record tracks

- Bitwig has no "print MIDI output" (Bounce is audio only).
- Recording each FX track's output natively needs a shadow record track per instrument — ~25 of
  them, set up by hand, for a full orchestra. That is exactly the tedium OrchCapture removes.
- A plugin sees its own track's MIDI **and** its own track's name (VST3 channel context via
  `AudioProcessor::updateTrackProperties`) → capture + label with no shadow tracks.
- `processBlock` runs whether the editor is open or not → more reliable than MC's Score View
  buffer, which needs its tab visible.

### Chain position

```
Note source (MPL / clip) → [Randomize] → OrchNoteFilter → OrchNoteMapper → OrchGate → OrchCapture → instrument
```

Placed after OrchGate, so gate-dropped notes are correctly **absent** from the capture — the file
matches what sounds.

---

## 2. Phasing

| Phase | What | Status |
|---|---|---|
| **1 — MVP** | Per-track plugin. Transparent passthrough, one "most recent take" note buffer, host track name, editor status, per-instance export: drag-out `.mid` + "Save .mid to folder…". | **built + live-tested 2026-08-30** |
| **1b — Two-tap** | `tapRole` (Performance / Articulation), `ksZoneMin/Max` (default 12–23), `ksExportMode` (Inline / Separate Track / Exclude / KS Only). `CapturedNote.isKeyswitch` tagged at capture; `writeTakeMidi` lays out one or two note tracks. Same binary in both roles. See §5. | **built 2026-08-30, not yet live-tested** |
| **2 — Coordinator** | `InterprocessConnectionServer` on port **47826**, a Coordinator toggle, a multi-lane UI keyed on `trackName + tapRole`, one **Export All** → merged multi-track `.mid` + drag-out, performance/KS track pairs matched by name. | planned |
| **3 — Polish** | Tempo + section-marker track (from MC's blueprint, or a manual tempo map), transport/blueprint-triggered auto-arm, optional quantize-for-notation toggle, per-lane solo/exclude. | planned |

---

## 3. Phase 1 internals

### Parameters (`OrchCaptureParameters`)

| ID | Name | Default | Meaning |
|---|---|---|---|
| `enable` | Capture Enabled | on | Off = pure passthrough, nothing recorded. |
| `resetOnPlay` | New Take On Play | on | Start a fresh take each time the transport starts. Off = one continuous take across stop/start until **Clear Take**. |
| `tapRole` | Tap Role | Performance | Performance / Articulation. Drives the editor subtitle and (Phase 2) the coordinator's grouping. Does *not* change capture behaviour — the two roles differ only by where the instance is placed and the KS settings. |
| `ksZoneMin` / `ksZoneMax` | KS Zone Min / Max | 12 / 23 | Note range treated as keyswitches. A captured note in `[min, max]` gets `isKeyswitch = true`. Default 12–23 = OrchNoteMapper's unified source window. |
| `ksExportMode` | KS Export | Inline | Inline (one track), Separate Track (musical on track 1, KS on track 2 `<name> KS`), Exclude (musical only), KS Only. |

State (`getStateInformation`) persists parameters only. The take itself is **ephemeral** — like MC's
Score View buffer, it is not saved with the project.

### Capture (audio thread, `processBlock`)

- MIDI passes through **untouched** — this plugin only observes.
- Playhead: `getPlayHead()->getPosition()` gives `isPlaying`, `bpm`, `ppqPosition` (block start).
  Event ppq = `blockStartPpq + samplePosition * (bpm / 60 / sampleRate)` — per-sample interpolation.
- **Take reset** on: transport start (`resetOnPlay`), or a backwards transport jump > 0.5 beat
  (rewind-and-replay). On reset the buffer clears and `takeStartPpq` is set to the current ppq;
  every stored note is relative to that origin, so the exported file starts at its own bar 1.
- Note pairing: note-on pushes an open note; note-off matches the oldest open note of the same
  `(channel, note)` (FIFO) and finalizes `{ppqOn, ppqOff, note, velocity, channel}`. All-notes-off
  / all-sound-off and a transport stop finalize any still-open notes at the current ppq.
- Buffers guarded by a `SpinLock`: `processBlock` appends briefly, the editor snapshots. Contention
  is near-zero (monophonic orchestral parts; export happens with the transport stopped).
- Safety rails: 200k captured notes max, 4096 open notes max (drop-oldest).

### Track name

`updateTrackProperties` (message thread, VST3 channel context) caches `properties.name`. Editor
shows `Track: <name>`, or `Track: OrchCapture  (host sent no name)` as the fallback. Confirmed
working in Bitwig 2026-08-30.

### Export (`OrchCaptureTakeLogic`, pure)

`writeTakeMidi(notes, options, stream)` → Format-1 SMF, 960 tpqn:
- **Track 0** — track-name text meta + tempo meta.
- **Track 1 (..2)** — one or two note tracks, per `options.keyswitchMode` (see §5).

`normalizeTake` drops out-of-range pitches, clamps onsets to ≥ 0, forces a minimum positive note
length, sorts by onset then pitch, and preserves `isKeyswitch`. Exercised directly by
`OrchCaptureTakeLogicCheck` (console app, `juce_audio_basics` only).

### Editor

- **Capture Enabled**, **New take on transport start** toggles.
- **Tap Role**, **KS Zone** (min / max), **KS Export** — see §5.
- `Track:` and take-status lines (10 Hz): `Take: N notes (K KS) | M bars | recording/stopped | last …`.
- **Drag MIDI out** pad — drag off it to drop `<trackname>.mid` onto a Bitwig track
  (`performExternalDragDropOfFiles`; the editor is the `DragAndDropContainer`). Disabled while
  playing or with an empty take. Proven pattern — MC's Score View does the same in Bitwig.
- **Save .mid to folder…** — `FileChooser`, default name `<trackname>.mid`.
- **Clear Take** — discard and listen fresh.

---

## 4. Phase 1 risks — verified live in Bitwig 2026-08-30

1. `updateTrackProperties` **does** deliver the track name in Bitwig — confirmed (`Track: Double Bass` etc.).
2. Drag-out from the plugin editor **works** in Bitwig.
3. Per-sample ppq interpolation lines up cleanly enough (as-performed; quantize toggle is the Phase 3 backstop).

Cosmetic: the Drag pad's arrow glyph was mojibake in the first build (raw UTF-8 in a `const char*`);
fixed via `juce::CharPointer_UTF8`.

---

## 5. Two-tap roles (Phase 1b)

The unified articulation vocabulary exists at exactly one point in the chain — **between
OrchNoteFilter and OrchNoteMapper** — where every track's keyswitches sit at the same note numbers
(the marker is C-1 = MIDI 12, spread across 12–18 by a Bitwig *Randomize Pitch* 0–6 device, one per
the 7 Iconica Sketch articulation slots). Downstream of OrchNoteMapper each library has its own
*destination* KS range (Iconica: 24–35 for most instruments, 72–83 for Double Bass and
Contrabassoon), so there is no single rule to classify keyswitches at the tail.

Full analysis: [`unified_keyswitch_articulation_scoping.md`](unified_keyswitch_articulation_scoping.md).

| | **Performance** role | **Articulation** role |
|---|---|---|
| Position | tail, after OrchNoteMapper | between OrchNoteFilter and OrchNoteMapper |
| Records | what actually sounds — the notation source | the unified keyswitch stream |
| `tapRole` | Performance | Articulation |
| `ksZoneMin/Max` | that instrument's destination range (only if using a non-Inline mode) | **12–23**, same on every track |
| `ksExportMode` | **Inline** (don't classify at the tail) | **KS Only** (or Separate Track) |

Same binary — the two roles differ only by parameter values and where the instance is inserted.
`isKeyswitch` is set at capture time by testing the note against `[ksZoneMin, ksZoneMax]`;
`ksExportMode` then decides the SMF layout:

- **Inline** — every note on track 1 (default).
- **Separate Track** — musical notes on track 1 `<name>`, keyswitches on track 2 `<name> KS`
  (KS track omitted if there are none, so Dorico never gets an empty staff).
- **Exclude** — musical notes only.
- **KS Only** — keyswitches only, track named `<name> KS`.

The downstream music21 / MusicXML pass reads keyswitches by track name and maps `12 → slot 1`,
`13 → slot 2`, … to a per-library technique name — no threshold guessing.

The clip-fed **wash tracks** still take their notation from the *tail* (Performance) capture: the
pre-Mapper stream is not range-mapped and has not been through OrchGate participation, so it contains
notes outside the instrument's range and notes that never sounded. The Articulation tap supplies only
the keyswitch events, aligned to the performance take by track name + onset ppq.

---

## 6. Build

```
cmake -S . -B build
cmake --build build --config Release --target OrchCapture_VST3
cmake --build build --config Release --target OrchCaptureTakeLogicCheck
```

`COPY_PLUGIN_AFTER_BUILD` is **FALSE** (matches OrchNoteFilter — the post-build copy into
`C:\Program Files\Common Files\VST3\` needs an elevated shell). Copy the built
`OrchCapture.vst3` there by hand, or flip the flag once elevated.
