# Media Interaction Framework

## Playlist System (`src/playlist/`)

### Playlist Engine (`Playlist.h/cpp`)

Manages three sections: **leadIn**, **mainPlaylist**, **leadOut**. Each contains `PlaylistEntryBase`-derived entries. Supports looping, randomization, pause/resume, position seeking, time-based queries, and nested sub-playlists (max 5 levels deep). Status tracked via mutex-protected state machine.

**Playlist States**: `IDLE`, `PLAYING`, `STOPPING_GRACEFULLY`, `STOPPING_GRACEFULLY_AFTER_LOOP`, `STOPPING_NOW`, `PAUSED`.

### Playlist Entry Types (12 types)

| Entry Type | Class | Purpose |
| --- | --- | --- |
| Sequence | `PlaylistEntrySequence` | Play FSEQ files with frame-based timing |
| Media | `PlaylistEntryMedia` | Audio/video playback (random file selection, backend abstraction) |
| Both | `PlaylistEntryBoth` | Synchronized sequence + media playback |
| Command | `PlaylistEntryCommand` | Execute FPP command, check for completion |
| Script | `PlaylistEntryScript` | Run external shell script (blocking/non-blocking) |
| Image | `PlaylistEntryImage` | Display image on overlay model (async load, ImageMagick, caching) |
| Pause | `PlaylistEntryPause` | Timed delay between entries |
| Playlist | `PlaylistEntryPlaylist` | Nested sub-playlist |
| Branch | `PlaylistEntryBranch` | Conditional branching (time/loop/MQTT-based, true/false targets) |
| Remap | `PlaylistEntryRemap` | Channel remapping operations |
| URL | `PlaylistEntryURL` | HTTP GET/POST requests (libcurl, token replacement) |
| Dynamic | `PlaylistEntryDynamic` | Load entries at runtime from command/file/URL/JSON |

> To trigger an overlay/`.eseq` effect from a playlist, use a **Command** entry running the
> `Effect Start` / `Effect Stop` / `Start FSEQ As Effect` commands (`src/commands/EventCommands.cpp`).
> The former standalone `effect` and `plugin` entry types were unfinished 2017 stubs (the `effect`
> one broken, the `plugin` one a do-nothing shell), never reachable from the editor, and were removed.

---

## Command System (`src/commands/`)

### CommandManager (Singleton)

Registration-based dispatch. Commands registered with `addCommand()`, executed via `run(name, args)`. Supports JSON args, HTTP endpoints, MQTT topics. File-watched preset system (`config/commandPresets.json`) with keyword replacement.

**HTTP Endpoints**: `GET/POST /command/{name}`, `GET /commands`, `GET /commandPresets`.
**MQTT Topic**: `/set/command/{command}/{arg1}/{arg2}`.

### Built-in Commands

#### Playlist Commands (`PlaylistCommands.h/cpp`)

Stop Now, Stop Gracefully, Restart/Next/Prev Playlist Item, Start Playlist, Toggle Playlist, Start At Position/Random, Insert Playlist (next/immediate/random), Pause/Resume Playlist.

#### Media Commands (`MediaCommands.h/cpp`)

Set/Adjust/Increase/Decrease Volume, Play Media, Stop Media, Stop All Media, URL Command.

#### Event Commands (`EventCommands.h/cpp`)

Trigger Preset (immediate/future/slot/multiple), Run Script Event, Start/Stop Effect, Start FSEQ As Effect, Stop All Effects, All Lights Off, Switch to Player/Remote Mode.

---

## Media Output System (`src/mediaoutput/`)

### Backends

- **`VLCOutput`** — libvlc backend. Supports MP4/AVI/MOV/MKV/MPG (video) and MP3/OGG/M4A/WAV/FLAC/AAC (audio). Speed adjustment, volume fine-tuning, event hooks.
- **`SDLOutput`** — SDL2 backend. Video display with overlay integration, audio sample extraction for audio-reactive effects.

### Format Support

- **Audio**: mp3, ogg, m4a, m4p, wav, au, wma, flac, aac
- **Video**: mp4, avi, mov, mkv, mpg, mpeg

Volume control via platform-specific mixer (amixer on Linux, CoreAudio on macOS).
