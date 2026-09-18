cmake_minimum_required(VERSION 3.15)
project(thecoolestcompiler CXX C)

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
# LLVM/Clang 18 headers require C++17 (std::optional, is_integral_v, etc. in
# llvm/ADT). ROSE 2.14 (pre-C++17) compiles fine as a strict superset under
# -std=c++17; verified directly against every src/*.cpp in this project.
# _GLIBCXX_USE_CXX11_ABI=1 (the std::string ABI ROSE was built against) is
# independent of -std= and still applies below.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Build type default
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
      "Build type: Debug, Release, RelWithDebInfo, MinSizeRel" FORCE)
endif()

# ---------------------------------------------------------------------------
# CMake module path — allow project-local Find modules if ever needed
# ---------------------------------------------------------------------------
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# ROSE
# ---------------------------------------------------------------------------
# RoseConfig.cmake lives at <prefix>/lib/cmake/Rose/
if(NOT DEFINED Rose_DIR)
  set(Rose_DIR "/config/workspace/rose/install/lib/cmake/Rose"
      CACHE PATH "Path to RoseConfig.cmake directory")
endif()

find_package(Rose REQUIRED)

# ---------------------------------------------------------------------------
# LLVM / Clang
# ---------------------------------------------------------------------------
# The LLVM/Clang *dev* packages (headers + cmake configs) may or may not be
# present.  We try CONFIG mode first; if that fails we fall back to a manual
# locate of the runtime libraries so the build file remains usable even when
# only the runtime .so files are installed.

# Hint: common versioned install prefixes on Debian/Ubuntu, plus a locally
# extracted copy of the llvm-18/libclang-18-dev .deb packages (no root
# available in this environment, so they were unpacked with dpkg-deb -x
# instead of installed system-wide).
list(APPEND CMAKE_PREFIX_PATH
  "/config/workspace/toolchain/usr/lib/llvm-18"
  "/usr/lib/llvm-18"
  "/usr/lib/llvm-17"
  "/usr/lib/llvm-16"
  "/usr/lib/llvm-15"
  "/usr/lib/llvm-14"
)

# --- LLVM ---
find_package(LLVM CONFIG QUIET)
if(LLVM_FOUND)
  message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} via CMake config")
  message(STATUS "  LLVM include dirs: ${LLVM_INCLUDE_DIRS}")
  # Map LLVM component libraries to targets
  llvm_map_components_to_libnames(_llvm_libs
    core support irreader bitreader bitwriter analysis
    transformutils scalaropts vectorize instcombine
    target aarch64 x86 aarch64codegen x86codegen
    aarch64asmparser x86asmparser
    mc mcparser option
  )
else()
  message(STATUS "LLVM CMake config not found — falling back to manual detection")
  # Attempt to find the monolithic LLVM shared library
  find_library(LLVM_LIBRARY
    NAMES LLVM-18 LLVM-17 LLVM-16 LLVM-15 LLVM LLVM-18.1
    HINTS "/config/workspace/toolchain/usr/lib/llvm-18/lib" "/usr/lib/llvm-18/lib" "/config/workspace/toolchain/usr/lib/aarch64-linux-gnu" "/usr/lib/aarch64-linux-gnu" "/usr/lib/x86_64-linux-gnu"
  )
  if(LLVM_LIBRARY)
    message(STATUS "  Found LLVM library: ${LLVM_LIBRARY}")
  else()
    message(WARNING "LLVM library not found — translator may fail to link")
  endif()

  # Try to locate headers
  find_path(LLVM_INCLUDE_DIRS
    NAMES llvm/IR/Module.h llvm/Support/CommandLine.h
    HINTS
      "/config/workspace/toolchain/usr/lib/llvm-18/include" "/usr/lib/llvm-18/include"
      "/usr/include/llvm-18"
      "/usr/include/llvm-c"
  )
  set(_llvm_libs ${LLVM_LIBRARY})
endif()

# --- Clang ---
find_package(Clang CONFIG QUIET)
if(Clang_FOUND OR TARGET clangTooling)
  message(STATUS "Found Clang via CMake config")
  # CLANG_INCLUDE_DIRS is set by ClangConfig.cmake
else()
  message(STATUS "Clang CMake config not found — falling back to manual detection")
  # Locate individual Clang libraries
  foreach(_lib
    clangTooling clangFrontend clangAST clangBasic clangLex
    clangDriver clangSerialization clangSema clangAnalysis
    clangEdit clangParse clangRewrite clangASTMatchers
    clangToolingCore clangSupport clangAPINotes
    clang-cpp  # monolithic Clang library (Debian)
  )
    find_library(_clang_lib_${_lib}
      NAMES ${_lib}
      HINTS
        "/config/workspace/toolchain/usr/lib/llvm-18/lib" "/usr/lib/llvm-18/lib"
        "/config/workspace/toolchain/usr/lib/aarch64-linux-gnu" "/usr/lib/aarch64-linux-gnu"
        "/usr/lib/x86_64-linux-gnu"
    )
    if(_clang_lib_${_lib})
      list(APPEND _clang_manual_libs ${_clang_lib_${_lib}})
    endif()
    unset(_clang_lib_${_lib} CACHE)
  endforeach()

  # If individual libs not found, try the monolithic clang-cpp
  if(NOT _clang_manual_libs)
    find_library(_clang_cpp_lib
      NAMES clang-cpp clang-18 clang
      HINTS
        "/config/workspace/toolchain/usr/lib/llvm-18/lib" "/usr/lib/llvm-18/lib"
        "/config/workspace/toolchain/usr/lib/aarch64-linux-gnu" "/usr/lib/aarch64-linux-gnu"
        "/usr/lib/x86_64-linux-gnu"
    )
    if(_clang_cpp_lib)
      list(APPEND _clang_manual_libs ${_clang_cpp_lib})
      message(STATUS "  Using monolithic Clang library: ${_clang_cpp_lib}")
    else()
      message(WARNING "Clang libraries not found — translator may fail to link")
    endif()
    unset(_clang_cpp_lib CACHE)
  endif()

  # Locate Clang headers
  find_path(CLANG_INCLUDE_DIRS
    NAMES clang/AST/ASTContext.h clang/Tooling/Tooling.h
    HINTS
      "/config/workspace/toolchain/usr/lib/llvm-18/include" "/usr/lib/llvm-18/include"
      "/usr/include/clang-18"
      "/usr/include"
  )
endif()

# ---------------------------------------------------------------------------
# ROSE compile flags (from rose-config.cfg)
# ---------------------------------------------------------------------------
# These are required for binary compatibility with librose.so:
#   -D_GLIBCXX_USE_CXX11_ABI=1   — use the C++11 std::string ABI
#   -Ddisc_union=union            — ROSE internal discriminated-union macro
add_compile_options(
  -D_GLIBCXX_USE_CXX11_ABI=1
  -Ddisc_union=union
)

# ---------------------------------------------------------------------------
# INTERFACE library: compiler_settings
# Bundles all header search paths so both translator and tests share them.
# ---------------------------------------------------------------------------
add_library(compiler_settings INTERFACE)

target_compile_options(compiler_settings INTERFACE
  -D_GLIBCXX_USE_CXX11_ABI=1
  -Ddisc_union=union
)

target_compile_features(compiler_settings INTERFACE cxx_std_17)

if(LLVM_INCLUDE_DIRS)
  target_include_directories(compiler_settings INTERFACE ${LLVM_INCLUDE_DIRS})
endif()
if(CLANG_INCLUDE_DIRS)
  target_include_directories(compiler_settings INTERFACE ${CLANG_INCLUDE_DIRS})
endif()

# ROSE includes are carried transitively by Rose::rose, but add them
# explicitly to catch any edge cases.
target_include_directories(compiler_settings INTERFACE
  "${Rose_INSTALL_PREFIX}/include/rose"
)

# ---------------------------------------------------------------------------
# Profiler runtime library  (plain C, no ROSE/Clang dependency)
# ---------------------------------------------------------------------------
# support/profiler.c is created in Task 6; guard against its absence so this
# file doesn't break earlier tasks.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/support/profiler.c")
  add_library(profiler STATIC support/profiler.c)
  target_include_directories(profiler PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
  )
  set(_profiler_available TRUE)
else()
  message(STATUS "support/profiler.c not found yet — profiler target skipped")
  set(_profiler_available FALSE)
endif()

# ---------------------------------------------------------------------------
# Translator executable
# ---------------------------------------------------------------------------
# Collect source files.  Use CONFIGURE_DEPENDS so CMake re-runs if the glob
# result changes (new .cpp files added by later tasks).
file(GLOB _translator_sources
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

# Expected source files (will be created by later tasks):
#   src/translator.cpp
#   src/ClangBridge.cpp
#   src/RoseBridge.cpp
#   src/InterproceduralAnalysis.cpp
#   src/ParallelismChecker.cpp
#   src/ProfilingInstrumentor.cpp
#   src/GpuSuitabilityEngine.cpp
#   src/OmpCodegen.cpp

if(_translator_sources)
  add_executable(translator ${_translator_sources})

  target_include_directories(translator PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
  )

  target_link_libraries(translator PRIVATE
    compiler_settings
    Rose::rose
  )

  # --- Clang link targets ---
  if(TARGET clangTooling)
    # ClangConfig.cmake was found: use imported targets
    target_link_libraries(translator PRIVATE
      clangTooling
      clangFrontend
      clangAST
      clangBasic
      clangLex
      clangDriver
      clangSerialization
      clangSema
      clangAnalysis
      clangEdit
      clangParse
      clangRewrite
    )
  elseif(_clang_manual_libs)
    # Clang's static libs have circular inter-dependencies (clangSema <->
    # clangFrontend <-> clangAST etc.) — group them so the linker re-scans
    # until all symbols resolve, regardless of the order CMake emits them in.
    target_link_libraries(translator PRIVATE
      -Wl,--start-group ${_clang_manual_libs} -Wl,--end-group
    )
  endif()

  # --- LLVM link targets ---
  if(TARGET LLVM)
    target_link_libraries(translator PRIVATE LLVM)
  elseif(_llvm_libs)
    target_link_libraries(translator PRIVATE
      -Wl,--start-group ${_llvm_libs} -Wl,--end-group
    )
  elseif(LLVM_LIBRARY)
    target_link_libraries(translator PRIVATE ${LLVM_LIBRARY})
  endif()

  # Clang/LLVM static libs pull in libtinfo (terminal capability queries in
  # LLVM Support) but this environment has no libtinfo *-dev* package, only
  # the runtime libtinfo.so.6 with no unversioned `-ltinfo` symlink — so
  # find_library(NAMES tinfo) fails; locate the versioned .so directly.
  find_library(TINFO_LIBRARY
    NAMES tinfo tinfo6
    HINTS "/usr/lib/aarch64-linux-gnu" "/usr/lib/x86_64-linux-gnu"
  )
  if(NOT TINFO_LIBRARY)
    foreach(_tinfo_candidate
      "/usr/lib/aarch64-linux-gnu/libtinfo.so.6"
      "/usr/lib/x86_64-linux-gnu/libtinfo.so.6"
    )
      if(EXISTS "${_tinfo_candidate}")
        set(TINFO_LIBRARY "${_tinfo_candidate}")
      endif()
    endforeach()
  endif()
  if(TINFO_LIBRARY)
    target_link_libraries(translator PRIVATE ${TINFO_LIBRARY})
  else()
    message(WARNING "libtinfo not found — link may fail with undefined terminfo symbols")
  endif()
  target_link_libraries(translator PRIVATE dl z)

  # --- Optional: link profiler ---
  if(_profiler_available)
    target_link_libraries(translator PRIVATE profiler)
  endif()

  # ROSE needs pthread
  find_package(Threads REQUIRED)
  target_link_libraries(translator PRIVATE Threads::Threads)

  # --- Boost libraries required by ROSE public API ---
  # Boost is found transitively via Rose::rose (RoseConfig.cmake calls
  # find_dependency(Boost ...)), but we link explicitly to be safe.
  find_package(Boost 1.83.0 REQUIRED COMPONENTS
    filesystem program_options system
  )
  target_link_libraries(translator PRIVATE
    Boost::filesystem
    Boost::program_options
    Boost::system
  )

  # --- RPATH: embed ROSE lib dir so the binary runs without LD_LIBRARY_PATH ---
  set_target_properties(translator PROPERTIES
    INSTALL_RPATH "${Rose_INSTALL_PREFIX}/lib"
    BUILD_RPATH   "${Rose_INSTALL_PREFIX}/lib"
    INSTALL_RPATH_USE_LINK_PATH TRUE
  )

else()
  message(STATUS
    "No src/*.cpp files found yet — translator target deferred.\n"
    "Run cmake --build again after Task 2+ source files are added.")

  # Create a dummy target so downstream cmake logic (tests, install) doesn't
  # break on missing translator target.
  add_custom_target(translator
    COMMENT "Translator source files not yet present; skipping build"
  )
endif()

# ---------------------------------------------------------------------------
# src/ subdirectory CMakeLists (optional, for future per-module structure)
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")
  add_subdirectory(src)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

# Install translator binary (only if the real executable target was created)
get_target_property(_translator_type translator TYPE)
if(_translator_type STREQUAL "EXECUTABLE")
  install(TARGETS translator
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT runtime
  )
endif()

# Install profiler static library + headers
if(_profiler_available)
  install(TARGETS profiler
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    COMPONENT development
  )
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/support/profiler.h")
    install(FILES support/profiler.h
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/thecoolestcompiler"
      COMPONENT development
    )
  endif()
endif()

# Install src headers (for anyone linking against internal APIs)
install(DIRECTORY src/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/thecoolestcompiler"
  COMPONENT development
  FILES_MATCHING PATTERN "*.h"
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "=== thecoolestcompiler configuration summary ===")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:      ${CMAKE_CXX_STANDARD}")
message(STATUS "  ROSE prefix:       ${Rose_INSTALL_PREFIX}")
message(STATUS "  LLVM found:        ${LLVM_FOUND}")
message(STATUS "  Clang found:       ${Clang_FOUND}")
message(STATUS "  Profiler:          ${_profiler_available}")
message(STATUS "  Translator srcs:   ${_translator_sources}")
message(STATUS "================================================")
