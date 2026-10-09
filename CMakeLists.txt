cmake_minimum_required(VERSION 3.21)

project(SummonNameFix
        VERSION 2.0.0
        DESCRIPTION "Cross-version SKSE plugin: replaces hardcoded 's possessive in summon/owned-container names for any locale (Chinese/Japanese/Korean/etc.)"
        LANGUAGES CXX)

find_package(CommonLibSSE REQUIRED)

# 绕过 CommonLibSSE-NG 3.5.3 的 CMake bug：
# share/CommonLibSSE/CommonLibSSE.cmake 第 91 行误写成 list(LENGTH ${VAR} ...)，
# 多了一层 ${} 导致 "list sub-command LENGTH requires two arguments"。
# CMake 函数按名后绑定，这里用本地修补版重新定义 add_commonlibsse_plugin 覆盖之。
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CommonLibSSE.cmake")

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES src/main.cpp
    NAME ${PROJECT_NAME}
    AUTHOR "SummonNameFix"
    COMPATIBLE_RUNTIMES 1.5.97 1.6.620 1.4.34)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_options(${PROJECT_NAME} PRIVATE
    /utf-8          # 源码与执行字符集均按 UTF-8 处理（游戏文本编码为 UTF-8）
    /W4
    # CommonLibSSE-NG 3.5.3 的 REL/Relocation.h 不自包含，需预包含 PCH.h（详见 src/preinclude.h）
    "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/preinclude.h")
target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX)
