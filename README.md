Tempo-warping scripts.

Requirements:
```
bc ffmpeg ffprobe jq sox yt-dlp
```

Additionally, `rubberband`, `bungee`, `stretch` or `soundtouch` are required for rendering output, or use engine=ableton to produce an Ableton Live Set. The default is engine=rubberband. 

To use the script, clone this repository and run `warptempo` within one of the example projects. 

To compile an adapter, install `cmake`, `libsndfile`, and `gcc` or `clang`. 

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

Once compiled, you can use engine=bungee, engine=stretch or engine=soundtouch in your .settings file.

For limiting, use the script's default limiter (ffmpeg alimiter) by setting limiter=true in .settings or leave limiter=false and use an external limiter. Recommended settings for FabFilter Pro-L2: disable true peak limiting and oversampling, gain +0.1dB, style modern, lookahead 0.1ms, attack 250ms, release 50ms, stereo link transients 10%, output level -0.1dB (Pro-L2 defines these terms differently than alimiter - see the [manual](https://www.fabfilter.com/downloads/pdf/help/ffprol2-manual.pdf)). A VST preset exported from REAPER is provided in the presets folder.

Stream / download examples:<br/>
[YouTube](https://www.youtube.com/playlist?list=PLm5sJJQZOLT2bLORBHd-lBtpx1PK_mxFl)  
[Google Drive](https://drive.google.com/drive/folders/1fIU1slnUX0zCRqBXFKTNfuvxz85QFdZw?usp=drive_link)

