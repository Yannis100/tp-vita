# Import Dusklight's source lists (files.cmake) without letting it create targets.
# files.cmake uses paths relative to the Dusklight root and declares the JSystem
# libraries with add_library(); capture those sources instead.

set(TPV_JSYSTEM_FILES "")
macro(add_library _name)
  if("${_name}" MATCHES "^JSystem_")
    set(_tpv_srcs ${ARGN})
    list(REMOVE_ITEM _tpv_srcs STATIC)
    list(APPEND TPV_JSYSTEM_FILES ${_tpv_srcs})
  else()
    _add_library(${_name} ${ARGN})
  endif()
endmacro()

include(${TP_DEPS}/files.cmake)

foreach(_v DOLZEL_FILES Z2AUDIOLIB_FILES SSYSTEM_FILES REL_FILES DOLPHIN_FILES DUSK_FILES TPV_JSYSTEM_FILES)
  list(FILTER ${_v} EXCLUDE REGEX "\\.(h|hpp)$")
  list(TRANSFORM ${_v} PREPEND "${TP_DEPS}/")
endforeach()
