// present(): the EFB is shown in the window at the largest size that keeps its aspect ratio, centred, on a black clear
// (vita_gl_replay.cpp present(); the Vita screen is 960 x 544, set in vita/aurora/backend.cpp; the harness EFB is 640 x 480).
//
// The contract, for an EFB of e x f pixels in a window of p x q: scale = min(p / e, q / f); the image is round(e * scale) wide
// and round(f * scale) high, its left/top edges at (p - width) / 2 and (q - height) / 2 (integer division), everything outside
// is black, and the EFB's top row is the top row of the image. The expectations below are worked out by hand for each size.
namespace present {
// Four colour quadrants over the whole EFB: red | green over blue | yellow.
void quadrants() {
  base_state();
  rect(0, 0, 320, 240, 1, 255, 0, 0);
  rect(320, 0, 640, 240, 1, 0, 255, 0);
  rect(0, 240, 320, 480, 1, 0, 0, 255);
  rect(320, 240, 640, 480, 1, 255, 255, 0);
}

// The presented image occupies [x0, x0 + w) x [y0, y0 + h) of a pw x ph window.
void check(const Image& img, Checker& c, int x0, int y0, int w, int h) {
  const int pw = img.w, ph = img.h;
  const int xm = x0 + w / 2, ym = y0 + h / 2; // the EFB's centre: where the quadrants meet
  // the four quadrants, well inside
  c.px(img, x0 + w / 4, y0 + h / 4, 255, 0, 0);
  c.px(img, x0 + 3 * w / 4, y0 + h / 4, 0, 255, 0);
  c.px(img, x0 + w / 4, y0 + 3 * h / 4, 0, 0, 255);
  c.px(img, x0 + 3 * w / 4, y0 + 3 * h / 4, 255, 255, 0);
  // where they meet: a few pixels either side of the centre lines (the filter blurs one or two pixels)
  c.px(img, xm - 4, y0 + h / 4, 255, 0, 0);
  c.px(img, xm + 4, y0 + h / 4, 0, 255, 0);
  c.px(img, x0 + w / 4, ym - 4, 255, 0, 0);
  c.px(img, x0 + w / 4, ym + 4, 0, 0, 255);
  // the outermost image pixels (left/right columns, top/bottom rows) are colour, the pixels just outside are black
  c.px(img, x0, y0 + h / 4, 255, 0, 0);
  c.px(img, x0 + w - 1, y0 + h / 4, 0, 255, 0);
  c.px(img, x0 + w / 4, y0, 255, 0, 0);
  c.px(img, x0 + w / 4, y0 + h - 1, 0, 0, 255);
  if (x0 > 0) {
    c.px(img, x0 - 1, y0 + h / 4, 0, 0, 0, 0);
    c.px(img, 0, ph / 2, 0, 0, 0, 0);
  }
  if (x0 + w < pw) {
    c.px(img, x0 + w, y0 + h / 4, 0, 0, 0, 0);
    c.px(img, pw - 1, ph / 2, 0, 0, 0, 0);
  }
  if (y0 > 0) {
    c.px(img, x0 + w / 4, y0 - 1, 0, 0, 0, 0);
    c.px(img, pw / 2, 0, 0, 0, 0, 0);
  }
  if (y0 + h < ph) {
    c.px(img, x0 + w / 4, y0 + h, 0, 0, 0, 0);
    c.px(img, pw / 2, ph - 1, 0, 0, 0, 0);
  }
}
} // namespace present

void add_present_scenes(std::vector<Scene>& s) {
  // The Vita screen: 640x480 into 960x544. scale = min(1.5, 1.1333) = 1.1333 -> 725 x 544, x0 = (960 - 725) / 2 = 117.
  s.push_back({"present_vita_960x544",
               "EFB 640x480 presented at 960x544 (the Vita screen): 725x544 at x = 117, black pillars either side",
               [] { present::quadrants(); },
               [](const Image& img, Checker& c) { present::check(img, c, 117, 0, 725, 544); },
               nullptr, 960, 544});

  // Taller than 4:3: scale = min(1, 1.25) = 1 -> 640 x 480, y0 = (600 - 480) / 2 = 60. The warmup frame fills the window's
  // bottom 480 rows white, so a present that does not clear leaves white bars.
  s.push_back({"present_letterbox_640x600",
               "EFB 640x480 presented at 640x600: 640x480 at y = 60, black bars above and below (a missing clear leaves the "
               "previous frame in the bar)",
               [] { present::quadrants(); },
               [](const Image& img, Checker& c) { present::check(img, c, 0, 60, 640, 480); },
               [] {
                 base_state();
                 rect(0, 0, W, H, 1, 255, 255, 255);
               },
               640, 600});

  // Rounding and an odd margin: scale = min(1.09375, 1.041667) = 1.041667 -> round(666.67) = 667 wide, 500 high,
  // x0 = (700 - 667) / 2 = 16 (16.5 rounded down). Truncating the width to 666 would give x0 = 17.
  s.push_back({"present_round_700x500",
               "EFB 640x480 presented at 700x500: width round(666.67) = 667, x = 16",
               [] { present::quadrants(); },
               [](const Image& img, Checker& c) { present::check(img, c, 16, 0, 667, 500); },
               nullptr, 700, 500});

  // Smaller than the EFB: scale = min(0.5, 1.1333) = 0.5 -> 320 x 240, y0 = (544 - 240) / 2 = 152.
  s.push_back({"present_downscale_320x544",
               "EFB 640x480 presented at 320x544: half size, 320x240 at y = 152, black above and below",
               [] { present::quadrants(); },
               [](const Image& img, Checker& c) { present::check(img, c, 0, 152, 320, 240); },
               nullptr, 320, 544});
}
