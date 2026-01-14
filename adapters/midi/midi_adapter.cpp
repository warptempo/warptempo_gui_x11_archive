// Compile: 
// g++ -o midi_adapter midi_adapter.cpp -I midifile/include midifile/src/MidiFile.cpp midifile/src/MidiEvent.cpp midifile/src/MidiEventList.cpp midifile/src/MidiMessage.cpp midifile/src/Binasc.cpp
#include "MidiFile.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <cmath>
#include <iomanip>

using namespace smf;
using namespace std;

// Structure to hold our parsed points
struct TimePoint {
    double src_frame;
    double tgt_frame;
    string label;
};

int main(int argc, char* argv[]) {
    // Usage: midi_adapter <input_timemap> <output_mid> <sample_rate> [base_bpm] [ppq]
    if (argc < 4) {
        cerr << "Usage: midi_adapter <input.timemap> <output.mid> <sample_rate> [base_bpm] [ppq]" << endl;
        return 1;
    }

    string input_path = argv[1];
    string output_path = argv[2];
    double sample_rate = stod(argv[3]);
    double base_bpm = (argc > 4) ? stod(argv[4]) : 120.0;
    int tpq = (argc > 5) ? stoi(argv[5]) : 30000;

    // 1. Read the Precision Timemap
    // Format: src_frame tgt_frame label [rms]
    vector<TimePoint> points;
    ifstream infile(input_path);
    if (!infile.is_open()) {
        cerr << "Error opening input file." << endl;
        return 1;
    }

    string line;
    while (getline(infile, line)) {
        stringstream ss(line);
        double s, t;
        string l, rms;
        if (ss >> s >> t >> l) {
            points.push_back({s, t, l});
        }
    }
    infile.close();

    if (points.empty()) {
        cerr << "Error: No points found in map." << endl;
        return 1;
    }

    // --- THE FIX: INSERT ZERO POINT IF MISSING ---
    // Rubberband often omits 0->0 because it causes divide-by-zero errors.
    // We strictly assume that if the first point is not 0, the file implicitly starts at 0.
    if (points[0].src_frame > 0.001) {
        cout << "Fixing missing start: Inserting {0, 0} before " << points[0].src_frame << endl;
        // Insert a virtual start point at the beginning of the vector
        points.insert(points.begin(), {0.0, 0.0, "Start_Fix"});
    }
    // ---------------------------------------------

    // 2. Setup MIDI File
    MidiFile midifile;
    midifile.absoluteTicks();
    midifile.setTicksPerQuarterNote(tpq);
    
    // We strictly use Track 0 for Tempo Map
    // No addTrack(1) needed for Type 0 files

    cout << "Processing " << points.size() << " points..." << endl;
    cout << "Base BPM: " << base_bpm << " | PPQ: " << tpq << endl;

    // 3. Calculate Intervals
    // We iterate from 0 to N-1.
    // The tempo set at Point[i] determines the speed to reach Point[i+1].
    
    for (size_t i = 0; i < points.size() - 1; ++i) {
        TimePoint& p1 = points[i];
        TimePoint& p2 = points[i+1];

        // DELTA CALCULATIONS
        double d_src = p2.src_frame - p1.src_frame;
        double d_tgt = p2.tgt_frame - p1.tgt_frame;

        // SKIP ZERO DELTAS (Avoid divide by zero)
        if (d_src <= 0.0001 || d_tgt <= 0.0001) continue;

        // CALCULATE RATIO
        // Ratio = (Source_Duration / Target_Duration)
        // Example: 10s audio in 5s wall clock = 2.0 ratio (Fast)
        double ratio = d_src / d_tgt;
        double new_bpm = base_bpm * ratio;

        // 1. ROUND TO NEAREST SAMPLE (The Fix)
		// We assume the source material is digital audio, so events happen at exact integer samples.
		// llrint() uses Banker's Rounding to snap 119.99999 -> 120.
		double src_frame_rounded = (double)llrint(p1.src_frame);

		// 2. CALCULATE TIME BASED ON ROUNDED FRAME
		double src_seconds = src_frame_rounded / sample_rate;
		
		// 3. CALCULATE TICK (Round nearest instead of floor)
		// We also use llrint here to ensure the final tick is the closest integer.
		int tick = (int)llrint(src_seconds * (base_bpm / 60.0) * tpq);

        // Add Tempo Event
        midifile.addTempo(0, tick, new_bpm);
    }

    // 4. Add Safety/End Marker
    // To ensure the last segment holds its tempo or returns to normal
    TimePoint& last = points.back();
    double last_seconds = last.src_frame / sample_rate;
    int last_tick = (int)(last_seconds * (base_bpm / 60.0) * tpq);
    
    // Reset to exactly Base BPM at the end of the file
    midifile.addTempo(0, last_tick, base_bpm);
    
    // --- NEW: ADD DUMMY NOTE TO TRACK 1 ---
    // This creates a silent note spanning the entire length of the audio.
    // It forces Ableton to see the file size and might fix the DP truncation bug.
    
    midifile.addTrack(1); // Ensure we have a second track (Track 1)
    
    int channel = 0;
    int note = 60;    // C3
    int velocity = 1; // Near silent
    
    // Note On at Tick 0
    midifile.addNoteOn(1, 0, channel, note, velocity);
    
    // Note Off at the exact end of the audio (last_tick)
    midifile.addNoteOff(1, last_tick, channel, note, 0);
    
    cout << "Added dummy note on Track 1 (Duration: " << last_tick << " ticks)" << endl;

    // 5. Write File
    midifile.sortTracks();
    midifile.write(output_path);

    cout << "Successfully wrote: " << output_path << endl;

    return 0;
}
