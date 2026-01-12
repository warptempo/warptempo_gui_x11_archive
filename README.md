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

For best timing accuracy, engine=ableton is recommended. Once rendered in Ableton, the output can be mastered in any DAW or CLI tool. To improve sound quality, `rms_splitter` is provided - pass the Ableton output and use the two split bands (quiet and loud) to apply separate mastering profiles (requires `libsndfile`).

Stream / download examples:<br/>
[YouTube](https://www.youtube.com/playlist?list=PLm5sJJQZOLT2bLORBHd-lBtpx1PK_mxFl)  
[Google Drive](https://drive.google.com/drive/folders/1fIU1slnUX0zCRqBXFKTNfuvxz85QFdZw?usp=drive_link)

