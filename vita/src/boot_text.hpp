// Text screens for failures that happen before the game has a renderer: wrap, lay out and rasterise
// text into a plain 32-bit pixel buffer. Header-only and free of Vita headers so tests/host/disc can
// run it; vita/src/boot_screen.cpp puts the result on the display.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "boot_font.hpp"

namespace tpv {

constexpr int kCellW = kGlyphW + 1; // one blank column between glyphs
constexpr int kCellH = kGlyphH + 1; // one blank row between lines

// Pixel layout of SCE_DISPLAY_PIXELFORMAT_A8B8G8R8: a little-endian u32 is 0xAABBGGRR.
constexpr uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | r;
}

struct Canvas {
    uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int pitch = 0; // in pixels
};

inline void fill(const Canvas& c, uint32_t colour) {
    for (int y = 0; y < c.height; ++y) {
        for (int x = 0; x < c.width; ++x) {
            c.pixels[static_cast<size_t>(y) * c.pitch + x] = colour;
        }
    }
}

// Draws one glyph with its top-left corner at (x, y); `scale` is an integer pixel multiplier. Pixels
// outside the canvas are clipped.
inline void draw_glyph(const Canvas& c, int x, int y, unsigned char ch, uint32_t colour, int scale) {
    const uint8_t* rows = glyph_for(ch);
    for (int gy = 0; gy < kGlyphH; ++gy) {
        for (int gx = 0; gx < kGlyphW; ++gx) {
            if ((rows[gy] & (1u << (kGlyphW - 1 - gx))) == 0) {
                continue;
            }
            for (int sy = 0; sy < scale; ++sy) {
                for (int sx = 0; sx < scale; ++sx) {
                    const int px = x + gx * scale + sx;
                    const int py = y + gy * scale + sy;
                    if (px >= 0 && px < c.width && py >= 0 && py < c.height) {
                        c.pixels[static_cast<size_t>(py) * c.pitch + px] = colour;
                    }
                }
            }
        }
    }
}

inline void draw_text(const Canvas& c, int x, int y, const std::string& s, uint32_t colour, int scale) {
    for (size_t i = 0; i < s.size(); ++i) {
        draw_glyph(c, x + static_cast<int>(i) * kCellW * scale, y, static_cast<unsigned char>(s[i]), colour, scale);
    }
}

enum class Style : uint8_t { Title, Body, Dim, Footer };

struct Line {
    std::string text;
    Style style = Style::Body;
};

struct Screen {
    std::string title;
    std::vector<Line> body;
    std::string footer;
};

// Splits `line` into rows of at most `cols` characters. Breaks at the last space that fits, else after
// the last '/' (paths), else in the middle of a word. Continuation rows are indented two columns
// deeper than the original line.
inline std::vector<std::string> wrap_line(const std::string& line, size_t cols) {
    std::vector<std::string> out;
    size_t indent = line.find_first_not_of(' ');
    if (indent == std::string::npos) {
        indent = 0;
    }
    if (cols < indent + 8) {
        indent = 0; // pathological width: do not let the indent eat the line
    }
    if (cols == 0) {
        return {std::string()};
    }
    const size_t continuation = (indent + 2 < cols) ? indent + 2 : 0;
    std::string rest = line;
    bool first = true;
    for (;;) {
        const size_t prefix = first ? 0 : continuation;
        const size_t width = cols - prefix;
        if (rest.size() <= width) {
            out.push_back(std::string(prefix, ' ') + rest);
            break;
        }
        const size_t minCut = first ? indent + 1 : 1;
        size_t take = width;
        size_t skip = 0; // characters dropped after the cut (the space)
        const size_t space = rest.rfind(' ', width);
        if (space != std::string::npos && space >= minCut) {
            take = space;
            skip = 1;
        } else {
            const size_t slash = rest.rfind('/', width - 1);
            if (slash != std::string::npos && slash + 1 >= minCut) {
                take = slash + 1;
            }
        }
        out.push_back(std::string(prefix, ' ') + rest.substr(0, take));
        rest.erase(0, take + skip);
        while (!rest.empty() && rest.front() == ' ') {
            rest.erase(0, 1);
        }
        first = false;
        if (rest.empty()) {
            break;
        }
    }
    return out;
}

// Places the title at the top, the body below it and the footer on the last row(s), `rows` rows in
// total (blank rows pad the gap). A body that does not fit is cut and ends with a "... N more" row, so
// the footer (which says how to leave the screen) is always visible.
inline std::vector<Line> layout(const Screen& screen, int cols, int rows) {
    std::vector<Line> head;
    for (const std::string& t : wrap_line(screen.title, static_cast<size_t>(cols))) {
        head.push_back({t, Style::Title});
    }
    head.push_back({"", Style::Body});

    std::vector<Line> body;
    for (const Line& l : screen.body) {
        for (const std::string& w : wrap_line(l.text, static_cast<size_t>(cols))) {
            body.push_back({w, l.style});
        }
    }

    std::vector<Line> foot;
    for (const std::string& t : wrap_line(screen.footer, static_cast<size_t>(cols))) {
        foot.push_back({t, Style::Footer});
    }

    const int room = rows - static_cast<int>(head.size()) - static_cast<int>(foot.size()) - 1; // 1 = spacer above footer
    std::vector<Line> out = head;
    if (static_cast<int>(body.size()) <= room) {
        out.insert(out.end(), body.begin(), body.end());
    } else if (room >= 1) {
        const int shown = room - 1;
        out.insert(out.end(), body.begin(), body.begin() + shown);
        out.push_back({"... " + std::to_string(body.size() - static_cast<size_t>(shown)) + " more lines (see boot.log)",
                       Style::Dim});
    }
    while (static_cast<int>(out.size()) < rows - static_cast<int>(foot.size())) {
        out.push_back({"", Style::Body});
    }
    out.insert(out.end(), foot.begin(), foot.end());
    if (static_cast<int>(out.size()) > rows) {
        out.resize(static_cast<size_t>(rows)); // only when title + footer alone exceed the screen
    }
    return out;
}

inline uint32_t style_colour(Style s) {
    switch (s) {
    case Style::Title:
        return rgb(255, 110, 100);
    case Style::Dim:
        return rgb(160, 160, 170);
    case Style::Footer:
        return rgb(110, 220, 255);
    case Style::Body:
    default:
        return rgb(235, 235, 235);
    }
}

constexpr uint32_t kBackground = rgb(20, 20, 44);

// Columns and rows of text that fit a canvas at a scale with a margin (in pixels) on every side.
constexpr int text_cols(int width, int scale, int margin) { return (width - 2 * margin) / (kCellW * scale); }
constexpr int text_rows(int height, int scale, int margin) { return (height - 2 * margin) / (kCellH * scale); }

inline void render(const Canvas& c, const std::vector<Line>& rows, int scale, int margin) {
    fill(c, kBackground);
    for (size_t i = 0; i < rows.size(); ++i) {
        draw_text(c, margin, margin + static_cast<int>(i) * kCellH * scale, rows[i].text, style_colour(rows[i].style),
                  scale);
    }
}

} // namespace tpv
