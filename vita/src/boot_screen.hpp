// Puts a text screen on the Vita display before the game has a renderer. Vita only (the text layout and
// rasteriser are in boot_text.hpp and are tested on the host).
#pragma once

#include "boot_text.hpp"

namespace tpv {

// Draws `screen` straight to the display (no GXM, no vitaGL) and blocks until the user presses O or X, then
// returns true. Returns false at once, showing nothing, if display memory could not be allocated. The
// memory is never released: the caller exits the process, and the display must not scan freed memory.
bool show_screen_and_wait(const Screen& screen);

} // namespace tpv
