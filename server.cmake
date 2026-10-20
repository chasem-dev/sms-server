# Included by sms-pc-port's CMakeLists.txt when it is configured with
# -DSMS_SERVER_DIR=<this checkout> (build.sh does that with SMS_SERVER_DIR):
# the game becomes sms-server, with this repository's sources built in.
#
# The game calls sms_server_init first thing (platform/port_runtime.cpp, under
# SMS_SERVER_BUILD); everything else here is the server's own: server.ini,
# and the room that lists it with the room server (libcurl's WebSocket, over
# Schannel on Windows and OpenSSL elsewhere).

set(_sms_server_dir ${CMAKE_CURRENT_LIST_DIR})
file(READ ${_sms_server_dir}/VERSION _sms_server_version)
string(STRIP "${_sms_server_version}" _sms_server_version)

# --- libcurl, built in: HTTP and WebSockets only
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW) # the archive's files dated when extracted
endif()
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(PICKY_COMPILER OFF CACHE BOOL "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_WEBSOCKETS OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE STRING "" FORCE)
set(CURL_BROTLI OFF CACHE STRING "" FORCE)
set(CURL_ZSTD OFF CACHE STRING "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(USE_WIN32_IDN OFF CACHE BOOL "" FORCE)
set(USE_APPLE_IDN OFF CACHE BOOL "" FORCE)
set(CURL_USE_PKGCONFIG OFF CACHE BOOL "" FORCE)
set(ENABLE_UNIX_SOCKETS OFF CACHE BOOL "" FORCE)
set(CURL_CA_BUNDLE "none" CACHE STRING "" FORCE) # found when the server runs (src/ws.cpp)
set(CURL_CA_PATH "none" CACHE STRING "" FORCE)
if(WIN32)
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
  set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
else()
  set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(sms_server_curl
  URL https://github.com/curl/curl/releases/download/curl-8_23_0/curl-8.23.0.tar.xz
  URL_HASH SHA256=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(sms_server_curl)

# --- the server
add_library(sms_server STATIC
  ${_sms_server_dir}/src/server.cpp
  ${_sms_server_dir}/src/config.cpp
  ${_sms_server_dir}/src/room.cpp
  ${_sms_server_dir}/src/ws.cpp
  ${_sms_server_dir}/src/udp.cpp
  ${_sms_server_dir}/src/packets.cpp
  ${_sms_server_dir}/src/json.cpp)
set_target_properties(sms_server PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
# The game's version, as SMS Launcher names it ("2026.10.18.1", or
# "2026.10.20-beta.8cedf48"): SMS_GAME_VERSION in the environment of the build
# (./build.sh, CI), told to the game when the server starts. Empty: the game's
# own (its last commit's date).
set(_sms_server_game_version "$ENV{SMS_GAME_VERSION}")
target_compile_definitions(sms_server PRIVATE SMS_SERVER_VERSION="${_sms_server_version}"
  SMS_SERVER_GAME_VERSION="${_sms_server_game_version}")
target_link_libraries(sms_server PRIVATE CURL::libcurl_static)
if(WIN32)
  target_link_libraries(sms_server PRIVATE ws2_32 iphlpapi)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(sms_server PRIVATE Threads::Threads)
endif()

target_compile_definitions(sms PRIVATE SMS_SERVER_BUILD)
target_link_libraries(sms PRIVATE sms_server)
set_target_properties(sms PROPERTIES OUTPUT_NAME sms-server)
message(STATUS "sms-server ${_sms_server_version}: building the game as a dedicated server")
