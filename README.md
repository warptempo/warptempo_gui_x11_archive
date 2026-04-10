Tempo-warping scripts.

Requirements:
```
bc ffmpeg ffprobe jq sox yt-dlp
```

To use, first clone this repository:
```
git clone https://github.com/warptempo/scripts.git
cd scripts
```

To compile the `warptempo` engine and parser, install `cmake`, `libsndfile`, and `gcc` or `clang`. Then install the required libraries for your OS:
```
sudo pacman -S cmake libsndfile libebur128 fftw pkgconf          # Arch Linux
sudo apt install cmake libsndfile1-dev libebur128-dev libfftw3-dev libomp-dev pkg-config  # Debian / WSL
brew install cmake libsndfile libebur128 fftw libomp pkg-config  # macOS
```

Once dependencies are installed, build the binaries:
```
cmake -B build -S .
cmake --build build -j$(nproc)
```

Then run `warptempo` within one of the example projects.

Optionally, the `rubberband`, `bungee`, `stretch` or `soundtouch` adapters can be used instead for rendering audio output. You can also use engine=midi to produce a MIDI file which can be opened in a DAW.

Below are instructions for compiling other engines.

For `midi`, run:
```
cd scripts/adapters/midi
git clone https://github.com/craigsapp/midifile.git
cmake -B build -S . && cmake --build build -j$(nproc)
```

For `bungee`, run:
```
cd scripts/adapters/bungee
git clone --recurse-submodules https://github.com/bungee-audio-stretch/bungee.git
cmake -B build -S . && cmake --build build -j$(nproc)
```

For `stretch`, run:
```
cd scripts/adapters/stretch
git clone https://github.com/Signalsmith-Audio/signalsmith-stretch.git
git clone https://github.com/Signalsmith-Audio/linear.git signalsmith-linear
cmake -B build -S . && cmake --build build -j$(nproc)
```

For `soundtouch`, install the library for your OS:
```
sudo pacman -S soundtouch fftw      # Arch Linux
sudo apt install libsoundtouch-dev  # Debian / WSL
brew install sound-touch            # macOS
```

Then compile:
```
cd scripts/adapters/soundtouch
cmake -B build -S . && cmake --build build -j$(nproc)
```

Once compiled, set `engine=midi`, `engine=bungee`, `engine=stretch`, or `engine=soundtouch` in the `.settings` file.
