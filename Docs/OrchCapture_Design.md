# OrchCapture — Design

Date: 2026-08-31
Status: Phases 1, 1b, 2 built & live-tested (merged export → Dorico, zero cleanup). Phase 3 (polish)
built, not yet live-tested.
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
| **1b — Two-tap** | `tapRole` (Performance / Articulation), `ksZoneMin/Max` (default 12–23), `ksExportMode` (Inline / Separate Track / Exclude / KS Only). `CapturedNote.isKeyswitch` tagged at capture; `writeTakeMidi` lays out one or two note tracks. Same binary in both roles. See §5. | **built + live-tested 2026-08-30** |
| **2 — Coordinator** | `coordinator` param; the on instance binds `InterprocessConnectionServer` on port **47826**, every other instance auto-connects as a client and pushes its lane (name, role, completed take). Coordinator editor gains a lane list + merged export (drag-out / save) — one SMF with matched `<name>` / `<name> KS` track pairs. See §6. | **built + live-tested 2026-08-31** (froze first, fixed — see §6) |
| **3 — Polish** | `quantizeGrid` per-instance notation quantize; coordinator `mergedContent` (Notes + KS / Notes only / KS only); coordinator section-marker + tempo-mark + score-order free-text fields (persisted, hand-entered, 1-indexed bars); click a lane to exclude it; **auto-save the merged `.mid` to a folder on transport stop**. See §7. | **built 2026-08-31; markers/tempo/order live-verified in Dorico, auto-save not yet tested** |
| **later** | Auto-capturing Bitwig's tempo automation as a tempo map (no VST3 API for host markers/tempo — would have to come from MC). | deferred |

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
| `coordinator` | Coordinator | off | On for exactly one instance in the rig — it becomes the export hub (binds port 47826, collects every other instance's take, editor drag-out/save produce the merged rig SMF). See §6. |
| `quantizeGrid` | Quantize | Off | Off / 1/4 / 1/8 / 1/16 / 1/8T / 1/16T / 1/32. Snaps this instance's export onsets **and** releases to the grid (min one grid unit long). As-performed by default. Each lane's setting is honoured in the merged export. |
| `mergedContent` | Merged Content | Notes + KS | Coordinator only. Notes + KS / Notes only / KS only — filters `<name>` vs `<name> KS` tracks out of the merged file. |
| `autoSaveOnStop` | Auto-save on stop | off | Coordinator only. When on (and a folder is set), the Link writes `OrchCapture_session_<timestamp>.mid` to the auto-save folder ~7 s after the transport stops — hands-free capture runs. Ignores per-lane exclude (always the full rig). |

State (`getStateInformation`) persists parameters, plus four coordinator strings — `markersText`
(`"bar:label, …"` → `textMetaEvent(6)` markers), `tempoText` (`"bar:bpm, …"` → tempo meta events),
`scoreOrderText` (comma/newline instrument names → merged track order; unlisted names fall after in
first-seen order), and `autoSaveFolder`. Marker/tempo/order fields are 1-indexed by bar and
hand-entered — **not** read from Bitwig's arrangement (no VST3 API). Each is mirrored into a
`metaTextLock`-guarded member (the Link worker thread reads them for auto-save) and an APVTS-state
ValueTree property (persistence); message thread writes, either thread reads. The take itself is **ephemeral** — like MC's Score View buffer, it is not saved
with the project. Per-lane exclude (click a lane row on the coordinator) is editor-only, not persisted.

Note timing is captured as musical ppq straight off the playhead, so note *positions* are already
correct across Bitwig tempo automation — `tempoText` only controls the tempo *marks* Dorico shows.

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
- **Track 0** — track-name text meta + tempo meta + live-captured time-signature meta events (see
  below; empty `options.timeSigChanges` writes none at all, no fabricated 4/4).
- **Track 1 (..2)** — one or two note tracks, per `options.keyswitchMode` (see §5).

### Live time-signature capture (2026-09-06)

**Problem**: OrchCapture was measure-agnostic — every exported take showed 4/4 in Dorico regardless
of the real meter in Bitwig, because nothing captured or wrote a time signature at all.

**Fix**: `AudioPlayHead::PositionInfo::getTimeSignature()` (JUCE 9, `juce_audio_basics`) does report
the host's current numerator/denominator — Bitwig's own arrangement time-signature track reaches the
plugin through it. It's pull-only (no change notification), so `processBlock` polls it every block
(same place `blockStartPpq` is read) and diffs against the last-seen value, appending an
`ocap::TimeSigMark { ppq, numerator, denominator }` (ppq relative to the take's own start, same
convention as `CapturedNote` - NOT a bar number, which would need the meter to convert, circularly)
to `capturedTimeSigChanges` on any change. `lastTimeSigNumerator/Denominator` reset to `0` in
`resetTake()` so every take's *first* observation always registers as a change too, anchoring the
starting meter at ppq 0 - not just later mid-take changes.

`snapshotTake()` has a `snapshotTimeSigChanges()` sibling (same `captureLock` pattern) for the editor
to copy the list out before export; both `writeTakeToTempFile()` (drag-out) and the Save-As handler
set `options.timeSigChanges` from it before calling `writeTakeMidi`. `tempoMetaTrack` (shared by both
`writeTakeMidi` and `writeMergedTakeMidi`) writes one `MidiMessage::timeSignatureMetaEvent(num, den)`
per mark at `mark.ppq * tpqn` - a ready-made JUCE primitive, no manual `FF 58` byte-writing needed.

**Scope**: single-take export (`TakeExportOptions`) only, matching the reported problem (a dragged-in
take showing 4/4). `MergedExportOptions`'s own meter is still the hand-set `barLengthPpq` (§ "Section
markers…" above still says "4/4 assumed") - wiring live capture into the merged/coordinator path is a
separate, not-yet-done follow-up, not automatically covered by this fix.

Verified: `OrchCaptureTakeLogicCheck` covers both the "no captured changes → no meta event" case and a
real starting-meter-plus-mid-take-change case (6/8 → 4/4 at beat 24) landing at the correct ticks.

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

## 6. Coordinator (Phase 2)

`OrchCaptureLink` (one per instance) turns the rig's ~50–100 instances into a single export action.

**All socket work is on `OrchCaptureLink`'s own background `juce::Thread` — never the host message
thread.** A rig has dozens of instances; a blocking connect on each, on the one shared message
thread, freezes the host (it did, 2026-08-30, when the link was a `juce::Timer`).

### Discovery — a lock file

The coordinator writes `<temp>/orchcapture-coordinator.lock` while it holds port 47826. A client
only calls `connectToSocket` when that file exists, so the common "no coordinator on the rig" state
costs **one `File::existsAsFile()` per 3 s poll** and no socket work at all.

### Roles

- `coordinator` param **off** → **client**: the worker thread, when the lock file exists, keeps a
  `juce::InterprocessConnection` (callbacks off the message thread) connected to `127.0.0.1:47826`
  and pushes a `lane` message (`uid`, track name, `tapRole`, tempo, `ksExportMode`, and the full
  note list as compact JSON) on connect and on every completed take (transport-stop edge /
  `takeGeneration` change). Between takes it sends a lightweight `status` (counts + playing).
- `coordinator` param **on** → binds port 47826 as an `InterprocessConnectionServer`, writes the
  lock file, and collects every client's lane into a `uid → Lane` map. Port already held →
  `CoordinatorPortBusy`, keeps retrying so it can take over if that instance leaves.

`InterprocessConnection(false)` — every connection callback (including the multi-thousand-note JSON
parse) is on the connection's own thread; `lanes` / `serverConnections` are mutex-guarded. Same
local-socket pattern as MC's `PatternSyncServer` / `McpBridgeServer` and the Transport Companion,
minus the message-thread timer.

### Merged export

The coordinator editor shows a **lane list** (its own lane first, then every client in first-seen
order: `Vln I · Perf · 43n · 15.2b`, `Vln I · Artic · 22ks`). The **drag pad** and **Save** button
switch to the merged file when this instance is the active coordinator:

`writeMergedTakeMidi` → one Format-1 SMF, 960 tpqn: track 0 tempo/name, then every lane's note
tracks (`planNoteTracks` per lane, honouring each lane's own `ksExportMode`), grouped by track name
in first-seen order, Performance (`tapRole` 0) before Articulation (`tapRole` 1) within a group. So
Dorico / the music21 script get `Violin I`, `Violin I KS`, `Viola`, `Viola KS`, … already paired.

### Phase 2 status

Built `a32f007`. **First live test froze Bitwig** — the link was a `juce::Timer`, so every
instance's blocking `connectToSocket` ran on the shared host message thread; with ~50–100 instances
and no coordinator, that starved the message thread. Fixed `9cb45e9`: link moved to its own
`juce::Thread`, lock-file discovery, callbacks off the message thread. **Not yet re-tested live.**

### Phase 2 risks still to verify (Bitwig)

1. Localhost `InterprocessConnectionServer` across sandboxed plugin instances (PatternSync /
   McpBridge / Transport Companion all prove this works — expected fine).
2. ~50–100 clients on one server; JSON take payloads (a few thousand notes each) on connect / take-end.
3. No message-thread stall at the full instance count (the whole point of the `9cb45e9` rework —
   confirm it holds).

### Phase 2 outcome

Live-tested 2026-08-31: ~59 lanes auto-connected, merged export imported into Dorico with **zero
MIDI cleanup**, paired `<name>` / `<name> KS` tracks, verified on an MPL-fed instrument (Oboe 1) and
a pitch-randomized-clip instrument (Horn 1). The first attempt froze the host — root cause and fix
above.

---

## 7. Polish (Phase 3)

All pure logic is in `OrchCaptureTakeLogic` and covered by `OrchCaptureTakeLogicCheck`.

### Quantize for notation — `quantizeGrid` (per instance)

`ocap::quantizeTake(notes, gridPpq)` snaps every onset and release to the nearest multiple of the
grid, keeping a minimum length of one grid unit. `planNoteTracks` applies it before the KS split, so
`writeTakeMidi` and `writeMergedTakeMidi` both get it. Grid ppq: 1/4 = 1.0, 1/8 = 0.5, 1/16 = 0.25,
1/32 = 0.125, 1/8T = 1/3, 1/16T = 1/6. The client pushes its grid (`qgrid`) with the lane, so the
coordinator quantizes each lane by that lane's own setting.

Capture stays as-performed; this only affects the export. The KS taps are usually left `Off` (the
half-bar keyswitch jitter is the "alive" mechanism, and music21 can quantize downstream).

### Merged content — `mergedContent` (coordinator)

`ocap::MergedContent` { NotesAndKeyswitches, NotesOnly, KeyswitchesOnly }. `writeMergedTakeMidi`
classifies each planned track by whether its name ends `" KS"` and drops the others. Notes only =
a straight-to-Dorico file; KS only = feed for the music21 articulation pass.

### Section markers + tempo marks + score order (coordinator, persisted free text)

- **`markersText`** — `"bar:label"` tokens, comma / newline / semicolon separated
  (`ocap::parseSectionMarkers`). Bar is **1-indexed** (bar 1 == the take's start), to match Bitwig /
  Dorico. Each becomes a `textMetaEvent(6, label)` on the merged tempo track at
  `(bar - 1) * barLengthPpq` (4/4 assumed). Malformed tokens are skipped. **Not** read from Bitwig's
  arrangement markers — no VST3 API for that.
- **OrchHarp pedal markers** — `buildMergedExportOptions` also scans `%TEMP%` for
  `orchharp-pedals-*.txt` files modified in the last 120 s (OrchHarp writes one per instance on
  transport stop, same `bar:label` format) and folds their lines into `options.markers`, skipping
  entries that duplicate an existing `(bar, label)`. So a harp's pedal changes reach the score
  without Dorico's semi-automatic Calculate Harp Pedals. No config — it just picks up the file.
- **`tempoText`** — `"bar:bpm"` tokens (`ocap::parseTempoMarks`), same 1-indexed bars. Each becomes a
  tempo meta event; an anchor tempo at tick 0 is always present. Empty → the single `tempoBpm`.
  Note positions don't need this (they're musical ppq); it only sets the score's tempo marks.
- **`scoreOrderText`** — comma / newline separated instrument track names
  (`ocap::parseScoreOrder`). `writeMergedTakeMidi` orders instruments by their index in this list;
  names not listed fall after, in first-seen order; Performance precedes Articulation within an
  instrument.

Both are stored as ValueTree properties on the APVTS state (ride `getStateInformation`), edited in
two `TextEditor`s on the coordinator, committed on focus-loss.

### Per-lane exclude (coordinator, editor-only)

Click a lane row to toggle it out of the merged export (struck through, "— excluded"). Keyed by the
lane's `uid` (`"local"` for the coordinator's own lane). Passed to
`OrchCaptureLink::collectTakesForExport(excludedUids)`; the editor's `collectFilteredTakes()`
wraps it. Not persisted.

### Auto-save on stop (coordinator)

`OrchCaptureLink::serviceAutoSave()` runs each worker-loop tick in coordinator mode. It tracks a
"playing" state = the coordinator's own `transportPlayingUi` **OR** any connected lane's reported
`playing` (so it works even if the coordinator instance's track isn't being processed by the host).
On the playing→stopped edge, with `autoSaveOnStop` on, it arms a timer for
`kAutoSaveSettleMs` (7 s) — long enough for every client's 3 s poll to have pushed its final take.
When the timer fires it writes `OrchCapture_session_<yyyymmdd_hhmmss>.mid` (full merged export, no
per-lane exclude) to `autoSaveFolder` and calls `processor.noteAutoSave()`, which the editor shows.
A transport restart before the timer fires cancels the pending save. All file I/O is on the worker
thread; the editor need not be open. The Transport Companion already stops Bitwig at blueprint end,
so in the real rig "stopped" ≈ "blueprint finished".

---

## 8. Build

```
cmake -S . -B build
cmake --build build --config Release --target OrchCapture_VST3
cmake --build build --config Release --target OrchCaptureTakeLogicCheck
```

`COPY_PLUGIN_AFTER_BUILD` is **FALSE** (matches OrchNoteFilter — the post-build copy into
`C:\Program Files\Common Files\VST3\` needs an elevated shell). Copy the built
`OrchCapture.vst3` there by hand, or flip the flag once elevated.
