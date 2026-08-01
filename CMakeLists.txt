cmake_minimum_required(VERSION 3.30)

# Clang on apple does not ship OpenMP, which XGBoost needs, so Homebrew LLVM is preferred
# when it is installed. The prefix is resolved through `brew` rather than hardcoded. If Homebrew
# LLVM is absent we fall through to the default compiler and let find_package(OpenMP)
if(APPLE AND NOT CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    find_program(BREW_EXECUTABLE brew)
    if(BREW_EXECUTABLE)
        execute_process(
            COMMAND ${BREW_EXECUTABLE} --prefix llvm
            OUTPUT_VARIABLE HOMEBREW_LLVM_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(HOMEBREW_LLVM_PREFIX AND EXISTS "${HOMEBREW_LLVM_PREFIX}/bin/clang++")
            set(CMAKE_C_COMPILER   "${HOMEBREW_LLVM_PREFIX}/bin/clang"   CACHE FILEPATH "")
            set(CMAKE_CXX_COMPILER "${HOMEBREW_LLVM_PREFIX}/bin/clang++" CACHE FILEPATH "")
            message(STATUS "Using Homebrew LLVM from ${HOMEBREW_LLVM_PREFIX}")
        endif()
    endif()
endif()

project(orama VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(ORAMA_USE_GUI "Build the ImGui/ImPlot live dashboard (requires OpenGL/GLFW)" ON)

file(GLOB SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/core/broker/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/core/util/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/learner/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/user/*.cpp")

if(ORAMA_USE_GUI)
    file(GLOB INTERFACE_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/interface/*.cpp")
    list(APPEND SOURCES ${INTERFACE_SOURCES})
endif()


if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(_DEBUG)
endif()

if(ORAMA_USE_GUI)
    find_package(OpenGL REQUIRED)
endif()

include(FetchContent)

FetchContent_Declare(
  databento
  GIT_REPOSITORY https://github.com/databento/databento-cpp
  GIT_TAG v0.62.1
)

# Windows has no system IANA tz database to fall back on.
if(WIN32)
    set(USE_SYSTEM_TZ_DB OFF CACHE BOOL "" FORCE)
else()
    set(USE_SYSTEM_TZ_DB ON CACHE BOOL "" FORCE)
endif()
set(BUILD_TZ_LIB       ON  CACHE BOOL "" FORCE)
set(ENABLE_DATE_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    date_src
    GIT_REPOSITORY https://github.com/HowardHinnant/date.git
    GIT_TAG        v3.0.1
)

FetchContent_Declare(
    protobuf
    GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
    GIT_TAG v3.12.4
    SOURCE_SUBDIR cmake
)
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# XGBoost: FetchContent instead of a Homebrew-only find_path, so no system package is
# needed on any platform.
FetchContent_Declare(
    xgboost
    GIT_REPOSITORY https://github.com/dmlc/xgboost.git
    GIT_TAG        v3.3.0
)
set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)

# TA-Lib: same reasoning as XGBoost above. The upstream 0.6+ line ships a portable
# CMake build (ta-lib-static target, public include dir at include/, headers flat
# e.g. <ta_common.h> - no "ta-lib/" prefix like Homebrew's repackaged layout).
FetchContent_Declare(
    ta_lib_src
    GIT_REPOSITORY https://github.com/ta-lib/ta-lib.git
    GIT_TAG        v0.7.1
)
set(BUILD_DEV_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS_TALIB OFF CACHE BOOL "" FORCE)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

FetchContent_MakeAvailable(databento date_src protobuf xgboost ta_lib_src)

if(ORAMA_USE_GUI)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.3.9)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)

    # ImGui
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.92.8)

    # ImPlot
    FetchContent_Declare(implot
        GIT_REPOSITORY https://github.com/epezent/implot.git
        GIT_TAG        v1.0)

    FetchContent_MakeAvailable(glfw imgui implot)
endif()

set_target_properties(libprotobuf PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

set(IB_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/core/twsapi")
set(IB_PROTO_DIR "${IB_API_DIR}/protobufUnix")

file(GLOB IB_API_SOURCES
    "${IB_API_DIR}/*.cpp"
    "${IB_PROTO_DIR}/*.cpp"
    "${IB_PROTO_DIR}/*.cc"
)

# core/twsapi/ is not part of this repo (IBKR's TWS API can't be redistributed here -
# see README). Whoever drops the official SDK back in also drops in the platform-
# appropriate decimal-math library from that same download, so we glob for whatever
# extension/form it takes rather than assuming a single filename.
file(GLOB BID_LIB_FILE
    "${IB_API_DIR}/libbid/*.a"
    "${IB_API_DIR}/libbid/*.lib"
)
file(GLOB BID_LIB_SOURCES
    "${IB_API_DIR}/libbid/*.c"
    "${IB_API_DIR}/libbid/*.cpp"
)

if(IB_API_SOURCES)
    add_library(ibapi STATIC ${IB_API_SOURCES} ${BID_LIB_SOURCES})

    target_include_directories(ibapi PUBLIC
        ${IB_API_DIR}
        ${IB_PROTO_DIR}
    )

    target_link_libraries(ibapi PUBLIC
        libprotobuf
        ${BID_LIB_FILE}
    )

    target_compile_definitions(ibapi PUBLIC
        TWS_S_ASYNC
    )

    # compile the IBKR API itself using C++17 so old headers don't break
    set_target_properties(ibapi PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_compile_options(ibapi PRIVATE -w) # suppress warnings

    set(IB_API_AVAILABLE TRUE)
    add_compile_definitions(HAS_IBKR_BROKER)
endif()

# Resolve Homebrew's libomp dynamically (works on both Apple Silicon /opt/homebrew and
# Intel Mac /usr/local prefixes) instead of hardcoding a path. Linux/Windows find their
# own OpenMP runtime (libgomp / MSVC's built-in support) with no extra hints needed.
if(APPLE)
    execute_process(
        COMMAND brew --prefix libomp
        OUTPUT_VARIABLE LIBOMP_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(LIBOMP_PREFIX)
        set(OpenMP_ROOT "${LIBOMP_PREFIX}")
    endif()
endif()
find_package(OpenMP REQUIRED)

if(ORAMA_USE_GUI)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC glfw)

    add_library(implot STATIC
        ${implot_SOURCE_DIR}/implot.cpp
        ${implot_SOURCE_DIR}/implot_items.cpp)
    target_include_directories(implot PUBLIC ${implot_SOURCE_DIR})
    target_link_libraries(implot PUBLIC imgui)

    add_compile_definitions(USE_GUI)
endif()

#add_compile_definitions(TRAIN_MODEL)

add_executable(orama ${SOURCES})

target_link_libraries(orama PRIVATE
    databento::databento
    date::date-tz
    xgboost
    OpenMP::OpenMP_CXX
    ta-lib-static
)

if(ORAMA_USE_GUI)
    target_link_libraries(orama PRIVATE imgui implot OpenGL::GL)

    # for opengl
    if(APPLE)
        target_compile_definitions(orama PRIVATE GL_SILENCE_DEPRECATION)
    endif()
endif()

if(IB_API_AVAILABLE)
    target_link_libraries(orama PRIVATE ibapi)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
message(STATUS "Configuring clang/gnu build")
    target_compile_options(orama PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
    )

endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")

    message(STATUS "Configuring Debug build")

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(orama PRIVATE
            -O0
            -g3
            -fno-omit-frame-pointer
            -fsanitize=address,undefined
        )

        target_link_options(orama PRIVATE
            -fsanitize=address,undefined
        )
    elseif(MSVC)
        # MSVC has no UBSan equivalent, so Debug-on-Windows only gets ASan.
        target_compile_options(orama PRIVATE
            /Od
            /Zi
            /RTC1
            /fsanitize=address
        )
    endif()

endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")

    message(STATUS "Configuring Release build")

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(orama PRIVATE
            -O3
            -DNDEBUG
        )
    elseif(MSVC)
        target_compile_options(orama PRIVATE
            /O2
            /DNDEBUG
        )
    endif()

    include(CheckIPOSupported)

    check_ipo_supported(
        RESULT lto_supported
        OUTPUT lto_error
    )

    if(lto_supported)
        set_property(
            TARGET orama
            PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE
        )
    else()
        message(WARNING
            "LTO not supported: ${lto_error}"
        )
    endif()

endif()
