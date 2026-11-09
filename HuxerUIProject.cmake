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
  "target": "webview",
  "namespace": "huxerui",
  "publicTarget": "HuxerUI::WebView"
}
]=])
endif ()

if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    enable_language(CXX)
    if (APPLE AND (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src"
            OR EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/src"))
        enable_language(OBJCXX)
    endif ()
endif ()

if (NOT TARGET HuxerUI::huxerui AND NOT TARGET HuxerUI::huxerui_static)
    set(HUXERUI_HOME "$ENV{HUXERUI_HOME}" CACHE PATH "HuxerUI framework directory")
    if (HUXERUI_HOME AND EXISTS "${HUXERUI_HOME}/CMakeLists.txt"
            AND EXISTS "${HUXERUI_HOME}/include/huxerui/huxerui.h")
        set(HUXERUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(HUXERUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(HUXERUI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(HUXERUI_BUILD_STATIC ON CACHE BOOL "" FORCE)
        add_subdirectory("${HUXERUI_HOME}" "${CMAKE_BINARY_DIR}/huxerui-sdk" EXCLUDE_FROM_ALL)
    elseif (HUXERUI_HOME)
        find_package(HuxerUI CONFIG REQUIRED
                PATHS "${HUXERUI_HOME}"
                NO_DEFAULT_PATH
                NO_CMAKE_FIND_ROOT_PATH
        )
    else ()
        find_package(HuxerUI CONFIG REQUIRED)
    endif ()
endif ()
