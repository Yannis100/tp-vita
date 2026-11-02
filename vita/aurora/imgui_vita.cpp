// Vita backend: Dear ImGui context + a small GLES2 renderer (vitaGL).
//
// Aurora's imgui.cpp drives the SDL3 backend and a WebGPU/SDL_Renderer renderer; neither exists on
// Vita. The game only needs a valid context each frame (menus, console, debug windows), so this
// keeps the context, builds the font atlas, and draws the frame's ImDrawData with client arrays.
#include "imgui_vita.hpp"

#include <aurora/imgui.h>

#include "gfx/vita_gl.hpp"
#include "logging.hpp"

#include <chrono>
#include <cstring>
#include <vector>

namespace aurora::vita::imgui {
namespace {
Module Log("aurora::vita::imgui");

GLuint sProgram = 0;
GLint sLocXf = -1;
GLint sLocTex = -1;
GLuint sFontTexture = 0;
std::vector<GLuint> sUserTextures;
std::chrono::steady_clock::time_point sLastFrame;
bool sFrameActive = false;

const char* VertexSource = "attribute vec2 aPos;\n"
                           "attribute vec2 aUv;\n"
                           "attribute vec4 aCol;\n"
                           "uniform vec4 uXf;\n"
                           "varying vec2 vUv;\n"
                           "varying vec4 vCol;\n"
                           "void main() {\n"
                           "  vUv = aUv;\n"
                           "  vCol = aCol;\n"
                           "  gl_Position = vec4(aPos * uXf.xy + uXf.zw, 0.0, 1.0);\n"
                           "}\n";
const char* FragmentSource = "precision mediump float;\n"
                             "uniform sampler2D uTex;\n"
                             "varying vec2 vUv;\n"
                             "varying vec4 vCol;\n"
                             "void main() { gl_FragColor = vCol * texture2D(uTex, vUv); }\n";

GLuint compile(GLenum type, const char* source) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  return shader;
}

bool ensure_program() {
  if (sProgram != 0) {
    return true;
  }
  const GLuint vs = compile(GL_VERTEX_SHADER, VertexSource);
  const GLuint fs = compile(GL_FRAGMENT_SHADER, FragmentSource);
  const GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glBindAttribLocation(program, 0, "aPos");
  glBindAttribLocation(program, 1, "aUv");
  glBindAttribLocation(program, 2, "aCol");
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[1024] = {};
    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
    Log.error("imgui program link failed: {}", log);
    glDeleteProgram(program);
    return false;
  }
  sProgram = program;
  sLocXf = glGetUniformLocation(program, "uXf");
  sLocTex = glGetUniformLocation(program, "uTex");
  glUseProgram(program);
  glUniform1i(sLocTex, 0);
  return true;
}

GLuint upload_rgba8(uint32_t width, uint32_t height, const void* pixels) {
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA,
               GL_UNSIGNED_BYTE, pixels);
  return texture;
}

// (Re)builds the font atlas texture whenever the game replaced the fonts (ImGuiEngine_Initialize).
void ensure_font_texture() {
  ImGuiIO& io = ImGui::GetIO();
  if (io.Fonts->TexID != 0 && sFontTexture != 0) {
    return;
  }
  unsigned char* pixels = nullptr;
  int width = 0;
  int height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  if (sFontTexture != 0) {
    glDeleteTextures(1, &sFontTexture);
  }
  sFontTexture = upload_rgba8(static_cast<uint32_t>(width), static_cast<uint32_t>(height), pixels);
  io.Fonts->SetTexID(static_cast<ImTextureID>(sFontTexture));
}
} // namespace

void create_context(const char* iniPath) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = iniPath;
  io.BackendPlatformName = "tp-vita";
  io.BackendRendererName = "tp-vita gles2";
  io.DisplaySize = ImVec2(960.f, 544.f);
  io.DisplayFramebufferScale = ImVec2(1.f, 1.f);
  sLastFrame = std::chrono::steady_clock::now();
}

void new_frame(uint32_t width, uint32_t height) {
  if (ImGui::GetCurrentContext() == nullptr) {
    return;
  }
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
  const auto now = std::chrono::steady_clock::now();
  io.DeltaTime = std::max(std::chrono::duration<float>(now - sLastFrame).count(), 1.f / 1000.f);
  sLastFrame = now;
  ensure_font_texture();
  ImGui::NewFrame();
  sFrameActive = true;
}

void render_and_draw(uint32_t width, uint32_t height) {
  if (!sFrameActive) {
    return;
  }
  sFrameActive = false;
  ImGui::Render();
  ImDrawData* data = ImGui::GetDrawData();
  if (data == nullptr || data->CmdListsCount == 0 || !ensure_program()) {
    return;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_POLYGON_OFFSET_FILL);
  glDepthMask(GL_FALSE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_SCISSOR_TEST);
  glUseProgram(sProgram);
  glActiveTexture(GL_TEXTURE0);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  for (uint32_t i = 3; i < 16; ++i) {
    glDisableVertexAttribArray(i);
  }

  const float scaleX = 2.f / data->DisplaySize.x;
  const float scaleY = -2.f / data->DisplaySize.y;
  glUniform4f(sLocXf, scaleX, scaleY, -1.f - data->DisplayPos.x * scaleX, 1.f - data->DisplayPos.y * scaleY);

  for (int n = 0; n < data->CmdListsCount; ++n) {
    const ImDrawList* list = data->CmdLists[n];
    const ImDrawVert* vertices = list->VtxBuffer.Data;
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert),
                          reinterpret_cast<const uint8_t*>(vertices) + offsetof(ImDrawVert, pos));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert),
                          reinterpret_cast<const uint8_t*>(vertices) + offsetof(ImDrawVert, uv));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert),
                          reinterpret_cast<const uint8_t*>(vertices) + offsetof(ImDrawVert, col));
    for (const ImDrawCmd& cmd : list->CmdBuffer) {
      if (cmd.UserCallback != nullptr) {
        continue;
      }
      const float x1 = cmd.ClipRect.x - data->DisplayPos.x;
      const float y1 = cmd.ClipRect.y - data->DisplayPos.y;
      const float x2 = cmd.ClipRect.z - data->DisplayPos.x;
      const float y2 = cmd.ClipRect.w - data->DisplayPos.y;
      if (x2 <= x1 || y2 <= y1) {
        continue;
      }
      glScissor(static_cast<GLint>(x1), static_cast<GLint>(static_cast<float>(height) - y2),
                static_cast<GLsizei>(x2 - x1), static_cast<GLsizei>(y2 - y1));
      glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(cmd.GetTexID()));
      static_assert(sizeof(ImDrawIdx) == 2, "ImDrawIdx must be 16 bit");
      glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd.ElemCount), GL_UNSIGNED_SHORT,
                     list->IdxBuffer.Data + cmd.IdxOffset);
    }
  }
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
}

ImTextureID add_texture(uint32_t width, uint32_t height, const void* rgba8) {
  const GLuint texture = upload_rgba8(width, height, rgba8);
  sUserTextures.push_back(texture);
  return static_cast<ImTextureID>(texture);
}

void shutdown() {
  if (sFrameActive && ImGui::GetCurrentContext() != nullptr) {
    ImGui::EndFrame();
    sFrameActive = false;
  }
  if (sProgram != 0) {
    glDeleteProgram(sProgram);
    sProgram = 0;
  }
  if (sFontTexture != 0) {
    glDeleteTextures(1, &sFontTexture);
    sFontTexture = 0;
  }
  for (const GLuint texture : sUserTextures) {
    glDeleteTextures(1, &texture);
  }
  sUserTextures.clear();
  if (ImGui::GetCurrentContext() != nullptr) {
    ImGui::DestroyContext();
  }
}
} // namespace aurora::vita::imgui

extern "C" ImTextureID aurora_imgui_add_texture(uint32_t width, uint32_t height, const void* rgba8) {
  return aurora::vita::imgui::add_texture(width, height, rgba8);
}
