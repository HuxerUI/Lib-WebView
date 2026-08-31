cmake_minimum_required(VERSION 3.20)

if (HUXERUI_PROJECT_PLAN_OUTPUT)
    get_filename_component(HUXERUI_PROJECT_PLAN_DIRECTORY
            "${HUXERUI_PROJECT_PLAN_OUTPUT}"
            DIRECTORY
    )
    file(MAKE_DIRECTORY "${HUXERUI_PROJECT_PLAN_DIRECTORY}")
    file(WRITE "${HUXERUI_PROJECT_PLAN_OUTPUT}" [=[
{
  "schema": 1,
  "kind": "library",
  "name": "WebView",
  "id": "org.huxerui.lib.webview",
  "target": "webview"
}
]=])
endif ()
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

project(web_view VERSION 0.1.0 LANGUAGES NONE)

enable_language(CXX)
if (APPLE AND NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    enable_language(OBJCXX)
endif ()

if (NOT TARGET HuxerUI::huxerui)
    set(HUXERUI_HOME "$ENV{HUXERUI_HOME}" CACHE PATH "HuxerUI SDK or source directory")
    if (HUXERUI_HOME AND EXISTS "${HUXERUI_HOME}/CMakeLists.txt"
            AND EXISTS "${HUXERUI_HOME}/include/huxerui/huxerui.h")
        set(HUXERUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(HUXERUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        if (ANDROID)
            set(HUXERUI_BUILD_SHARED ON CACHE BOOL "" FORCE)
            set(HUXERUI_BUILD_STATIC OFF CACHE BOOL "" FORCE)
        else ()
            set(HUXERUI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
            set(HUXERUI_BUILD_STATIC ON CACHE BOOL "" FORCE)
        endif ()
        add_subdirectory("${HUXERUI_HOME}" "${CMAKE_BINARY_DIR}/huxerui-sdk" EXCLUDE_FROM_ALL)
    else ()
        if (HUXERUI_HOME)
            find_package(HuxerUI CONFIG REQUIRED
                    PATHS "${HUXERUI_HOME}"
                    NO_DEFAULT_PATH
                    NO_CMAKE_FIND_ROOT_PATH
            )
        else ()
            find_package(HuxerUI CONFIG REQUIRED)
        endif ()
    endif ()
endif ()

file(GLOB_RECURSE LIBRARY_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
if (ANDROID)
    file(GLOB_RECURSE LIBRARY_ANDROID_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_ANDROID_SOURCE_FILES})
endif ()
if (WIN32)
    file(GLOB_RECURSE LIBRARY_WINDOWS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WINDOWS_SOURCE_FILES})
endif ()
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(GLOB_RECURSE LIBRARY_LINUX_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_LINUX_SOURCE_FILES})
endif ()
if (EMSCRIPTEN)
    file(GLOB_RECURSE LIBRARY_WEB_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WEB_SOURCE_FILES})
endif ()
if (APPLE AND NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(LIBRARY_APPLE_SOURCE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/src")
    else ()
        set(LIBRARY_APPLE_SOURCE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src")
    endif ()
    file(GLOB_RECURSE LIBRARY_APPLE_PLATFORM_SOURCE_FILES CONFIGURE_DEPENDS
            "${LIBRARY_APPLE_SOURCE_DIRECTORY}/*.mm"
    )
    set_source_files_properties(${LIBRARY_APPLE_PLATFORM_SOURCE_FILES} PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_APPLE_PLATFORM_SOURCE_FILES})
endif ()

huxerui_add_library(webview
        SOURCES
            ${LIBRARY_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            huxerui
)

target_include_directories(webview PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)
target_include_directories(webview PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

if (APPLE)
    target_link_libraries(webview PRIVATE "-framework WebKit")
endif ()
if (WIN32)
    set(WEBVIEW2_SDK_ROOT "$ENV{WEBVIEW2_SDK_ROOT}" CACHE PATH "Microsoft.Web.WebView2 NuGet package root")
    find_path(WEBVIEW2_INCLUDE_DIRECTORY
            NAMES WebView2.h
            HINTS "${WEBVIEW2_SDK_ROOT}"
            PATH_SUFFIXES build/native/include
    )
    if (CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
        set(WEBVIEW2_ARCHITECTURE arm64)
    elseif (CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
        set(WEBVIEW2_ARCHITECTURE x86)
    else ()
        set(WEBVIEW2_ARCHITECTURE x64)
    endif ()
    find_library(WEBVIEW2_LOADER_LIBRARY
            NAMES WebView2LoaderStatic
            HINTS "${WEBVIEW2_SDK_ROOT}"
            PATH_SUFFIXES "build/native/${WEBVIEW2_ARCHITECTURE}"
    )
    if (NOT WEBVIEW2_INCLUDE_DIRECTORY OR NOT WEBVIEW2_LOADER_LIBRARY)
        message(FATAL_ERROR
                "WebView requires Microsoft.Web.WebView2; set WEBVIEW2_SDK_ROOT to the extracted NuGet package root"
        )
    endif ()
    target_include_directories(webview PRIVATE "${WEBVIEW2_INCLUDE_DIRECTORY}")
    target_compile_definitions(webview PRIVATE WEBVIEW2_STATIC)
    target_link_libraries(webview PRIVATE "${WEBVIEW2_LOADER_LIBRARY}" Shlwapi)
endif ()

add_library(HuxerUI::WebView ALIAS webview)
