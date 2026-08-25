cmake_minimum_required(VERSION 3.15)

project(Porthole
    VERSION 1.0.2
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Universal build by default so the plugin loads in both native Apple Silicon
# and Intel builds of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64
# for a faster single-arch dev build.
#
# This must sit immediately after project() and before any target is defined.
# CMake latches the architecture list when the first target is created, and
# setting it later is silently ignored -- you get an arm64-only binary that the
# build log calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and Resolume's plugin interface
# is still OpenGL, so every SDK call warns. Silencing it is not ignoring a
# problem -- there is no alternative API to move to here.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

option(PORTHOLE_BUILD_TOOLS "Build the offline render and measurement harness" ON)

# The FFGL plugin, the FFGL SDK submodule and GLEW are one dependency group,
# and the OpenFX plugin belongs to none of it. Turning this OFF builds the OFX
# plugin alone, with no GL loader anywhere in the configure -- which is what
# the Linux job does, because the FFGL SDK has no Linux path, and GLEW would
# otherwise have to be installed to build something that never calls it.
option(PORTHOLE_BUILD_FFGL "Build the FFGL plugin (needs the FFGL SDK and, off macOS, GLEW)" ON)

if(PORTHOLE_BUILD_FFGL)
# ---------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule under external/.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Everything the plugin is, as an OBJECT library so both the bundle and the
# offline harness are built from exactly the same code.
#
# OBJECT and not STATIC on purpose. CFFGLPluginInfo registers itself from a
# file-scope constructor in Porthole.cpp and nothing ever references that object
# by name, so in an archive the linker is entitled to drop the whole translation
# unit -- and the result is a bundle that loads, exports plugMain, and reports
# that it contains no plugins.
# ---------------------------------------------------------------------------
add_library(porthole_core OBJECT
    source/Diag.h
    source/Diag.cpp
    source/Projection.h
    source/Projection.cpp
    source/Shaders.h
    source/Shaders.cpp
    source/Porthole.h
    source/Porthole.cpp
)

target_include_directories(porthole_core PUBLIC source)
target_link_libraries(porthole_core PUBLIC ffgl::sdk)
target_compile_definitions(porthole_core PUBLIC PORTHOLE_VERSION="${PROJECT_VERSION}")
set_target_properties(porthole_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# On Windows/Linux the SDK headers we include pull in <GL/glew.h>. The SDK links
# GLEW privately, so consumers need it too for the include path (and for the
# GLEW_STATIC define the static vcpkg triplet propagates). macOS uses the system
# OpenGL framework and needs none of this.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(porthole_core PUBLIC GLEW::GLEW)
endif()

# ---------------------------------------------------------------------------
# The plugin itself: a loadable macOS .bundle (MODULE library).
# ---------------------------------------------------------------------------
add_library(Porthole MODULE source/PluginEntry.cpp)
target_link_libraries(Porthole PRIVATE porthole_core)

if(APPLE)
    set_target_properties(Porthole PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        OUTPUT_NAME "Porthole"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Porthole"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.stoatworks.ffgl.porthole"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
    # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
    target_link_libraries(Porthole PRIVATE
        "-framework OpenGL"
        "-framework Carbon"
        "-framework AppKit"
    )
endif()

# ---------------------------------------------------------------------------
# Offline harness. A geometry effect is judged on where pixels went, and that is
# measurable rather than a matter of taste -- so this is not just a previewer,
# it is where the projection maths is actually verified. See tools/phtest.
# ---------------------------------------------------------------------------
if(PORTHOLE_BUILD_TOOLS AND APPLE)
    add_executable(phtest tools/phtest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines here
    # rather than a vendored dependency.
    target_link_libraries(phtest PRIVATE porthole_core z
        "-framework OpenGL"
        "-framework AppKit"
    )
endif()

endif()  # PORTHOLE_BUILD_FFGL

# ---------------------------------------------------------------------------
# The OpenFX build of the same effect, for Resolve/Nuke/Natron/Vegas. CPU
# render against the vendored OFX SDK subset under external/openfx (C headers
# + the official C++ Support library, BSD-3 — see external/openfx/LICENSE.md).
# Links Projection.cpp straight from source: the lens maths has one home.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# The FxPlug 4 build, for Final Cut Pro and Motion (macOS only, and only where
# Apple's SDK is installed - it is a login-gated download that cannot be
# vendored, so this is OFF by default and CI cannot build it).
# Pattern doc: resolume-luma-keyer/docs/FXPLUG-PORT.md
# ---------------------------------------------------------------------------
option(BUILD_FXPLUG "Build the FxPlug plugin for Final Cut Pro and Motion" OFF)

if(BUILD_FXPLUG)
    if(NOT APPLE)
        message(FATAL_ERROR "BUILD_FXPLUG is macOS only.")
    endif()
    add_subdirectory(fxplug)
endif()

option(BUILD_OFX "Build the OpenFX plugin" ON)

if(BUILD_OFX)
    set(OFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/external/openfx")

    add_library(PortholeOFX MODULE
        source/ofx/PortholeOFX.cpp
        source/Projection.cpp
        ${OFX_SDK}/Support/Library/ofxsCore.cpp
        ${OFX_SDK}/Support/Library/ofxsImageEffect.cpp
        ${OFX_SDK}/Support/Library/ofxsInteract.cpp
        ${OFX_SDK}/Support/Library/ofxsLog.cpp
        ${OFX_SDK}/Support/Library/ofxsMultiThread.cpp
        ${OFX_SDK}/Support/Library/ofxsParams.cpp
        ${OFX_SDK}/Support/Library/ofxsProperty.cpp
        ${OFX_SDK}/Support/Library/ofxsPropertyValidation.cpp
    )
    target_include_directories(PortholeOFX PRIVATE
        "${OFX_SDK}/include"
        "${OFX_SDK}/Support/include"
    )
    target_compile_definitions(PortholeOFX PRIVATE
        PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    )

    # An OFX plugin is not a macOS .bundle in the CMake sense: hosts expect the
    # fixed layout <Name>.ofx.bundle/Contents/<arch>/<Name>.ofx, assembled here
    # by hand. "MacOS" is the OFX architecture name for universal binaries.
    set(OFX_BUNDLE_DIR "${CMAKE_BINARY_DIR}/Porthole.ofx.bundle")
    set_target_properties(PortholeOFX PROPERTIES
        PREFIX ""
        SUFFIX ".ofx"
        OUTPUT_NAME "Porthole"
        LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/MacOS"
    )
    if(APPLE)
        configure_file(cmake/InfoOFX.plist.in
            "${OFX_BUNDLE_DIR}/Contents/Info.plist" @ONLY)
    elseif(WIN32)
        set_target_properties(PortholeOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            LIBRARY_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            # MSVC's .exp/.lib for this target; its default directory collides
            # with the FFGL target's, which shares the output base name, and a
            # parallel build then races on the file (LNK1104).
            ARCHIVE_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/importlib"
            ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/importlib")
    else()
        set_target_properties(PortholeOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Linux-x86-64")
    endif()
endif()

# ---------------------------------------------------------------------------
# `cmake --install` drops the bundle straight into the user's Resolume plugin
# folder. Override with -DCMAKE_INSTALL_PREFIX=...
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    # honour an explicit prefix
elseif(APPLE)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()

if(PORTHOLE_BUILD_FFGL)
install(TARGETS Porthole
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)
endif()  # PORTHOLE_BUILD_FFGL
