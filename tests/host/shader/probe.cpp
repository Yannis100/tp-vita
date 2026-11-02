#include "gx/vita_gx.hpp"
#include "gx/shader_info.hpp"
int main() {
  using namespace aurora::gx;
  PipelineConfig pc{};
  pc.shaderConfig = {};
  auto info = build_shader_info(pc.shaderConfig);
  auto ps = vita::build_program_source(pc, info);
  return ps.vertex.size() > 0 ? 0 : 1;
}
