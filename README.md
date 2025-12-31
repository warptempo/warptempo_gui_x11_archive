Tempo-warping scripts.

Requirements:
```
bc ffmpeg ffprobe jq sox yt-dlp
```

Additionally, `stretch`, `bungee` or `rubberband` are required.

To use the script, clone this repository and run `warptempo` within one of the example projects. Set engine=rubberband in the project's .settings file to use rubberband without needing to compile either `warptempo_stretchadapter` or `warptempo_bungeeadapter`.

To compile an adapter, install `cmake`, `libsndfile`, and `gcc` or `clang`. For `warptempo_stretchadapter`, run:
```
cd scripts/warptempo_stretchadapter
git clone https://github.com/Signalsmith-Audio/signalsmith-stretch.git
git clone https://github.com/Signalsmith-Audio/linear.git signalsmith-linear
```

Or, for `warptempo_bungeeadapter`, run:
```
cd scripts/warptempo_bungeeadapter
git clone --recurse-submodules https://github.com/bungee-audio-stretch/bungee.git
```

Then compile:
```
mkdir build && cd build
cmake ..
make
```

Once compiled, you can use engine=stretch or engine=bungee in your .settings file, or use engine=bungee+stretch to render using both engines. If the limiter is enabled, the bungee version will be used for frequencies above the crossover point, and the stretch version will be used for frequencies below the crossover. The stretch adapter is optimized for low frequencies; reduce the windowSize (eg, to 4096) and the overlapFactor (eg, to 4) for general purpose use.

For limiting, use the script's default limiter (ffmpeg alimiter) by setting limiter=true in .settings or leave limiter=false and use an external limiter. Recommended settings for FabFilter Pro-L2: disable true peak limiting and oversampling, gain +0.1dB, style modern, lookahead 0.1ms, attack 250ms, release 50ms, stereo link transients 10%, output level -0.1dB (Pro-L2 defines these terms differently than alimiter - see the [manual](https://www.fabfilter.com/downloads/pdf/help/ffprol2-manual.pdf)). A VST preset exported from REAPER is provided in the warptempo_vstpresets folder.

For crossover when alimiter is disabled, use a true Linkwitz-Reilly filter such as Crossover Stereo x8 in `lsp-plugins-vst3`. A VST preset is also provided. 

Stream / download examples:<br/>
[YouTube](https://www.youtube.com/playlist?list=PLm5sJJQZOLT2bLORBHd-lBtpx1PK_mxFl)  
[Google Drive](https://drive.google.com/drive/folders/1fIU1slnUX0zCRqBXFKTNfuvxz85QFdZw?usp=drive_link)

