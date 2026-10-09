cmake_minimum_required(VERSION 3.21)

# 静态 MSVC 运行时，与 x64-windows-static 三元组保持一致（必须在 project() 之前设置）。
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
	set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
endif()

project(SummonNameFix VERSION 2.0.0 LANGUAGES CXX)

set(PLUGIN_NAME "SummonNameFix" CACHE STRING "生成的 SKSE 插件 DLL 名称")
set(PLUGIN_AUTHOR "SummonNameFix" CACHE STRING "SKSE 中显示的插件作者")

# UNIVERSAL：一个 DLL 同时支持 SE 1.5.97、AE 1.6/1.7 与 VR 1.4.15（布局差异在运行时选择）。
set(SKYRIM_VARIANT "UNIVERSAL" CACHE STRING "Skyrim runtime variant: UNIVERSAL, SE, AE, or VR")
set_property(CACHE SKYRIM_VARIANT PROPERTY STRINGS UNIVERSAL SE AE VR)
string(TOUPPER "${SKYRIM_VARIANT}" SKYRIM_VARIANT)

if(SKYRIM_VARIANT STREQUAL "UNIVERSAL")
	set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
	set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
	set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
elseif(SKYRIM_VARIANT STREQUAL "SE")
	set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
	set(ENABLE_SKYRIM_AE OFF CACHE BOOL "" FORCE)
	set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
elseif(SKYRIM_VARIANT STREQUAL "AE")
	set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
	set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
	set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
elseif(SKYRIM_VARIANT STREQUAL "VR")
	set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
	set(ENABLE_SKYRIM_AE OFF CACHE BOOL "" FORCE)
	set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
else()
	message(FATAL_ERROR "SKYRIM_VARIANT 必须是 UNIVERSAL / SE / AE / VR 之一。")
endif()

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)   # 不拉 Catch2

# CommonLibSSE-NG（Zzyxz 修复分支：AE 1.7 / VR 布局 / Address Library V5 / 大量 ID 修正）
set(COMMONLIB_NAME "CommonLibSSE")
set(COMMONLIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibSSE-NG/extern/CommonLibSSE")
if(NOT EXISTS "${COMMONLIB_PATH}/include/REL/Relocation.h")
	message(FATAL_ERROR
		"找不到 CommonLibSSE 检出：${COMMONLIB_PATH}\n"
		"请先拉取子模块：git submodule update --init --recursive")
endif()
add_subdirectory("${COMMONLIB_PATH}" ${COMMONLIB_NAME})

# ---------------------------------------------------------------------------
# 插件本体
# ---------------------------------------------------------------------------
add_library(${PLUGIN_NAME} SHARED src/main.cpp)
target_compile_features(${PLUGIN_NAME} PRIVATE cxx_std_23)
target_compile_definitions(${PLUGIN_NAME} PRIVATE
	WIN32_LEAN_AND_MEAN
	NOMINMAX)
target_link_libraries(${PLUGIN_NAME} PRIVATE ${COMMONLIB_NAME}::${COMMONLIB_NAME})
if(MSVC)
	target_compile_options(${PLUGIN_NAME} PRIVATE
		/permissive-
		/utf-8          # 源码与执行字符集均按 UTF-8 处理（游戏文本编码为 UTF-8）
		/W4
		# CommonLibSSE 的 REL/Relocation.h 不自包含，需预包含 PCH.h（详见 src/preinclude.h）
		"/FI${CMAKE_CURRENT_SOURCE_DIR}/src/preinclude.h")
endif()
