cmake_minimum_required(VERSION 3.14)
project(sluice VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to an optimized build unless told otherwise (also required for
# _FORTIFY_SOURCE, which is a no-op without optimization).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# library to parse JSON
include(FetchContent)
FetchContent_Declare(
  json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

FetchContent_Declare(
  httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.1.tar.gz
)
FetchContent_MakeAvailable(httplib)

add_executable(sluice
  src/main.cpp
  src/config.cpp
  src/gateway.cpp
  src/access.cpp
  src/log_buffer.cpp
  src/dashboard.cpp
  src/drain_snapshot.cpp
  src/master.cpp
  src/log.cpp
  src/logwriter.cpp
)

add_executable(setup
  src/setup.cpp
  src/config.cpp
  src/access.cpp
  src/log.cpp
  src/log_buffer.cpp
)

find_package(Threads REQUIRED)
target_link_libraries(sluice PRIVATE 
  nlohmann_json::nlohmann_json 
  httplib::httplib 
  Threads::Threads
)

target_link_libraries(setup PRIVATE
  nlohmann_json::nlohmann_json
  Threads::Threads
)

# ── Hardening + stripping ───────────────────────────────────────────────────
# Applied to both binaries:
#   -fstack-protector-strong  stack canaries
#   -D_FORTIFY_SOURCE=2       fortified libc calls (needs optimization)
#   -fPIE / -pie              position-independent executable (ASLR)
#   -z relro,-z now           full RELRO (GOT read-only after startup)
#   -z noexecstack            non-executable stack
#   -s (Release only)         strip symbols from the shipped binaries
foreach(tgt sluice setup)
  target_compile_options(${tgt} PRIVATE
    -Wall -Wextra
    -fstack-protector-strong
    -fPIE
    $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>)
  target_link_options(${tgt} PRIVATE
    -pie
    -Wl,-z,relro,-z,now
    -Wl,-z,noexecstack
    $<$<CONFIG:Release>:-s>)
endforeach()

# ── Install rules ───────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS sluice setup RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Service files (absolute destinations so they land correctly regardless of prefix).
install(FILES packaging/config.json      DESTINATION /etc/sluice)
install(FILES packaging/sluice.service   DESTINATION /lib/systemd/system)
install(FILES packaging/logrotate        DESTINATION /etc/logrotate.d RENAME sluice)

# ── .deb packaging (cpack -G DEB from the build dir) ────────────────────────
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "sluice")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Sluice — epoll-driven Layer-4 TCP reverse proxy")
# TODO: set a real maintainer before publishing the package.
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "SystematicChaos <radhan.r.dandekar@gmail.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)  # auto libc/libstdc++ deps (needs dpkg-dev)
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "logrotate")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_STRIP_FILES TRUE)
# Maintainer scripts (create/remove the sluice user, enable the service) and the
# conffiles list (so dpkg preserves an admin-edited /etc/sluice/config.json).
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
  "${CMAKE_CURRENT_SOURCE_DIR}/packaging/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/prerm;${CMAKE_CURRENT_SOURCE_DIR}/packaging/postrm;${CMAKE_CURRENT_SOURCE_DIR}/packaging/conffiles")
include(CPack)