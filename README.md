# AutoSample Module

Autosample external MIDI gear to create SFZ instruments for [Move Everything](https://github.com/charlesvestal/move-anything).

Connect a synthesizer via USB MIDI and line-in audio, configure the sampling range, and AutoSample records every note and velocity layer automatically — producing a playable SFZ instrument with loop points.

## Features

- Configurable note range, key zones, and velocity layers
- Automatic MIDI note sending and audio capture
- Spectral loop point detection with zero-crossing refinement
- Silence-based release tail trimming
- Velocity crossfade regions in generated SFZ
- On-screen text entry for instrument naming
- Full screen reader accessibility
- Hold Back to cancel mid-session (partial instruments are kept)
- Output instruments appear directly in SFZ Player

## Prerequisites

- [Move Everything](https://github.com/charlesvestal/move-anything) installed on your Ableton Move
- SSH access enabled: http://move.local/development/ssh
- External synth connected via USB MIDI out and line-in audio
- Audio input set to line-in

## Installation

### Via Module Store (Recommended)

1. Launch Move Everything on your Move
2. Select **Module Store** from the main menu
3. Navigate to **Tools** → **AutoSample**
4. Select **Install**

### Manual Installation

```bash
./scripts/build.sh
./scripts/install.sh
```

## Usage

1. Connect your synth (USB MIDI out + line-in)
2. Set audio input to line-in on your Move
3. Open AutoSample from the Tools menu
4. Configure: note range, zones, velocity layers, hold duration
5. Select **Name & Sample** (or press Rec)
6. Type an instrument name and confirm
7. Wait for sampling to complete
8. Play the instrument in SFZ Player

## Third-Party Libraries

- [KissFFT](https://github.com/mborgerding/kissfft) (BSD-3-Clause) — FFT for spectral loop detection

## License

MIT — see [LICENSE](LICENSE)

## AI Assistance Disclaimer

This module is part of Move Everything and was developed with AI assistance, including Claude, Codex, and other AI assistants.

All architecture, implementation, and release decisions are reviewed by human maintainers.
AI-assisted content may still contain errors, so please validate functionality, security, and license compatibility before production use.
