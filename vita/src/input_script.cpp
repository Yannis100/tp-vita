/**
 * Frame-counted input injection for automated testing.
 *
 * WHY THIS EXISTS
 * `tests/vita3k/script` replays keys with xdotool, and its own header states the honest limit: the key
 * SEQUENCE is fixed but the TIMING is wall-clock. The emulator's speed varies between runs, so a press
 * lands on a different frame each time. That makes it a reproducible key sequence, not a reproducible
 * run -- useless for anything that must happen on a specific frame (title screen -> file select ->
 * gameplay), and useless for a bisect.
 *
 * This module makes the input reproducible by counting FRAMES instead of seconds. It is called once
 * per frame from the game loop, advances one step of a small script, and pushes the resulting pad state
 * through `PADSetVirtualStatus()`, which aurora already merges into every `PADRead()`. No keyboard is
 * involved, so nothing depends on the host's timing, focus or window manager.
 *
 * INPUT FORMAT (`ux0:data/tpvita/input_script.txt`, absent = feature off, zero cost)
 *   # comment
 *   <frames> <name>[,<name>...]          hold these inputs for N frames
 *   <frames> .                           hold nothing for N frames (a gap)
 *
 * BUTTON names (PADStatus::button), exactly as the GameCube pad reports them:
 *   A B X Y START LEFT RIGHT UP DOWN L R Z
 *
 * STICK names (PADStatus::stickX / stickY, analog deflection):
 *   STICKLEFT STICKRIGHT STICKUP STICKDOWN
 *
 * THE TWO SETS ARE NOT INTERCHANGEABLE, AND THIS IS THE ENTIRE REASON BOTH EXIST.
 * Dusklight's menus do not read the D-pad. They read the analog stick through
 * STControl::checkLeftTrigger() / checkRightTrigger(), which needs a nonzero `stickValue` and derives
 * the direction from `stickAngle`. A script that presses `LEFT` on the memory-card Yes/No prompt sets
 * PAD_BUTTON_LEFT, and nothing in d_file_select.cpp ever looks at that bit, so the cursor does not
 * move, A confirms "No" again, and the game loops forever between "create a save file?" and "continue
 * without saving?". Measured, twice, before the cause was found: see tests/vita3k/scripts/.
 *
 * Example: title -> card prompt -> answer YES -> gameplay.
 *   480 .
 *   60 START
 *   120 .
 *   60 A
 *   180 .
 *   30 STICKLEFT,LEFT     # move the Yes/No cursor to Yes (stick), D-pad too for good measure
 *   30 A
 *
 * SAFETY AND HONEST LIMITS
 * - Off unless the file exists, and it removes the file after reading it so a later launch of the same
 *   work directory does not silently inject input into a manual test.
 * - It cannot distinguish "the game reached the frame I expected" from "the game was still loading";
 *   it counts frames of the main loop, which includes the boot screens' frames. A script therefore
 *   needs generous gaps, and a run that fails to advance is a finding, not a pass.
 * - It reports every step in the boot log with the frame number it fired on, so a run can be checked
 *   against the script afterwards instead of being believed.
 */

#include "input_script.hpp"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dolphin/pad.h>

namespace tpv {
namespace {

constexpr const char* kScriptPath = "ux0:data/tpvita/input_script.txt";
constexpr const char* kLogPath = "ux0:data/tpvita/input_script.log";

// Not the game's logger: this file must not depend on aurora::Module (the port's main.cpp has no
// Module). Two sinks on purpose, and both are needed:
//   - stderr, flushed every line, is what survives a process that is about to die;
//   - the file is what can actually be READ BACK afterwards. Vita3K does not forward the guest's
//     stderr into vita3k.log -- measured, not assumed: a run that opened input_script.txt produced no
//     [input script] lines in vita3k.log at all -- so stderr alone is invisible in the harness. On a
//     real Vita there is no log to read, so the file is the only record there too.
void say(const char* format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);

  std::fprintf(stderr, "[input script] %s\n", text);
  std::fflush(stderr);
  FILE* file = std::fopen(kLogPath, "a");
  if (file != nullptr) {
    std::fprintf(file, "[input script] %s\n", text);
    std::fclose(file);
  }
}
// A hard ceiling on the whole run. A script that runs past this is almost certainly wrong (a typo in a
// frame count makes the game sit in the title screen while the injector waits for frame 90000).
constexpr long kMaxTotalFrames = 200000;

struct Step {
  long frames = 0;
  u16 buttons = 0;
  s8 stickX = 0; // main stick deflection, -128..127 (GameCube convention: +X is right)
  s8 stickY = 0; // main stick deflection, -128..127 (+Y is UP on the Cube; PADStatus flips it below)
  std::string text; // as written, for the log
};

// Deflection used for a scripted direction: FULL deflection, 127 of 127.
//
// This is not arbitrary, and the two obvious smaller values both fail, each for a different reason.
// The numbers below were read out of the code, and then measured:
//
//   * PADClamp() runs on every PADRead, and ClampStick() first subtracts a dead zone of 15 then
//     rescales by max/(127-15) = 72/112 for the main stick. It also shrinks a pure-axis push, because
//     the octagon test in ClampStick() only preserves the radius on the DIAGONALS. A raw 100 on one
//     axis comes out of PADClamp as 54, not 100.
//   * JUTGamePad::CStick::update() then divides by 54 (main stick, EClampStick), so 54/54 = 1.0.
//   * dFile_select.cpp constructs its STControl with threshold 0.9, and STControl::checkTrigger()
//     requires stickValue >= mThreshold. A raw 100 survives at exactly 1.0 (54 clamped, /54), and a
//     raw 127 also saturates to 1.0. This is why a SMALLER push is the trap: 90 raw becomes 45
//     clamped, 45/54 = 0.83 < 0.9, and the cursor never moves while the log looks perfect.
//
// Full deflection is used because it is the one value that cannot be knocked below the threshold by
// whichever clamp the game has installed: JUTGamePad::sClampMode is game-controlled, and under
// EClampCircle the divisors are 56/38 rather than 54, so a value tuned for one clamp is wrong for the
// other. Full deflection saturates to 1.0 under both.
//
// The ANGLE is not a problem for a pure-axis push: with mPosY == 0, CStick::update sets the angle to
// exactly -0x4000 (left) or 0x4000 (right), and checkTrigger()'s sector chain classifies -0x4000 as
// TRIG_LEFT and 0x4000 as TRIG_RIGHT. A HALF-diagonal push would land on a sector boundary and could
// pick either neighbour, which is why only one axis is pushed at a time.
constexpr int kStickPush = 127;

// KNOWN LIMIT, measured: pushing the stick at a frame where the game is showing a plain message
// rather than a Yes/No prompt does nothing, and the press is simply lost. Frame counts are exact;
// the SCREEN at a given frame is not, because the game's state machine depends on how many frames
// each animation took. Scripts must therefore repeat a navigation press across a wide window instead
// of firing it once -- see tests/vita3k/scripts/frame_card_sweep.txt.

std::vector<Step> sSteps;
size_t sStep = 0;
long sFrameInStep = 0;
long sFrameTotal = 0;
bool sActive = false;
bool sExhausted = false;
u16 sLastButtons = 0;
s8 sLastStickX = 0;
s8 sLastStickY = 0;

// Two separate namespaces on purpose: a name either names a BUTTON or a STICK DIRECTION, never both.
// This distinction is the whole reason the first version of this driver could not leave the memory
// card prompt. "LEFT" is a real GameCube button (the D-pad), and Dusklight's menus do not read the
// D-pad -- they read the analog stick through STControl::checkLeftTrigger(), which needs
// PADStatus::stickX, not PADStatus::button. Pressing LEFT did nothing because it set a bit nothing
// looks at. The names below say which one you mean.
enum class Input : uint8_t { None, Button, Stick };

Input input_for(const char* name, size_t len, Step& out) {
  const auto is = [name, len](const char* candidate) {
    return std::strlen(candidate) == len && std::strncmp(name, candidate, len) == 0;
  };
  // Stick directions.
  if (is("STICKLEFT")) {
    out.stickX = static_cast<s8>(-kStickPush);
    return Input::Stick;
  }
  if (is("STICKRIGHT")) {
    out.stickX = static_cast<s8>(kStickPush);
    return Input::Stick;
  }
  if (is("STICKUP")) {
    out.stickY = static_cast<s8>(kStickPush);
    return Input::Stick;
  }
  if (is("STICKDOWN")) {
    out.stickY = static_cast<s8>(-kStickPush);
    return Input::Stick;
  }
  // Buttons.
  if (is("A")) {
    out.buttons = static_cast<u16>(out.buttons | PAD_BUTTON_A);
    return Input::Button;
  }
  if (is("B")) {
    out.buttons = static_cast<u16>(out.buttons | PAD_BUTTON_B);
    return Input::Button;
  }
  if (is("X")) {
    out.buttons = static_cast<u16>(out.buttons | PAD_BUTTON_X);
    return Input::Button;
  }
  if (is("Y")) {
    out.buttons = static_cast<u16>(out.buttons | PAD_BUTTON_Y);
    return Input::Button;
  }
  if (is("START")) {
    out.buttons = static_cast<u16>(out.buttons | PAD_BUTTON_START);
    return Input::Button;
  }
  if (is("LEFT")) {
    out.buttons = static_cast<u16>(out.buttons | PAD_BUTTON_LEFT);
    return Input::Button;
  }
  if (is("RIGHT")) {
    out.buttons = static_cast<u16>(out.buttons | PAD_BUTTON_RIGHT);
    return Input::Button;
  }
  if (is("UP")) {
    out.buttons = static_cast<u16>(out.buttons | PAD_BUTTON_UP);
    return Input::Button;
  }
  if (is("DOWN")) {
    out.buttons = static_cast<u16>(out.buttons | PAD_BUTTON_DOWN);
    return Input::Button;
  }
  if (is("L")) {
    out.buttons = static_cast<u16>(out.buttons | PAD_TRIGGER_L);
    return Input::Button;
  }
  if (is("R")) {
    out.buttons = static_cast<u16>(out.buttons | PAD_TRIGGER_R);
    return Input::Button;
  }
  if (is("Z")) {
    out.buttons = static_cast<u16>(out.buttons | PAD_TRIGGER_Z);
    return Input::Button;
  }
  return Input::None;
}

// Parses "30 A,B" into a Step. Returns false for anything it does not understand, so a typo in the
// script is a loud failure at load time rather than a silently ignored line.
bool parse_step(const char* line, Step& out) {
  char* end = nullptr;
  const long frames = std::strtol(line, &end, 10);
  if (end == line || frames <= 0 || frames > kMaxTotalFrames) {
    return false;
  }
  while (*end == ' ' || *end == '\t') {
    ++end;
  }
  out.frames = frames;
  out.text = line;
  out.buttons = 0;
  if (*end == '.' && (end[1] == '\0' || end[1] == '\n' || end[1] == '\r')) {
    return true; // explicit gap
  }
  bool any = false;
  while (*end != '\0' && *end != '\n' && *end != '\r') {
    const char* nameStart = end;
    while (*end != '\0' && *end != ',' && *end != '\n' && *end != '\r' && *end != ' ' && *end != '\t') {
      ++end;
    }
    const size_t len = static_cast<size_t>(end - nameStart);
    if (len != 0) {
      if (input_for(nameStart, len, out) == Input::None) {
        return false; // unknown name
      }
      any = true;
    }
    while (*end == ',' || *end == ' ' || *end == '\t') {
      ++end;
    }
  }
  return any;
}

void push(const Step& step) {
  PADStatus status{};
  status.button = step.buttons;
  // Axis convention: PADStatus::stickX is +right, and stickY is +DOWN (the GameCube's raw axes are
  // inverted relative to screen space, and aurora's PADRead normalises to screen space). The script
  // therefore stores +Y as UP and negates it here, once, instead of making every script author
  // remember the sign.
  status.stickX = step.stickX;
  status.stickY = static_cast<s8>(-step.stickY);
  PADSetVirtualStatus(0, &status);
  sLastButtons = step.buttons;
  sLastStickX = step.stickX;
  sLastStickY = step.stickY;
}

void release() {
  Step none;
  push(none);
}

} // namespace

void input_script_load() {
  FILE* file = std::fopen(kScriptPath, "rb");
  if (file == nullptr) {
    return;
  }

  std::remove(kLogPath); // a stale log from a previous run would be read as this run's

  std::vector<Step> steps;
  char line[256];
  int lineNo = 0;
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    ++lineNo;
    char* cursor = line;
    while (*cursor == ' ' || *cursor == '\t') {
      ++cursor;
    }
    // Strip a trailing comment BEFORE parsing. Without this, "60 .   # settle" fails: the step text
    // is followed by words that are not button names, and the parser is strict on purpose. The first
    // version of this loader had exactly that bug and rejected every commented step, which read as
    // "the injector did nothing" rather than "the script was not understood" until the log existed.
    if (char* hash = std::strchr(cursor, '#'); hash != nullptr) {
      *hash = '\0';
    }
    // Trim trailing whitespace so "60 ." and "60 .   " are the same line.
    char* endOfText = cursor + std::strlen(cursor);
    while (endOfText > cursor && (endOfText[-1] == ' ' || endOfText[-1] == '\t' || endOfText[-1] == '\r' ||
                                  endOfText[-1] == '\n')) {
      --endOfText;
    }
    *endOfText = '\0';
    if (*cursor == '\0') {
      continue;
    }
    Step step;
    if (!parse_step(cursor, step)) {
      // A bad line must not half-apply: the whole script is rejected, because a partially loaded
      // script moves the game to a state nobody asked for.
      say("line %d is not understood, script ignored: '%s'", lineNo, cursor);
      steps.clear();
      break;
    }
    steps.push_back(std::move(step));
  }
  std::fclose(file);

  if (steps.empty()) {
    return;
  }

  // Consumed: a later launch from the same work directory must not inject input into a manual test.
  std::remove(kScriptPath);

  long total = 0;
  for (const auto& step : steps) {
    total += step.frames;
  }
  sSteps = std::move(steps);
  sStep = 0;
  sFrameInStep = 0;
  sFrameTotal = 0;
  sActive = true;
  sExhausted = false;
  // Cast every width explicitly. newlib's vsnprintf does not support "%zu" -- the first version
  // printed a literal "zu" and, worse, consumed no argument, so every later value in the line shifted
  // by one. "%u" and "%d" are safe everywhere; long is 32-bit on this target, so a plain cast to int
  // cannot truncate a frame count that the parser already capped at kMaxTotalFrames.
  say("%u step(s), %d frames total -- frames are COUNTED, not timed", static_cast<unsigned>(sSteps.size()),
      static_cast<int>(total));
  for (const auto& step : sSteps) {
    say("  %d frames  buttons 0x%04x stick %d,%d   (%s)", static_cast<int>(step.frames),
        static_cast<unsigned>(step.buttons), static_cast<int>(step.stickX), static_cast<int>(step.stickY),
        step.text.c_str());
  }
}

void input_script_frame() {
  if (!sActive || sExhausted) {
    return;
  }

  if (sStep >= sSteps.size()) {
    // Release everything: a stuck button would hold the game in a menu forever, and a stuck stick
    // would walk the player off a ledge during the frames the harness spends taking screenshots.
    if (sLastButtons != 0 || sLastStickX != 0 || sLastStickY != 0) {
      release();
    }
    sExhausted = true;
    say("finished at frame %d (buttons and stick released)", static_cast<int>(sFrameTotal));
    return;
  }

  const Step& step = sSteps[sStep];
  if (sFrameInStep == 0) {
    say("frame %d -> buttons 0x%04x stick %d,%d for %d frames (%s)", static_cast<int>(sFrameTotal),
        static_cast<unsigned>(step.buttons), static_cast<int>(step.stickX), static_cast<int>(step.stickY),
        static_cast<int>(step.frames), step.text.c_str());
  }

  push(step);
  ++sFrameInStep;
  ++sFrameTotal;

  if (sFrameInStep >= step.frames) {
    sFrameInStep = 0;
    ++sStep;
  }
  if (sFrameTotal >= kMaxTotalFrames) {
    say("aborted at the %d frame ceiling", static_cast<int>(kMaxTotalFrames));
    release();
    sExhausted = true;
  }
}

long input_script_frame_number() { return sFrameTotal; }

bool input_script_active() { return sActive && !sExhausted; }

} // namespace tpv
