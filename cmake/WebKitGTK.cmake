include_guard(GLOBAL)

function(huxerui_link_webkitgtk target_name)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HUXERUI_WEBKITGTK QUIET IMPORTED_TARGET webkitgtk-6.0>=2.40)
    if (HUXERUI_WEBKITGTK_FOUND)
        target_link_libraries(${target_name} PRIVATE PkgConfig::HUXERUI_WEBKITGTK)
        return()
    endif ()

    # WebKit uses CMAKE_SOURCE_DIR internally and must be configured as a separate project.
    include(ExternalProject)
    if (POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif ()
    find_program(HUXERUI_WEBKITGTK_NINJA NAMES ninja ninja-build REQUIRED)
    pkg_check_modules(HUXERUI_WEBKITGTK_DEPS REQUIRED IMPORTED_TARGET
            gtk4>=4.6 libsoup-3.0>=3.0
    )

    set(webkit_root "${CMAKE_CURRENT_BINARY_DIR}/_deps/webkitgtk")
    set(webkit_install "${webkit_root}/install")
    set(webkit_library "${webkit_install}/lib/libwebkitgtk-6.0.so")
    set(javascriptcore_library "${webkit_install}/lib/libjavascriptcoregtk-6.0.so")
    file(MAKE_DIRECTORY "${webkit_install}/include/webkitgtk-6.0")

    set(webkit_cmake_args
            -DPORT=GTK
            -DUSE_GTK4=ON
            -DCMAKE_BUILD_TYPE=Release
            "-DCMAKE_INSTALL_PREFIX:PATH=${webkit_install}"
            -DCMAKE_INSTALL_LIBDIR=lib
            "-DCMAKE_INSTALL_RPATH:STRING=${webkit_install}/lib"
            "-DCMAKE_MAKE_PROGRAM:FILEPATH=${HUXERUI_WEBKITGTK_NINJA}"
            "-DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}"
            -DENABLE_DOCUMENTATION=OFF
            -DENABLE_INTROSPECTION=OFF
            -DENABLE_MINIBROWSER=OFF
            -DENABLE_WEBDRIVER=OFF
            -DENABLE_API_TESTS=OFF
    )
    foreach(variable CMAKE_C_COMPILER CMAKE_TOOLCHAIN_FILE CMAKE_SYSROOT)
        if (${variable})
            list(APPEND webkit_cmake_args "-D${variable}:FILEPATH=${${variable}}")
        endif ()
    endforeach()

    message(STATUS "HuxerUI WebView: WebKitGTK 6.0 not found; downloading and building WebKitGTK 2.54.2")
    ExternalProject_Add(huxerui_webkitgtk_build
            PREFIX "${webkit_root}"
            URL "https://webkitgtk.org/releases/webkitgtk-2.54.2.tar.xz"
            URL_HASH SHA256=a76345b391bef6211fb5ab6cfab2e86b285d18cda17cbb9ade595f1252b86065
            DOWNLOAD_DIR "${webkit_root}/download"
            BINARY_DIR "${webkit_root}/build"
            CMAKE_GENERATOR Ninja
            CMAKE_ARGS ${webkit_cmake_args}
            BUILD_BYPRODUCTS "${webkit_library}" "${javascriptcore_library}"
            USES_TERMINAL_DOWNLOAD TRUE
            USES_TERMINAL_CONFIGURE TRUE
            USES_TERMINAL_BUILD TRUE
            USES_TERMINAL_INSTALL TRUE
    )

    add_library(huxerui_javascriptcoregtk SHARED IMPORTED)
    set_target_properties(huxerui_javascriptcoregtk PROPERTIES
            IMPORTED_LOCATION "${javascriptcore_library}"
    )
    add_dependencies(huxerui_javascriptcoregtk huxerui_webkitgtk_build)

    add_library(huxerui_webkitgtk SHARED IMPORTED)
    set_target_properties(huxerui_webkitgtk PROPERTIES
            IMPORTED_LOCATION "${webkit_library}"
            INTERFACE_INCLUDE_DIRECTORIES "${webkit_install}/include/webkitgtk-6.0"
            INTERFACE_LINK_LIBRARIES "huxerui_javascriptcoregtk;PkgConfig::HUXERUI_WEBKITGTK_DEPS"
    )
    add_dependencies(huxerui_webkitgtk huxerui_webkitgtk_build)
    add_dependencies(${target_name} huxerui_webkitgtk_build)
    target_link_libraries(${target_name} PRIVATE huxerui_webkitgtk)
endfunction()
