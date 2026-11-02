/**
 * Vita port: placeholder for dusk::ui::icon_provider.
 *
 * The desktop icon_provider (dusklight/src/dusk/ui/icon_provider.cpp) is a dev-UI feature that
 * renders item/meter icons into the RmlUI overlay. On Vita the RmlUI overlay is not built, so
 * the real implementation (which pulls in the Aurora WebGPU/Dawn C++ layer) is excluded from
 * the build (see CMakeLists.txt TPV_DUSK_EXCLUDE). These stubs keep the callers
 * (d_meter2_draw.cpp, ui.cpp, touch_controls.cpp) linkable.
 */

#include "dusk/ui/icon_provider.hpp"
#include "dusk/ui/controls.hpp"

#include <optional>
#include <string>

class J2DPane;

namespace dusk::ui {

void register_icon_texture_provider() noexcept {}
void unregister_icon_texture_provider() noexcept {}
void update_midna_icon_texture(J2DPane*) noexcept {}

std::string midna_icon_source() {
    return {};
}

uint64_t midna_icon_revision() noexcept {
    return 0;
}

std::string item_icon_source_for_button(Control) {
    return {};
}

std::string item_count_label_for_button(Control) {
    return {};
}

std::optional<float> item_oil_fill_for_button(Control) noexcept {
    return std::nullopt;
}

}  // namespace dusk::ui
