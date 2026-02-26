add_definitions(-DNAPI_VERSION=6)
add_definitions(-DNODE_RUNTIME=node)
add_definitions(-DBUILDING_NODE_EXTENSION)

# If on Windows use npx.cmd instead of npx
if(WIN32)
  set(NPX_CMD npx.cmd)
else()
  set(NPX_CMD npx)
endif()

execute_process(
  COMMAND ${NPX_CMD} cmake-js print-cmakejs-include
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE CMAKE_JS_INC
  ERROR_QUIET
)
execute_process(
  COMMAND ${NPX_CMD} cmake-js print-cmakejs-lib
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE CMAKE_JS_LIB
  ERROR_QUIET
)
execute_process(
  COMMAND ${NPX_CMD} cmake-js print-cmakejs-src
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE CMAKE_JS_SRC
  ERROR_QUIET
)

# Without this windows breaks with:  Invalid character escape '\P'.
string(REPLACE "\\" "/" CMAKE_JS_INC "${CMAKE_JS_INC}")
string(REPLACE "\\" "/" CMAKE_JS_LIB "${CMAKE_JS_LIB}")
string(REPLACE "\\" "/" CMAKE_JS_SRC "${CMAKE_JS_SRC}")

# cmake-js v8 pollutes stdout with multi-line log messages (INFO, WARN) before
# printing the actual value. The actual value is always the LAST line of output
# (from console.info(command) in bin/cmake-js). Some log messages are multi-line
# (e.g. find-visualstudio emits "INFO ... found at:\n\"C:/path\"\nrun with --verbose")
# so we can't simply strip lines starting with INFO/WARN.
# Instead, extract only the last non-empty line which is the real value.
# First strip trailing whitespace/newlines so the actual value is the last line
# without a trailing \n, then remove all lines that DO end with \n (log lines).
string(STRIP "${CMAKE_JS_INC}" CMAKE_JS_INC)
string(STRIP "${CMAKE_JS_LIB}" CMAKE_JS_LIB)
string(STRIP "${CMAKE_JS_SRC}" CMAKE_JS_SRC)
string(REGEX REPLACE ".*\n" "" CMAKE_JS_INC "${CMAKE_JS_INC}")
string(REGEX REPLACE ".*\n" "" CMAKE_JS_LIB "${CMAKE_JS_LIB}")
string(REGEX REPLACE ".*\n" "" CMAKE_JS_SRC "${CMAKE_JS_SRC}")

# Print CMAKE_JS variables
message(STATUS "CMake.js configurations: LIB=${CMAKE_JS_LIB}, INC=${CMAKE_JS_INC}, SRC=${CMAKE_JS_SRC}")

get_filename_component(NODE_ADDON_API_INCLUDE_PATH ./node_modules/node-addon-api ABSOLUTE)

include_directories(${CMAKE_JS_INC})
include_directories(${NODE_ADDON_API_INCLUDE_PATH})

file(GLOB CPP_SOURCE_FILES ./src_cpp/*)
file(GLOB JS_SOURCE_FILES 
    ./src_js/*.js
    ./src_js/*.mjs
    ./src_js/*.d.ts
)
# Copy all JS/TS files to build directory
file(COPY ${JS_SOURCE_FILES} DESTINATION  "${CMAKE_CURRENT_SOURCE_DIR}/build")

add_library(lbugjs SHARED ${CPP_SOURCE_FILES})
if(CMAKE_JS_SRC)
  target_sources(lbugjs PRIVATE "${CMAKE_JS_SRC}")
endif()
set_target_properties(lbugjs PROPERTIES PREFIX "" SUFFIX ".node")
set_target_properties(lbugjs
  PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build")
if(APPLE)
  target_link_options(lbugjs PRIVATE -undefined dynamic_lookup)
else()
  target_link_options(lbugjs PRIVATE -Wl,--export-dynamic)
endif()
target_link_libraries(lbugjs PRIVATE lbug ${CMAKE_JS_LIB})
