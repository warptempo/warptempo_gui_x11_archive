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
    vector<TimePoint> points;
    ifstream infile(input_path);
    if (!infile.is_open()) {
        cerr << "Error opening input file." << endl;
        return 1;
    }

    string line;
    while (getline(infile, line)) {
        if (line.empty()) continue;
        stringstream ss(line);
        double s, t;
        string l = ""; // Default to empty string
        
        // UPDATED: Check for 2 columns (Source, Target) only
        if (ss >> s >> t) {
            points.push_back({s, t});
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
        points.insert(points.begin(), {0.0, 0.0});
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

    // 4. Finalize & Add Dummy Note
    // ---------------------------------------------------------
    TimePoint& last = points.back();
    
    // Use llrint to ensure the final file length matches the loop's rounding logic
    double last_frame_rounded = (double)llrint(last.src_frame);
    double last_seconds = last_frame_rounded / sample_rate;
    int last_tick = (int)llrint(last_seconds * (base_bpm / 60.0) * tpq);
    
    // A. Reset Tempo at the very end (Clean exit)
    midifile.addTempo(0, last_tick, base_bpm);
    
    // B. (Safety Buffer Removed) 
    // We rely on the Dummy Note below to define the file length.
    
    // C. Add Dummy Note to Track 1 (For Ableton)
    // This forces the DAW to recognize the full duration without adding extra silence.
    midifile.addTrack(1); 
    int channel = 0;
    int note = 60;    // C3
    int velocity = 127; // Near silent
    
    midifile.addNoteOn(1, 0, channel, note, velocity);
    midifile.addNoteOff(1, last_tick, channel, note, 0);
    
    cout << "Added dummy note on Track 1. Total Length: " << last_tick << " ticks." << endl;

    // 5. Write File
    midifile.sortTracks();
    midifile.write(output_path);

    cout << "Successfully wrote: " << output_path << endl;
 
    return 0;
}
