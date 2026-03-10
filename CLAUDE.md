# CLAUDE.md

## Project Overview

Sample Robot tool module for Move Anything. Autosamples external MIDI gear to create SFZ instruments.

## Build Commands

```bash
./scripts/build.sh      # Build with Docker (cross-compiles plugin)
./scripts/install.sh    # Deploy to Move
```

## Structure

```
src/
  module.json           # Module metadata
  ui.js                 # JavaScript UI
  help.json             # On-device help
  dsp/
    samplerobot_plugin.c  # Main plugin - state machine, capture, MIDI
    wav_writer.c/.h       # WAV file writing with loop points
    silence_detect.c/.h   # RMS-based silence detection
    sfz_writer.c/.h       # SFZ file generation
    loop_finder.c/.h      # Spectral+correlation loop detection
    third_party/          # KissFFT (BSD license)
```

## How It Works

1. Sends MIDI notes out USB to external synth
2. Records audio from Move's line-in
3. Trims silence, finds loop points
4. Writes WAV samples + .sfz file to SFZ Player's instruments folder
