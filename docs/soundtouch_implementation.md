# SoundTouch Implementation & Audio Analysis

For the `soundtouch` adapter, install the following, in addition to the packages listed in README.md:

Arch Linux: `sudo pacman -S soundtouch fftw`
Debian / WSL: `sudo apt install libsoundtouch-dev libfftw3-dev`
Mac: `brew install sound-touch fftw`

Although its WSOLA algorithm is not viable for some uses, it does maintain many spectral characteristics of the original that phase vocoder-based algorithms do not, and so it can be useful as a reference for mastering or A / B testing.

Briefly, the soundtouch adapter analyzes the timemap and input audio to categorize sections as either Percussive (transient-heavy) or Tonal (transient-light). It then applies hiflux_seq, hiflux_seek, hiflux_ovl to the Percussive parts, and loflux_seq, loflux_seek and loflux_ovl to the Tonal parts. See the [manual](https://www.surina.net/soundtouch/README.html).

If the .settings file does not define these values, it will use 0 (automatic) for both sequence and seek and 8 for overlap. The automatic values are calculated based on the tempo of the segment. The analyzer will render .timemap-analyzed, which can be helpful in fine-tuning. The columns are: source frame, target frame, seq, seek, ovl, category, flux (200 is the boundary), and the estimated values that soundtouch would use for seq and seek if auto is enabled, as well as the tempo.

In general, the hiflux_seq is the only one that needs to be changed to emphasize transients in Percussive parts. Experiment with 24, 32, etc.

## Overview
We use a customized integration of the SoundTouch library to handle time-stretching. Instead of using static defaults, we analyze audio segments to dynamically adjust WSOLA parameters (`Sequence`, `SeekWindow`, `Overlap`) based on transient flux and tempo changes.

## Analyzer Logic (`analyzer.cpp`)
The analyzer calculates spectral flux to categorize segments as either **Percussive** or **Tonal**.

### 1. Transient Flux Detection
* **FFT Size:** 4096
* **Hop Size:** 1024
* **Threshold:** > 200.0 Flux indicates a high-transient (Percussive) section.

### 2. Adaptive Parameters
We pass two sets of WSOLA parameters to the analyzer:
* **High Flux (Percussive):** typically `24ms` Sequence (faster update), `0ms` Seek (tighter drift control).
* **Low Flux (Tonal):** typically `82ms` Sequence, `14ms` Seek (standard defaults).

### 3. Heuristic Calculation
We replicate SoundTouch's internal logic to estimate optimal window sizes based on the stretch ratio (Tempo):
* **Formula:** Linear interpolation between `90ms` (at 0.5x tempo) and `40ms` (at 2.0x tempo).
* **Purpose:** Ensures window sizes scale inversely with playback speed to preserve transient density.


## Usage
The analyzer output map format:
```text
<start_sample> <end_sample> <seq> <seek> <ovl> # <label> | <Type> | Flux:<val> | ST_Auto(...)
