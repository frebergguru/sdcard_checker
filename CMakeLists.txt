cmake_minimum_required(VERSION 3.16)
project(sdcheck C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# ---- core library (no UI dependencies) ----
add_library(sdcheck_core STATIC
    src/core/pattern.c
    src/core/util.c
    src/core/device.c
    src/core/device_guard.c
    src/core/cardinfo.c
    src/core/filemode.c
    src/core/devmode.c
    src/core/format.c
    src/core/auto.c
)
target_include_directories(sdcheck_core PUBLIC include PRIVATE src/core)

# ---- CLI front-end (always built) ----
add_executable(sdcheck src/cli/main.c)
target_include_directories(sdcheck PRIVATE src/core)
target_link_libraries(sdcheck PRIVATE sdcheck_core)

# ---- GUI front-end (built only if GTK4 is available) ----
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GTK4 QUIET gtk4)
    if(GTK4_FOUND)
        add_executable(sdcheck-gui src/gui/main.c)
        target_include_directories(sdcheck-gui PRIVATE ${GTK4_INCLUDE_DIRS})
        target_compile_options(sdcheck-gui PRIVATE ${GTK4_CFLAGS_OTHER})
        target_link_libraries(sdcheck-gui PRIVATE sdcheck_core ${GTK4_LIBRARIES})
        target_link_directories(sdcheck-gui PRIVATE ${GTK4_LIBRARY_DIRS})
        message(STATUS "GTK4 found — building sdcheck-gui")
    else()
        message(STATUS "GTK4 not found — skipping GUI (CLI still builds)")
    endif()

    # ---- Tier 3: privileged D-Bus mechanism (needs GIO + polkit-gobject) ----
    pkg_check_modules(GIO QUIET gio-2.0)
    pkg_check_modules(POLKIT QUIET polkit-gobject-1)
    if(GIO_FOUND AND POLKIT_FOUND)
        add_executable(sdcheck-mechanism src/mechanism/main.c)
        target_include_directories(sdcheck-mechanism PRIVATE ${GIO_INCLUDE_DIRS} ${POLKIT_INCLUDE_DIRS})
        target_compile_options(sdcheck-mechanism PRIVATE ${GIO_CFLAGS_OTHER} ${POLKIT_CFLAGS_OTHER})
        target_link_libraries(sdcheck-mechanism PRIVATE sdcheck_core ${GIO_LIBRARIES} ${POLKIT_LIBRARIES})
        target_link_directories(sdcheck-mechanism PRIVATE ${GIO_LIBRARY_DIRS} ${POLKIT_LIBRARY_DIRS})
        set(SDCHECK_HAVE_MECHANISM ON)
        message(STATUS "GIO + polkit found — building sdcheck-mechanism (Tier 3)")
    else()
        message(STATUS "GIO/polkit-gobject not found — skipping D-Bus mechanism (GUI uses pkexec)")
    endif()
endif()

# ---- install ----
include(GNUInstallDirs)

# The CLI binary doubles as the privileged helper the GUI invokes via pkexec.
install(TARGETS sdcheck RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(TARGET sdcheck-gui)
    install(TARGETS sdcheck-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # The GUI resolves the helper as the `sdcheck` binary sitting next to it, so
    # both land in the same bindir; the polkit rule matches that absolute path.
    set(SDCHECK_HELPER_PATH "${CMAKE_INSTALL_FULL_BINDIR}/sdcheck")
    configure_file(dist/49-sdcheck.rules.in
                   "${CMAKE_CURRENT_BINARY_DIR}/49-sdcheck.rules" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/49-sdcheck.rules"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/polkit-1/rules.d")
    message(STATUS "polkit rule will install to "
            "${CMAKE_INSTALL_FULL_DATADIR}/polkit-1/rules.d/49-sdcheck.rules")

    # Desktop entry + app icon (named after the application ID so GTK and the
    # shell resolve the window/taskbar icon automatically).
    install(FILES dist/org.sdcheck.gui.desktop
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES dist/org.sdcheck.gui.svg
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
endif()

# ---- Tier 3 install: mechanism binary + polkit action + D-Bus config ----
if(SDCHECK_HAVE_MECHANISM)
    install(TARGETS sdcheck-mechanism
            RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/sdcheck")

    set(SDCHECK_MECHANISM_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/sdcheck/sdcheck-mechanism")
    configure_file(dist/org.sdcheck.Mechanism.service.in
                   "${CMAKE_CURRENT_BINARY_DIR}/org.sdcheck.Mechanism.service" @ONLY)

    # polkit action (branded auth message)
    install(FILES dist/org.sdcheck.policy
            DESTINATION "${CMAKE_INSTALL_DATADIR}/polkit-1/actions")
    # D-Bus system bus policy + on-demand activation
    install(FILES dist/org.sdcheck.Mechanism.conf
            DESTINATION "${CMAKE_INSTALL_DATADIR}/dbus-1/system.d")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/org.sdcheck.Mechanism.service"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/dbus-1/system-services")
    message(STATUS "Tier 3 mechanism installs to ${SDCHECK_MECHANISM_PATH}")
endif()

# ---- uninstall target ----
# Removes whatever the last `cmake --install` recorded in install_manifest.txt.
#   sudo cmake --build build --target uninstall
if(NOT TARGET uninstall)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake" @ONLY)
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        COMMENT "Uninstalling files listed in install_manifest.txt")
endif()
