cmake_minimum_required(VERSION 3.21)

project(SummonNameFixCN
        VERSION 1.0.0
        DESCRIPTION "SKSE plugin: translate the hardcoded 's possessive in summon/owned-container names for Chinese localization (Skyrim SE 1.5.97)"
        LANGUAGES CXX)

find_package(CommonLibSSE REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES src/main.cpp
    NAME ${PROJECT_NAME}
    AUTHOR "SummonNameFixCN"
    COMPATIBLE_RUNTIMES 1.5.97)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_options(${PROJECT_NAME} PRIVATE
    /utf-8          # 源码与执行字符集均按 UTF-8 处理（游戏文本编码为 UTF-8）
    /W4)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX)
