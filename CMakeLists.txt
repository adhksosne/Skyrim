cmake_minimum_required(VERSION 3.24)

project(SummonNameFix
        VERSION 2.0.0
        DESCRIPTION "Cross-version SKSE plugin (SE/AE/VR): replaces the hardcoded 's possessive in summon / owned-container names with a locale-aware replacement. No ESP/ESM/Papyrus."
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 依赖策略：不使用 vcpkg / 私有 registry / baseline。
# 全部通过 CMake FetchContent 直接从上游仓库按 tag 拉取，版本可复现、可审计，
# 且与 runner 上的新工具链（VS 18 / CMake 4.x）天然兼容。
#   fmt             —— CommonLibSSE-NG 与 spdlog 共用
#   spdlog          —— 日志
#   rapidcsv        —— CommonLibSSE-NG 内部读取版本库 CSV（header-only）
#   CommonLibSSE-NG —— SKSE 插件框架（单个 DLL 同时支持 SE / AE / VR）
# ---------------------------------------------------------------------------
include(FetchContent)

# --- fmt ---
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        11.2.0
    GIT_SHALLOW    TRUE
    OVERRIDE_FIND_PACKAGE)   # 让下游的 find_package(fmt) 直接用这份
FetchContent_MakeAvailable(fmt)

# --- spdlog ---
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)   # 复用上面的 fmt，避免两份 fmt
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.3
    GIT_SHALLOW    TRUE
    OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(spdlog)

# --- rapidcsv（header-only，只取头文件目录）---
FetchContent_Declare(rapidcsv
    GIT_REPOSITORY https://github.com/d99kris/rapidcsv.git
    GIT_TAG        v8.99
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(rapidcsv)
# CommonLibSSE-NG 用 find_path(RAPIDCSV_INCLUDE_DIRS "rapidcsv.h")；
# 预先设好该变量即可跳过它的查找。
set(RAPIDCSV_INCLUDE_DIRS "${rapidcsv_SOURCE_DIR}/src" CACHE PATH "rapidcsv include dir" FORCE)

# --- CommonLibSSE-NG ---
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)          # 不拉 Catch2
set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)   # 不需要 trampoline，也就完全避开 xbyak
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
FetchContent_Declare(CommonLibSSE_NG
    GIT_REPOSITORY https://github.com/CharmedBaryon/CommonLibSSE-NG.git
    GIT_TAG        v3.7.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(CommonLibSSE_NG)

# ---------------------------------------------------------------------------
# 插件本体
# ---------------------------------------------------------------------------
# 上游 CMake/CommonLibSSE.cmake 在解析 COMPATIBLE_RUNTIMES 时多写了一层 ${}：
# 单版本恰好无害，多版本会展开成多个参数并报
# "list sub-command LENGTH requires two arguments"。这里用修补版同名函数覆盖之。
include("${CMAKE_CURRENT_SOURCE_DIR}/ci/patched-commonlibsse.cmake")

# 以 add_subdirectory 方式引入时没有安装期的 Config，需自行提供 include 根。
set(CommonLibSSE_INCLUDE_DIRS "${CommonLibSSE_NG_SOURCE_DIR}/include")

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES src/main.cpp
    NAME ${PROJECT_NAME}
    AUTHOR "SummonNameFix"
    COMPATIBLE_RUNTIMES 1.5.97 1.6.620 1.4.34)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_options(${PROJECT_NAME} PRIVATE
    /utf-8          # 源码与执行字符集均按 UTF-8 处理（游戏文本编码为 UTF-8）
    /W4
    # CommonLibSSE-NG 的 REL/Relocation.h 不自包含，需预包含 PCH.h（详见 src/preinclude.h）
    "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/preinclude.h")
target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX)
