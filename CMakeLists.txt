cmake_minimum_required(VERSION 3.20)

project(web_view VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
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
if (APPLE AND NOT IOS)
    file(GLOB_RECURSE LIBRARY_MACOS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cxx"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.mm"
    )
    set_source_files_properties(${LIBRARY_MACOS_SOURCE_FILES} PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_MACOS_SOURCE_FILES})
endif ()
if (APPLE AND IOS)
    file(GLOB_RECURSE LIBRARY_IOS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/src/*.mm"
    )
    set_source_files_properties(${LIBRARY_IOS_SOURCE_FILES} PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_IOS_SOURCE_FILES})
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
    if (NOT WEBVIEW2_SDK_ROOT)
        include(FetchContent)
        if (POLICY CMP0135)
            cmake_policy(SET CMP0135 NEW)
        endif ()
        FetchContent_Declare(huxerui_webview2_sdk
                URL "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4191.47/microsoft.web.webview2.1.0.4191.47.nupkg"
                URL_HASH SHA256=F492BBF547D0DA329553B6727435B677579B1E9F91CC9E4A1AD029366D5F23D0
        )
        FetchContent_MakeAvailable(huxerui_webview2_sdk)
        set(WEBVIEW2_SDK_ROOT "${huxerui_webview2_sdk_SOURCE_DIR}")
    endif ()
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
                "HuxerUI WebView could not find Microsoft.Web.WebView2 under WEBVIEW2_SDK_ROOT"
        )
    endif ()
    target_include_directories(webview PRIVATE "${WEBVIEW2_INCLUDE_DIRECTORY}")
    target_compile_definitions(webview PRIVATE NOMINMAX UNICODE _UNICODE WEBVIEW2_STATIC)
    target_link_libraries(webview PRIVATE "${WEBVIEW2_LOADER_LIBRARY}" Shlwapi)
endif ()

add_library(HuxerUI::WebView ALIAS webview)
