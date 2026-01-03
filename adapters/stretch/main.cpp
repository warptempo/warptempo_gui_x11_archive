#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <iomanip>

// Requires libsndfile
#include <sndfile.hh> 
#include "signalsmith-stretch.h"

// Convenience struct for our map points
struct TimemapPoint {
    long long source;
    long long target;
};

// Helper to get planar audio (Vector of Vectors) from interleaved buffer
std::vector<std::vector<float>> interleavedToPlanar(const std::vector<float>& interleaved, int channels, size_t frames) {
    std::vector<std::vector<float>> planar(channels, std::vector<float>(frames));
    for (size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) {
            planar[c][i] = interleaved[i * channels + c];
        }
    }
    return planar;
}

// Helper to flatten planar audio back to interleaved for writing
std::vector<float> planarToInterleaved(const std::vector<std::vector<float>>& planar, int channels, size_t frames) {
    std::vector<float> interleaved(frames * channels);
    for (size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) {
            interleaved[i * channels + c] = planar[c][i];
        }
    }
    return interleaved;
}

int main(int argc, char* argv[]) {
    // --- Check for Version Flag ---
    if (argc > 1 && std::strcmp(argv[1], "-v") == 0) {
        auto v = signalsmith::stretch::SignalsmithStretch<float>::version;
        std::cout << "signalsmith-stretch v" << v[0] << "." << v[1] << "." << v[2] << std::endl;
        return 0;
    }

    // 1. Argument Validation
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <input.wav> <timemap.txt> <output.wav>" << std::endl;
        std::cerr << "       " << argv[0] << " -v (to see version)" << std::endl;
        return 1;
    }
    std::string inputPath = argv[1];
    std::string timemapPath = argv[2];
    std::string outputPath = argv[3];

    // 2. Load Input Audio
    SndfileHandle inputFile(inputPath);
    if (inputFile.error()) {
        std::cerr << "Error opening input: " << inputFile.strError() << std::endl;
        return 1;
    }

    size_t inputFrames = inputFile.frames();
    int channels = inputFile.channels();
    int sampleRate = inputFile.samplerate();

    std::cout << "Loading audio (" << inputFrames << " frames)..." << std::endl;
    std::vector<float> inputBuffer(inputFrames * channels);
    inputFile.read(inputBuffer.data(), inputFrames * channels);

    auto inputPlanar = interleavedToPlanar(inputBuffer, channels, inputFrames);

    // 3. Load Timemap
    std::ifstream timemapFile(timemapPath);
    std::vector<TimemapPoint> mapPoints;
    std::string line;
    while (std::getline(timemapFile, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        long long src, tgt;
        if (ss >> src >> tgt) {
            mapPoints.push_back({src, tgt});
        }
    }

    if (mapPoints.empty()) {
        std::cerr << "Error: Timemap is empty." << std::endl;
        return 1;
    }

    if (mapPoints[0].source != 0 || mapPoints[0].target != 0) {
        mapPoints.insert(mapPoints.begin(), {0, 0});
    }

    // 4. Configure Stretch Engine
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.presetDefault(channels, sampleRate);
    
    std::vector<std::vector<float>> outputPlanar(channels);

    // Helper lambda to process a segment
    auto processSegment = [&](long long startSrc, long long endSrc, long long startTgt, long long endTgt) {
        long long inputLen = endSrc - startSrc;
        long long outputLen = endTgt - startTgt;

        if (inputLen <= 0 || outputLen <= 0) return;
        if (startSrc >= (long long)inputFrames) return; 

        if (endSrc > (long long)inputFrames) {
            endSrc = inputFrames;
            inputLen = endSrc - startSrc;
        }

        std::vector<const float*> inputChPtrs(channels);
        for (int c = 0; c < channels; ++c) {
            inputChPtrs[c] = &inputPlanar[c][startSrc];
        }

        std::vector<std::vector<float>> segmentOutput(channels, std::vector<float>(outputLen));
        std::vector<float*> outputChPtrs(channels);
        for (int c = 0; c < channels; ++c) {
            outputChPtrs[c] = segmentOutput[c].data();
        }

        stretch.process(inputChPtrs, inputLen, outputChPtrs, outputLen);

        for (int c = 0; c < channels; ++c) {
            outputPlanar[c].insert(outputPlanar[c].end(), segmentOutput[c].begin(), segmentOutput[c].end());
        }
    };

    // 5. Execution Loop
    long long currentSrc = 0;
    long long currentTgt = 0;

    for (const auto& point : mapPoints) {
        if (point.source > currentSrc) {
            processSegment(currentSrc, point.source, currentTgt, point.target);
            currentSrc = point.source;
            currentTgt = point.target;

            int percent = (int)((currentSrc * 100.0) / inputFrames);
            std::cout << "\rProcessing: " << percent << "%" << std::flush;
        } else {
            currentTgt = std::max(currentTgt, point.target);
        }
    }

    if (currentSrc < (long long)inputFrames) {
        long long remaining = inputFrames - currentSrc;
        processSegment(currentSrc, inputFrames, currentTgt, currentTgt + remaining);
    }

    std::cout << "\rProcessing: 100%" << std::endl;

    // =========================================================
    // --- NEW: FLUSH THE BUFFER (Recover the Tail) ---
    // =========================================================
    // The STFT engine holds audio in its buffer. We must push it out.
    // We flush the same amount we intend to trim from the start (4096)
    // so the final duration remains mathematically accurate.
    
    int latencyFrames = 4096; // Hardcoded Power-of-2 (85.33ms)
    
    std::cout << "Flushing internal buffer (" << latencyFrames << " frames)..." << std::endl;

    // Prepare flush buffers
    std::vector<std::vector<float>> flushOutput(channels, std::vector<float>(latencyFrames));
    std::vector<float*> flushChPtrs(channels);
    for (int c = 0; c < channels; ++c) flushChPtrs[c] = flushOutput[c].data();

    // Tell library to generate output with no new input
    stretch.flush(flushChPtrs, latencyFrames);

    // Append flushed audio to our main planar output
    for (int c = 0; c < channels; ++c) {
        outputPlanar[c].insert(outputPlanar[c].end(), flushOutput[c].begin(), flushOutput[c].end());
    }
    // =========================================================

    // Trimming Latency
    std::cout << "Trimming start latency: " << latencyFrames << " frames..." << std::endl;

    for (int c = 0; c < channels; ++c) {
        if (outputPlanar[c].size() > (size_t)latencyFrames) {
            outputPlanar[c].erase(outputPlanar[c].begin(), outputPlanar[c].begin() + latencyFrames);
        } else {
            outputPlanar[c].clear();
        }
    }

    // 6. Write Output
    if (outputPlanar[0].empty()) {
        std::cerr << "Error: No output generated (or file was shorter than latency)." << std::endl;
        return 1;
    }

    std::cout << "Writing output (" << outputPlanar[0].size() << " frames)..." << std::endl;
    
    // Force 32-bit Floating Point Output
    SndfileHandle outputFile(outputPath, SFM_WRITE, SF_FORMAT_WAV | SF_FORMAT_FLOAT, channels, sampleRate);
    
    auto outputInterleaved = planarToInterleaved(outputPlanar, channels, outputPlanar[0].size());
    outputFile.write(outputInterleaved.data(), outputInterleaved.size());

    std::cout << "Done." << std::endl;

    return 0;
}

