# The FxPlug 4 build, for Final Cut Pro and Motion.
#
# The pattern is settled in resolume-luma-keyer/docs/FXPLUG-PORT.md — read that
# before changing anything structural here. Two products, because FxPlug demands
# both:
#
#   Stoatworks Porthole.app          the wrapper - an empty app whose only job is
#     Contents/                      to exist in /Applications so that the system
#       Frameworks/                  registers the service inside it
#       PlugIns/
#         Porthole.pluginkit         the XPC service that actually renders
#
# Note .pluginkit and NOT .fxplug: with .fxplug this builds, signs and verifies
# perfectly and then silently never registers.

set(FXPLUG_SDK "/Library/Developer/SDKs/FxPlug.sdk"
    CACHE PATH "Apple's FxPlug SDK (a login-gated download; not redistributable)")
set(FXPLUG_FRAMEWORKS "/Library/Developer/Frameworks"
    CACHE PATH "Runtime FxPlug.framework and PluginManager.framework")

if(NOT EXISTS "${FXPLUG_SDK}")
    message(FATAL_ERROR
        "FxPlug SDK not found at ${FXPLUG_SDK}.\n"
        "It is a login-gated download from developer.apple.com and cannot be "
        "vendored into this repo, so CI cannot build this target.")
endif()

set(FXPLUG_APP_NAME "Stoatworks Porthole")
set(FXPLUG_SERVICE_NAME "Porthole")

# ---------------------------------------------------------------- XPC service

# Projection.cpp is compiled straight in rather than linking porthole_core,
# which is an OBJECT library carrying the FFGL SDK and OpenGL with it. The XPC
# service should link as little as possible. Same choice the OpenFX build makes.
add_executable(PortholeFxPlug
    PortholePlugIn.mm
    main.mm
    ${CMAKE_SOURCE_DIR}/source/Projection.cpp
)

set_target_properties(PortholeFxPlug PROPERTIES OUTPUT_NAME "${FXPLUG_SERVICE_NAME}")

target_include_directories(PortholeFxPlug PRIVATE "${CMAKE_SOURCE_DIR}/source")

# The -F path is passed unquoted on purpose: CMake escapes embedded quotes into
# the flag and clang then looks for a directory whose name starts with a quote.
target_compile_options(PortholeFxPlug PRIVATE
    -F${FXPLUG_SDK}/Library/Frameworks
    -fobjc-arc
)
target_link_options(PortholeFxPlug PRIVATE
    -F${FXPLUG_SDK}/Library/Frameworks
    #   .app/Contents/PlugIns/Porthole.pluginkit/Contents/MacOS/  ->  .app/Contents/Frameworks
    "-Wl,-rpath,@loader_path/../../../../Frameworks"
)
# Each "-framework X" is ONE quoted item on purpose. Listed as separate options
# CMake de-duplicates the repeated -framework flag and the linker is left with a
# bare framework name it reads as a missing file.
target_link_libraries(PortholeFxPlug PRIVATE
    "-framework FxPlug"
    "-framework PluginManager"
    "-framework Foundation"
    "-framework AppKit"
    "-framework CoreMedia"
    "-framework IOSurface"
)

configure_file(Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/ServiceInfo.plist" @ONLY)
configure_file(wrapper/Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/WrapperInfo.plist" @ONLY)

# ------------------------------------------------------------- wrapper binary

add_executable(PortholeFxPlugWrapper wrapper/main.mm)
set_target_properties(PortholeFxPlugWrapper PROPERTIES OUTPUT_NAME "${FXPLUG_APP_NAME}")
target_compile_options(PortholeFxPlugWrapper PRIVATE -fobjc-arc)
target_link_libraries(PortholeFxPlugWrapper PRIVATE "-framework Cocoa")

# --------------------------------------------------------------------- tests
#
# Host-free: drives PortholeTile.h directly, so it needs neither the SDK nor a
# running Final Cut Pro.

add_executable(porthole-tiletest
    test/tiletest.cpp
    ${CMAKE_SOURCE_DIR}/source/Projection.cpp
)
target_include_directories(porthole-tiletest PRIVATE "${CMAKE_SOURCE_DIR}/source")
enable_testing()
add_test(NAME tiletest COMMAND porthole-tiletest)

# ------------------------------------------------------------------ assembly

set(FXPLUG_APP "${CMAKE_BINARY_DIR}/${FXPLUG_APP_NAME}.app")
set(FXPLUG_SERVICE "${FXPLUG_APP}/Contents/PlugIns/${FXPLUG_SERVICE_NAME}.pluginkit")

add_custom_target(PortholeFxPlugBundle ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${FXPLUG_APP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FXPLUG_APP}/Contents/MacOS"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FXPLUG_APP}/Contents/Frameworks"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FXPLUG_SERVICE}/Contents/MacOS"

    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_BINARY_DIR}/WrapperInfo.plist" "${FXPLUG_APP}/Contents/Info.plist"
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:PortholeFxPlugWrapper>" "${FXPLUG_APP}/Contents/MacOS/"

    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_BINARY_DIR}/ServiceInfo.plist" "${FXPLUG_SERVICE}/Contents/Info.plist"
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:PortholeFxPlug>" "${FXPLUG_SERVICE}/Contents/MacOS/"

    # Embedded rather than referenced: /Library/Developer/Frameworks is part of
    # the SDK install and is absent on a user's machine. Headers are excluded
    # because shipping Apple's headers is not ours to do.
    COMMAND rsync --archive --links --exclude=Headers --exclude=PrivateHeaders
        "${FXPLUG_FRAMEWORKS}/FxPlug.framework" "${FXPLUG_APP}/Contents/Frameworks/"
    COMMAND rsync --archive --links --exclude=Headers --exclude=PrivateHeaders
        "${FXPLUG_FRAMEWORKS}/PluginManager.framework" "${FXPLUG_APP}/Contents/Frameworks/"

    DEPENDS PortholeFxPlug PortholeFxPlugWrapper
    COMMENT "Assembling ${FXPLUG_APP_NAME}.app"
    VERBATIM
)
