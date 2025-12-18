Tempo-warping scripts.

Requirements:
```
bc ffmpeg ffprobe jq sox yt-dlp
```

Additionally, either `rubberband` or `bungee` (via the `warptempo_bungeeadapter`) are required.

To use the script, clone this repository and run `warptempo` within one of the example projects. Set engine=rubberband in the project's .settings file to use rubberband without needing to compile `warptempo_bungeeadapter`.

To compile `warptempo_bungeeadapter`, install `cmake`, `libsndfile`, and `gcc` or `clang`. Then run:
```
cd scripts/warptempo_bungeeadapter
git clone --recurse-submodules https://github.com/bungee-audio-stretch/bungee.git
mkdir build && cd build
cmake ..
make

```

Once compiled, you can use engine=bungee in your .settings file.

For limiting, use the script's default limiter (ffmpeg alimiter) by setting limit_final_output=true in .settings or leave limit_final_output=false and use an external limiter. Recommended settings for FabFilter Pro-L v1: gain +0.1dB, style dynamic, lookahead 0.1ms, attack 250ms, release 50ms, stereo link transients 10%, output level -0.1dB (Pro-L defines these terms differently than alimiter - see the [manual](https://www.fabfilter.com/downloads/pdf/help/ffprol-manual.pdf)).  

Stream / download examples:  
[YouTube](https://www.youtube.com/playlist?list=PLm5sJJQZOLT2bLORBHd-lBtpx1PK_mxFl)  
[Google Drive](https://drive.google.com/drive/folders/1fIU1slnUX0zCRqBXFKTNfuvxz85QFdZw?usp=drive_link)

