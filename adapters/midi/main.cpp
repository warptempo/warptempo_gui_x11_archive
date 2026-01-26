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

struct TempoPoint {
    double time_sec;
    double multiplier;
};

int main(int argc, char* argv[]) {
    // Usage: midi_adapter <input_tempomap> <output_mid> <base_bpm> <ppq>
    if (argc < 5) {
        cerr << "Usage: midi_adapter <input.tempomap> <output.mid> <base_bpm> <ppq>" << endl;
        return 1;
    }

    string input_path = argv[1];
    string output_path = argv[2];
    double base_bpm = stod(argv[3]);
    int tpq = stoi(argv[4]);

    vector<TempoPoint> points;
    ifstream infile(input_path);
    if (!infile.is_open()) {
        cerr << "Error opening input file: " << input_path << endl;
        return 1;
    }

    string line;
    while (getline(infile, line)) {
        if (line.empty()) continue;
        stringstream ss(line);
        double t, m;
        if (ss >> t >> m) {
            points.push_back({t, m});
        }
    }
    infile.close();

    if (points.empty()) {
        cerr << "Error: No points found in map." << endl;
        return 1;
    }

    MidiFile midifile;
    midifile.absoluteTicks();
    midifile.setTicksPerQuarterNote(tpq);
    
    midifile.addTrack(1); 

    cout << "Processing " << points.size() << " points..." << endl;
    
    // [FIX] Accumulate ticks based on the active tempo of each segment
    double current_abs_tick = 0.0;
    
    // Add the first tempo point at Tick 0
    // (We know the first point is time 0.0 from parser)
    if (points[0].time_sec != 0.0) {
        // Safety: If file doesn't start at 0.0, we assume base_bpm until first point
        // But parser guarantees 0.0 start.
    }
    midifile.addTempo(0, 0, base_bpm * points[0].multiplier);

    int last_tick_int = 0;

    // Loop through points to calculate the NEXT point's position
    for (size_t i = 0; i < points.size() - 1; ++i) {
        double t_start = points[i].time_sec;
        double t_next = points[i+1].time_sec;
        
        // The tempo for this segment is defined by points[i]
        double segment_bpm = base_bpm * points[i].multiplier;
        
        double dt_sec = t_next - t_start;
        
        // Calculate ticks for this segment: Time * (Beats/Sec) * (Ticks/Beat)
        double dt_ticks = dt_sec * (segment_bpm / 60.0) * tpq;
        
        current_abs_tick += dt_ticks;
        last_tick_int = (int)llrint(current_abs_tick);
        
        // Add the tempo change for the NEXT segment
        midifile.addTempo(0, last_tick_int, base_bpm * points[i+1].multiplier); 
    }

    // [OPTIONAL] Extend Dummy Note
    // The MIDI file ends at the start of the last segment (the tail).
    // We extend the dummy note by 1 Beat just to make the clip visible in DAW.
    int end_tick = last_tick_int + tpq;
    
    // [FIX] Extend Track 0 (Tempo Map) to match Track 1 length
    // We add a redundant tempo event at the very end so the DAW sees both tracks as equal length. 
    midifile.addTempo(0, end_tick, base_bpm * points.back().multiplier); 

    int channel = 0;
    int note = 60;      // C3
    int velocity = 127; 
    
    midifile.addNoteOn(1, 0, channel, note, velocity);
    midifile.addNoteOff(1, end_tick, channel, note, 0);

    cout << "Final Event at Tick: " << last_tick_int << endl;

    midifile.sortTracks();
    midifile.write(output_path);
    cout << "Successfully wrote: " << output_path << endl;
 
    return 0;
}
