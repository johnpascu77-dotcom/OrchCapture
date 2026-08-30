# OrchCapture — Design

Date: 2026-08-30
Status: Phase 1 (MVP) building.
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
| **1 — MVP** | Per-track plugin only. Transparent passthrough, one "most recent take" note buffer, host track name, editor status (`Track: X | Take: N notes | M bars | recording/stopped`). Per-instance export: drag-out `.mid` + "Save .mid to folder…". No coordinator. Import ~25 auto-named files into Dorico. | **building** |
| **2 — Coordinator** | `InterprocessConnectionServer` on port **47826**, a Coordinator toggle, a multi-lane UI (one row per connected instance), one **Export All** → merged multi-track `.mid` + drag-out. | planned |
| **3 — Polish** | Tempo + section-marker track (from MC's blueprint, or a manual tempo map), transport/blueprint-triggered auto-arm, optional quantize-for-notation toggle, per-lane solo/exclude. | planned |

---

## 3. Phase 1 internals

### Parameters (`OrchCaptureParameters`)

| ID | Name | Default | Meaning |
|---|---|---|---|
| `enable` | Capture Enabled | on | Off = pure passthrough, nothing recorded. |
| `resetOnPlay` | New Take On Play | on | Start a fresh take each time the transport starts. Off = one continuous take across stop/start until **Clear Take**. |

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
shows `Track: <name>`, or `Track: OrchCapture  (host sent no name)` as the fallback — a **Phase 1
risk to verify first** that Bitwig actually delivers the name.

### Export (`OrchCaptureTakeLogic`, pure)

`writeTakeMidi(notes, options, stream)` → Format-1 SMF, 960 tpqn:
- **Track 0** — track-name text meta + tempo meta.
- **Track 1** — track-name text meta, then note on/offs at as-performed ppq.

`normalizeTake` drops out-of-range pitches, clamps onsets to ≥ 0, forces a minimum positive note
length, and sorts by onset then pitch. Exercised directly by `OrchCaptureTakeLogicCheck` (console
app, `juce_audio_basics` only).

### Editor

- **Capture Enabled**, **New take on transport start** toggles.
- `Track:` and take-status lines (10 Hz).
- **Drag MIDI out** pad — drag off it to drop `<trackname>.mid` onto a Bitwig track
  (`performExternalDragDropOfFiles`; the editor is the `DragAndDropContainer`). Disabled while
  playing or with an empty take. Proven pattern — MC's Score View does the same in Bitwig.
- **Save .mid to folder…** — `FileChooser`, default name `<trackname>.mid`.
- **Clear Take** — discard and listen fresh.

---

## 4. Phase 1 risks to verify live (Bitwig)

1. `updateTrackProperties` actually delivers the track name in Bitwig (high confidence — name/colour
   is standard VST3 channel context — but check first).
2. Drag-out from the plugin editor in Bitwig (MC's Score View proves it — expected to work).
3. Per-sample ppq interpolation lines up with the grid closely enough for a clean Dorico import
   (as-performed timing; a quantize toggle is the Phase 3 backstop).

---

## 5. Build

```
cmake -S . -B build
cmake --build build --config Release --target OrchCapture_VST3
cmake --build build --config Release --target OrchCaptureTakeLogicCheck
```

`COPY_PLUGIN_AFTER_BUILD` is **FALSE** (matches OrchNoteFilter — the post-build copy into
`C:\Program Files\Common Files\VST3\` needs an elevated shell). Copy the built
`OrchCapture.vst3` there by hand, or flip the flag once elevated.
