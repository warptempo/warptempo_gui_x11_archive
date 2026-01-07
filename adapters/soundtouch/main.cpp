#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>

#include <sndfile.h>
#include <soundtouch/SoundTouch.h>

using namespace soundtouch;
using namespace std;

const int BUFFER_SIZE = 2048;

struct TimeMapPoint {
    long src_frame;
    long tgt_frame;
};

// Simple loader for strict "Source Target" (float space float) format
vector<TimeMapPoint> loadTimeMap(const string& filename) {
    vector<TimeMapPoint> map;
    ifstream infile(filename);
    if (!infile.is_open()) {
        cerr << "Error: Could not open timemap: " << filename << endl;
        exit(1);
    }
    
    // Implicit start point at 0,0
    map.push_back({0, 0}); 

    double src, tgt;
    // Standard stream extraction: reads two floats, skipping whitespace/newlines
    while (infile >> src >> tgt) {
        map.push_back({(long)src, (long)tgt});
    }
    return map;
}

int main(int argc, char* argv[]) {
    // Usage: adapter <in.wav> <out.wav> <map.timemap>
    if (argc < 4) {
        cerr << "Usage: " << argv[0] << " <in.wav> <out.wav> <map.timemap>" << endl;
        return 1;
    }

    string inputPath = argv[1];
    string outputPath = argv[2];
    string mapPath = argv[3];

    SF_INFO sfInfoInput;
    memset(&sfInfoInput, 0, sizeof(sfInfoInput)); 
    SNDFILE* infile = sf_open(inputPath.c_str(), SFM_READ, &sfInfoInput);
    if (!infile) {
        cerr << "Error opening input file: " << inputPath << endl;
        return 1;
    }

    SF_INFO sfInfoOutput = sfInfoInput;
    // Force 32-bit float output to match input precision
    sfInfoOutput.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    
    SNDFILE* outfile = sf_open(outputPath.c_str(), SFM_WRITE, &sfInfoOutput);
    if (!outfile) return 1;

    SoundTouch soundTouch;
    soundTouch.setSampleRate(sfInfoInput.samplerate);
    soundTouch.setChannels(sfInfoInput.channels);
    
    // Standard Quality Settings
    soundTouch.setSetting(SETTING_USE_AA_FILTER, 1);
    soundTouch.setSetting(SETTING_USE_QUICKSEEK, 0);
    
    // Defaults (confirmed in TDStretch.h) are used automatically:
    // Sequence: 0 (Auto)
    // Seek: 0 (Auto)
    // Overlap: 8 ms

    vector<TimeMapPoint> timeMap = loadTimeMap(mapPath);
    vector<float> inputBuffer(BUFFER_SIZE * sfInfoInput.channels);
    vector<float> outputBuffer(BUFFER_SIZE * sfInfoInput.channels);

    long current_src_pos = 0;
    
    for (size_t i = 1; i < timeMap.size(); ++i) {
        long target_src = timeMap[i].src_frame;
        long target_tgt = timeMap[i].tgt_frame;
        
        long prev_src = timeMap[i-1].src_frame;
        long prev_tgt = timeMap[i-1].tgt_frame;
        
        double src_delta = (double)(target_src - prev_src);
        double tgt_delta = (double)(target_tgt - prev_tgt);
        
        if (tgt_delta <= 0) tgt_delta = 1.0;
        
        soundTouch.setTempo(src_delta / tgt_delta);

        long frames_remaining = target_src - current_src_pos;
        while (frames_remaining > 0) {
            int read_size = (frames_remaining > BUFFER_SIZE) ? BUFFER_SIZE : frames_remaining;
            sf_count_t count = sf_readf_float(infile, inputBuffer.data(), read_size);
            if (count == 0) break;

            soundTouch.putSamples(inputBuffer.data(), count);
            uint nSamples;
            do {
                nSamples = soundTouch.receiveSamples(outputBuffer.data(), BUFFER_SIZE);
                if (nSamples > 0) {
                    sf_writef_float(outfile, outputBuffer.data(), nSamples);
                }
            } while (nSamples != 0);

            frames_remaining -= count;
            current_src_pos += count;
        }
    }

    // Flush remaining samples
    soundTouch.flush();
    uint nSamples;
    do {
        nSamples = soundTouch.receiveSamples(outputBuffer.data(), BUFFER_SIZE);
        if (nSamples > 0) sf_writef_float(outfile, outputBuffer.data(), nSamples);
    } while (nSamples != 0);

    sf_close(infile);
    sf_close(outfile);
    return 0;
}
