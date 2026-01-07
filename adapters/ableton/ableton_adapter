#!/bin/bash
# Run only from terminal
if [ ! -t 0 ]; then exit; fi
shopt -s nocaseglob
set -e
export LC_ALL=C

	rm -rf ".ableton Project"
	cp -r "$script_dir/adapters/ableton/.ableton Project" .
	if [[ ! -z "$begin_time" ]] && [[ ! -z "$end_time" ]]; then
		cp .$md5-trimmed.wav ".ableton Project/Samples/Imported/${audio_input%.*}.wav"
	elif [[ "${audio_input,,}" != *.wav ]]; then
		if [[ -f "${audio_input%.*}.wav" ]]; then
			fmt=$(ffprobe -v error -select_streams a:0 -show_entries stream=sample_fmt -of default=noprint_wrappers=1:nokey=1 "${audio_input%.*}.wav")
			if [[ "$fmt" != "flt" ]]; then
				echo "File exists but is $fmt (not 32-bit float). Removing and re-rendering as 32-bit float."
				rm "${audio_input%.*}.wav"
				ffmpeg -i "$audio_input" -c:a pcm_f32le "${audio_input%.*}.wav"
			fi
		elif [[ ! -f "${audio_input%.*}.wav" ]]; then
			ffmpeg -i "$audio_input" -c:a pcm_f32le "${audio_input%.*}.wav"
		fi
		cp "${audio_input%.*}.wav" ".ableton Project/Samples/Imported"
	else
		cp "$audio_input" ".ableton Project/Samples/Imported"
	fi
	
	# 1. Locate and prepare the Project files
	project_dir=".ableton Project"
	als_file="$project_dir/.ableton.als"
	if [[ -z "$als_file" ]]; then echo "Error: No .als file found in template."; exit 1; fi
	
	# Define destination path inside the Project (Standard Ableton structure)
	# Note: $title is used for the filename as requested
	dest_filename="${audio_input%.*}.wav"
	dest_rel_path="Samples/Imported"
	
	# 2. Get Statistics for the New Audio File
	# We probe the actual file in the project to ensure data consistency
	eval $(ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate,duration_ts -of default=noprint_wrappers=1:nokey=0 "$project_dir/$dest_rel_path/$dest_filename")
	file_size=$(stat -c%s "$project_dir/$dest_rel_path/$dest_filename")
	
	# 3. Calculate Final Timeline Duration
	# Read the last target frame from the timemap to know where the clip ends
	# At 60 BPM, 1 Beat = 1 Second. Duration(s) = Frame / SampleRate
	last_target_frame=$(tail -n 1 ".$md5-timemap-precise" | cut -d ' ' -f 2)
	final_duration=$(echo "$last_target_frame / $sample_rate" | bc -l)
	
	# 4. Generate the Warp Markers XML Block
	# Find the largest existing 'Id' attribute to avoid collisions
	# We stream the compressed ALS file to grep to find the max ID
	max_id=$(gzip -cd "$als_file" | grep -o 'Id="[0-9]*"' | cut -d'"' -f2 | sort -rn | head -n1)
	
	# Start numbering one higher than the max found (default to 1 if none found)
	id=$(( ${max_id:-0} + 1 ))

	# Convert Source/Target Frames to Seconds/Beats
	markers_xml=""
	while read -r src_frame tgt_frame label; do
		# Math: Frame / SR = Seconds (and Beats at 60 BPM)
		sec_time=$(echo "$src_frame / $sample_rate" | bc -l)
		beat_time=$(echo "$tgt_frame / $sample_rate" | bc -l)
		
		# Append to XML string
		markers_xml+="<WarpMarker Id=\"$id\" SecTime=\"$sec_time\" BeatTime=\"$beat_time\" />"
		((id++))
	done < ".$md5-timemap-precise"
	
	# 5. Patch the ALS (XML) File
	# Decompress the Live Set
	gunzip -c "$als_file" > "$als_file.xml"
	xml_file="$als_file.xml" # The unzipped XML path
	volume="0.5011872053" # -6db
	
	# --- STRICT DETECTION LOGIC ---

	# 1. Identify the "Victim" (The specific file currently in the template)
	# We grep for the path in Samples/Imported and extract just the filename.
	# This ensures we target ONLY the audio clip being replaced.
	current_import_path=$(grep -o "Samples/Imported/[^\"]*" "$xml_file" | head -n 1)

	if [[ -z "$current_import_path" ]]; then
		echo "Error: Could not find any file in 'Samples/Imported/' within the template."
		exit 1
	fi

	template_filename=$(basename "$current_import_path")
	# Get the name without extension (for Track Name matching)
	template_basename="${template_filename%.*}"

	# --- STRICT REPLACEMENTS ---

	# 1. Replace the File Reference (Path and Filename)
	# We strictly replace the detected path string.
	sed -i "s|$current_import_path|Samples/Imported/$dest_filename|g" "$xml_file"

	# 2. Update File Size (Protecting Reverbs/Presets)
	# The regex `!/Value="0"/` tells sed to SKIP lines where Value="0".
	# This prevents corruption of Reverb/Delay devices which use 0 as a placeholder.
	sed -i "/OriginalFileSize/ {
		/Value=\"0\"/! s|Value=\"[0-9]*\"|Value=\"$file_size\"|
	}" "$xml_file"

	# 3. Rename the Track/Clip (Protecting "Main" and "Returns")
	# We only replace EffectiveName/Name if it matches the OLD template filename/basename.
	# This prevents renaming 'Main' to '-3 Main'.
	sed -i "s|Value=\"$template_basename\"|Value=\"$base_name\"|g" "$xml_file"
	sed -i "s|Value=\"$template_filename\"|Value=\"$dest_filename\"|g" "$xml_file"

	# 4. Set Clip Volume to -6dB
	# -6dB in linear amplitude is approx 0.5011872053
	sed -i "s|SampleVolume Value=\"[0-9.]*\"|SampleVolume Value=\"0.5011872053\"|g" "$xml_file"

	# 5. Update Audio Properties (Duration & SampleRate)
	sed -i "s|DefaultDuration Value=\"[0-9]*\"|DefaultDuration Value=\"$duration_ts\"|g" "$xml_file"
	sed -i "s|DefaultSampleRate Value=\"[0-9]*\"|DefaultSampleRate Value=\"$sample_rate\"|g" "$xml_file"

	# 6. Update Timeline/Looping
	# We update every reference to the track length to ensure the loops and views match the new file.

	# A. Clip Loop and End Points
	sed -i "s|CurrentEnd Value=\"[0-9.]*\"|CurrentEnd Value=\"$final_duration\"|g" "$xml_file"
	sed -i "s|LoopEnd Value=\"[0-9.]*\"|LoopEnd Value=\"$final_duration\"|g" "$xml_file"
	sed -i "s|HiddenLoopEnd Value=\"[0-9.]*\"|HiddenLoopEnd Value=\"$final_duration\"|g" "$xml_file"
	sed -i "s|OutMarker Value=\"[0-9.]*\"|OutMarker Value=\"$final_duration\"|g" "$xml_file"

	# B. Global Transport Loop and Position
	sed -i "s|LoopLength Value=\"[0-9.]*\"|LoopLength Value=\"$final_duration\"|g" "$xml_file"
	sed -i "s|CurrentTime Value=\"[0-9.]*\"|CurrentTime Value=\"$final_duration\"|g" "$xml_file"

	# C. Time Selection (Arrangement View Highlight)
	sed -i "s|OtherTime Value=\"[0-9.]*\"|OtherTime Value=\"$final_duration\"|g" "$xml_file"

	# 7. Inject Warp Markers
	# Create a safe temp file for markers
	echo "<WarpMarkers>$markers_xml</WarpMarkers>" > markers.tmp
	# Read content and escape for sed
	marker_content=$(cat markers.tmp | sed 's/\\/\\\\/g' | sed 's/\//\\\//g' | sed 's/&/\\&/g')

	# Replace the WarpMarkers block
	sed -i "/<WarpMarkers>/,/<\/WarpMarkers>/c $marker_content" "$xml_file"
	rm markers.tmp

	# --- FINALIZATION ---
	
	# Re-compress and Cleanup
	gzip -c "$xml_file" > "$als_file"
	
	echo "Generated Ableton Project at $project_dir"
	
