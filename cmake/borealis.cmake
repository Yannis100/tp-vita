# Borealis (TwilitRealm app framework) for Vita: no HTTP/websocket/sentry/discord backends.
set(_B ${BOREALIS_DIR})
set(BOREALIS_APP_DESCRIBE "vita-dev")
set(BOREALIS_APP_VERSION "0.0.0.0")
set(BOREALIS_APP_SHORT_VERSION "0.0.0")
set(BOREALIS_APP_VERSION_CODE 0)
set(BOREALIS_APP_BRANCH "vita")
set(BOREALIS_APP_REVISION "unknown")
string(TIMESTAMP BOREALIS_APP_DATE "%Y-%m-%d")
set(BOREALIS_PLATFORM_NAME "PS Vita")
set(BOREALIS_LIB_DESCRIBE "vita")
configure_file(${_B}/cmake/version.h.in ${CMAKE_BINARY_DIR}/generated/borealis/version.h @ONLY)

add_library(tpv_borealis STATIC
  ${_B}/src/url.cpp ${_B}/src/task.cpp ${_B}/src/log.cpp ${_B}/src/cli.cpp
  ${_B}/src/io.cpp ${_B}/src/presentation.cpp ${_B}/src/data.cpp
  ${_B}/src/sentry.cpp ${_B}/src/sentry_config.cpp
  ${_B}/src/discord/stub.cpp
  ${_B}/src/http.cpp ${_B}/src/http_download.cpp ${_B}/src/http/no_backend.cpp
  ${_B}/src/ws.cpp ${_B}/src/ws/no_backend.cpp
  ${_B}/src/update/github.cpp ${_B}/src/update/version.cpp
  ${_B}/src/disc.cpp ${TPV_DIR}/vita/src/borealis_crash_stub.cpp ${_B}/src/file_select/file_select.cpp
  ${_B}/src/net.cpp ${_B}/src/net/platform_posix.cpp
)
target_include_directories(tpv_borealis PUBLIC ${_B}/include ${CMAKE_BINARY_DIR}/generated)
target_include_directories(tpv_borealis PRIVATE ${_B}/src)
target_compile_definitions(tpv_borealis PUBLIC BOREALIS_HAS_SENTRY=0 BOREALIS_HAS_DISCORD=0
  BOREALIS_BUILD_TYPE="$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,None>")
target_link_libraries(tpv_borealis PUBLIC tpv_aurora nod_lite xxHash::xxhash fmt::fmt cxxopts::cxxopts nlohmann_json::nlohmann_json SDL3)
