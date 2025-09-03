To use the script, run:
```
git clone https://github.com/warptempo/scripts
cd scripts/examples/"Mozart - Symphony No. 40 in G Minor (K. 550) - I. Molto Allegro (2024 Remaster, Tempo Warp)"
../../warptempo
```

Requirements:
```
bc ffmpeg ffprobe jq metaflac rubberband sox yt-dlp
```

For limiting, use the script's default limiter (ffmpeg alimiter) or set limit_final_output=false and use an external limiter. Recommended settings for FabFilter Pro-L v1: gain +0.1dB, style dynamic, lookahead 0.1ms, attack 250ms, release 50ms, stereo link transients 10%, output level -0.1dB (Pro-L defines these terms differently than alimiter - see the [manual](https://www.fabfilter.com/downloads/pdf/help/ffprol-manual.pdf)).  

Stream / download examples:  
https://www.youtube.com/watch?v=gzPq_t0eecY&list=PLm5sJJQZOLT2bLORBHd-lBtpx1PK_mxFl  
https://drive.google.com/drive/folders/1fIU1slnUX0zCRqBXFKTNfuvxz85QFdZw?usp=drive_link

