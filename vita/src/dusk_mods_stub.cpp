/**
 * Vita port: the graphics and camera mod services are WebGPU-bound and not built on Vita
 * (src/dusk/mods/svc/{gfx,camera}.cpp are excluded). These definitions satisfy the references
 * from m_Do_graphic.cpp, d_camera.cpp and the service registry; both modules report
 * themselves unavailable so the registry skips them.
 */
#include "dusk/camera_operators.hpp"
#include "dusk/gfx.hpp"
#include "dusk/mods/svc/registry.hpp"
#include "mods/svc/camera.h"
#include "mods/svc/gfx.h"

namespace dusk::mods::svc {
namespace {
bool unavailable() { return false; }
}  // namespace

constinit const ServiceModule g_cameraModule{.id = CAMERA_SERVICE_ID, .available = unavailable};
constinit const ServiceModule g_gfxModule{.id = GFX_SERVICE_ID, .available = unavailable};
}  // namespace dusk::mods::svc

namespace dusk::mods {
void gfx_run_stage(GfxStage, const view_class*, const view_port_class*) {}
}  // namespace dusk::mods

bool dusk::mods::camera_run_operators(dCamera_c*) { return false; }
