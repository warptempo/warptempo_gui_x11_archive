Tempo-warping scripts.

Requirements:
```
bc ffmpeg ffprobe jq sox yt-dlp
```

Additionally, `rubberband`, `bungee`, `stretch` or `soundtouch` are required for rendering audio output, or use engine=midi to produce a MIDI file which can be opened in a DAW. The default is engine=rubberband. 

To use the script, clone this repository and run `warptempo` within one of the example projects. 

To compile an adapter, install `cmake`, `libsndfile`, and `gcc` or `clang`. 

For `midi`, run:
```
cd scripts/adapters/midi
git clone https://github.com/craigsapp/midifile.git
```

For `bungee`, run:
```
cd scripts/adapters/bungee
git clone --recurse-submodules https://github.com/bungee-audio-stretch/bungee.git
```

For `stretch`, run:
```
cd scripts/adapters/stretch
git clone https://github.com/Signalsmith-Audio/signalsmith-stretch.git
git clone https://github.com/Signalsmith-Audio/linear.git signalsmith-linear
```

For `soundtouch`, run one of the following:
```
cd scripts/adapters/soundtouch
sudo pacman -S soundtouch fftw # Arch Linux
sudo apt install libsoundtouch-dev libfftw3-dev # Debian / WSL
brew install sound-touch fftw # MacOS
```

Then compile:
```
mkdir build && cd build
cmake ..
make
```

Once compiled, you can use engine=midi, engine=bungee, engine=stretch or engine=soundtouch in the .settings file.

The recommended workflow is to test in one of the audio output engines using limiter=true, then use MIDI for the final render. Ableton Live and Serato Sample are recommended for use with MIDI - in Sample, click "Grid", then manually set the tempo for the file to the base bpm given by midi_adapter via warptempo, and clear all markers in Sample and set one marker at the start of the file. Then import the MIDI clip onto Ableton Live and render. 

Once an audio output has been rendered, it is recommended to use the included `eqmatch` algorithm to generate a linear phase, impulse response-based EQ curve that approximates the spectral characteristics of the original in relation to the output. The algorithm also applies that curve to the output and trims initial latency caused by the convolution as well as the additional end silence generated as a byproduct of the MIDI rendering workflow.

For `eqmatch`, run one of the following:
```
sudo pacman -S libsndfile libebur128 fftw pkgconf # Arch Linux
sudo apt install libsndfile1-dev libebur128-dev libfftw3-dev pkg-config # Debian / WSL
brew install libsndfile libebur128 fftw pkg-config # MacOS
```

Then compile.

The REAPER mastering project is included, and uses FabFilter's Pro-L 2 plugin. 

Stream / download examples:<br/>
[YouTube](https://www.youtube.com/playlist?list=PLm5sJJQZOLT2bLORBHd-lBtpx1PK_mxFl)  
[Google Drive](https://drive.google.com/drive/folders/1fIU1slnUX0zCRqBXFKTNfuvxz85QFdZw?usp=drive_link)

