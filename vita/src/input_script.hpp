#pragma once

// Frame-counted input injection for automated runs. See input_script.cpp for the file format, the
// reasoning (xdotool scripts are wall-clock, so a press lands on a different frame every run) and the
// honest limits. Both calls are no-ops when ux0:data/tpvita/input_script.txt is absent.

namespace tpv {

// Reads and validates the script, then deletes it. Call once, after the boot log is open.
void input_script_load();

// Advances one frame of the script and pushes the pad state. Call once per main-loop iteration,
// before the game reads the pad.
void input_script_frame();

// Frames advanced so far, for log lines and for "did the run get as far as the script expected?".
long input_script_frame_number();

// True while there are steps left to play.
bool input_script_active();

} // namespace tpv
