cmake_minimum_required(VERSION 3.21)

project(SummonNameFix
        VERSION 2.0.0
        DESCRIPTION "Cross-version SKSE plugin: replaces hardcoded 's possessive in summon/owned-container names for any locale (Chinese/Japanese/Korean/etc.)"
        LANGUAGES CXX)

find_package(CommonLibSSE REQUIRED)

# 绕过 CommonLibSSE-NG 3.5.3 的 CMake bug（详见 ci/patched-commonlibsse.cmake）：
#   list(LENGTH ${ADD_COMMONLIBSSE_PLUGIN_COMPATIBLE_RUNTIMES} ...)
# 单版本时 ${} 恰好展开成一个 token，无害；多版本时展开成多个参数，直接报
# "list sub-command LENGTH requires two arguments"。这就是 v1 用单个 1.5.97 能编过、
# 改成 1.5.97 1.6.620 1.4.34 后失败的原因。
# CMake 函数按名后绑定：在 find_package 之后重新定义同名函数即可覆盖上游实现。
include("${CMAKE_CURRENT_SOURCE_DIR}/ci/patched-commonlibsse.cmake")

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
