// PossessiveLocalization（归属名本地化 / 所有格本地化）
// SKSE 插件：为"有主对象的显示名"补上本地化，修掉引擎硬编码的英文所有格 "'s"
//
// 说明：严格来说这不是一个缺陷（bug），而是引擎把英文所有格硬编码进了可执行文件，
// 导致本地化版本里"所有者 + 所有格 + 名字"始终夹着英文 's，属于**本地化不完整**。
//
// 现象：中文/日文/韩文等本地化后，召唤物与有主容器的名字仍是
//   召唤物：  "本怡's 骷髅战士"
//   有主容器："Sven's Chest"
//
// 原理（全部经实际 exe 字节取证验证，详见 REPORT.md）：
//   引擎把"所有者 + 所有格 + 名字"拼起来时，用的是编译进 SkyrimSE.exe 的两个字符串字面量：
//     1) "%s's %s"   —— printf 式复合格式（召唤物 / 有主 Actor 的名字组装）
//     2) "'s "       —— 独立串，strcat 拼接（有主容器的名字组装）
//   它们属于可执行映像（.rdata），**不在任何 ESP/ESM/Papyrus 数据里**，所以汉化包改不到，
//   只能由 SKSE 插件在运行时把内存里这两串等长改掉。
//   实测：两串在全 exe 中各仅出现 1 次、且前一字节均为 0x00（标准字面量起点），
//   因此按"完整字面量"精确定位即可，不需要地址库/偏移，也不会误伤。
//
// 硬性约束：不依赖 ESP/ESL/ESM、不用 Papyrus、不改存档；不需要 Address Library。
//   任何一步不符预期（找不到字面量、长度不等、内存不可写）都只记日志并跳过，绝不含错改写。

#include "SKSE/SKSE.h"

#include <Windows.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/logger.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// 构建指纹：CI 通过 -DPLUGIN_BUILD_ID=<commit sha> 注入；本地构建为 "dev"。
// 日志启动时会打印它，用来确认实际被加载的到底是哪一次构建。
#ifndef PLUGIN_BUILD_ID
#define PLUGIN_BUILD_ID "dev"
#endif

// ----------------------------------------------------------------------------------
// 待替换的引擎原串
// ----------------------------------------------------------------------------------
namespace patch_bytes
{
	// 全 exe 各仅 1 处；等长替换：7 字节 -> 7 字节，3 字节 -> 3 字节
	constexpr std::string_view kFmtExpected{ "%s's %s", 7 };
	constexpr std::string_view kAposExpected{ "'s ", 3 };

	// 所有格标记固定 3 字节时，"%s<标记>%s" 恰好也是 7 字节，与原格式串等长，
	// 因此格式串可以由标记推导，配置里只需要一项"目标语言"。
	constexpr std::size_t kMarkerBytes = 3;
	constexpr std::size_t kFmtBytes = 7;
}

// ----------------------------------------------------------------------------------
// 语言预设：key（ini 里的 Language）→ 3 字节 UTF-8 所有格标记
// ----------------------------------------------------------------------------------
namespace
{
	// 只处理非拉丁文字语言：它们的文字体系里本不该出现英文所有格。
	// 拉丁文字语言（英/德/法…）用 's 或词尾 -s，视觉上都属正常，不处理。
	constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kLanguagePresets{ {
		{ "zh", "\xE7\x9A\x84" },        // 的
		{ "ja", "\xE3\x81\xAE" },        // の
		{ "ko", "\xEC\x9C\x9C" },        // 의
	} };

	constexpr std::string_view kDefaultLanguage = "zh";

	struct Config
	{
		std::string language{ kDefaultLanguage };   // Language：zh / ja / ko
		std::string customApos;                     // CustomAposReplacement：自定义 3 字节标记（覆盖预设）
	};

	struct Language
	{
		std::string apos{ "\xE7\x9A\x84" };   // 3 字节所有格标记
		std::string fmt{ "%s\xE7\x9A\x84%s" };  // 7 字节格式串（由标记推导）
	};

	Config g_cfg;
	Language g_lang;
}

// ----------------------------------------------------------------------------------
// 路径工具（全部基于模块/已知文件夹，不依赖当前工作目录）
// ----------------------------------------------------------------------------------
std::filesystem::path GetPluginPath()
{
	HMODULE self = nullptr;
	::GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&GetPluginPath),
		&self);
	// 宽字符 API：Mod Organizer 的 mod 路径常含非 ASCII 字符
	wchar_t buf[MAX_PATH]{};
	::GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
	return { buf };
}

std::optional<std::filesystem::path> GetGameRoot()
{
	const HMODULE exe = ::GetModuleHandleW(nullptr);
	if (!exe) return std::nullopt;
	wchar_t buf[MAX_PATH]{};
	const DWORD len = ::GetModuleFileNameW(exe, buf, static_cast<DWORD>(std::size(buf)));
	if (len == 0 || len >= std::size(buf)) return std::nullopt;
	return std::filesystem::path{ buf }.parent_path();
}

// 日志目录：<文档>\My Games\<版本>\SKSE（与 skse64.log 同目录）。
// 版本按 exe 所在目录判断，而不是按当前工作目录 —— CommonLibSSE 的 log_directory()
// 用 CWD 判断，在 MO2 等启动方式下可能误判成 GOG 版，导致日志"看起来不存在"。
std::filesystem::path GetCanonicalLogDir()
{
	const auto ngDir = SKSE::log::log_directory();   // 只借用它解析出 <文档>\My Games
	if (!ngDir) return {};

	const auto myGames = ngDir->parent_path().parent_path();
	std::wstring variant = L"Skyrim Special Edition";
	if (const auto gameRoot = GetGameRoot()) {
		std::error_code ec;
		if (std::filesystem::exists(*gameRoot / L"openvr_api.dll", ec)) {
			variant = L"Skyrim VR";
		} else if (!std::filesystem::exists(*gameRoot / L"steam_api64.dll", ec)) {
			variant = L"Skyrim Special Edition GOG";
		}
	}
	return myGames / variant / L"SKSE";
}

// ----------------------------------------------------------------------------------
// 日志
// ----------------------------------------------------------------------------------
void SetupLog()
{
	const auto logsFolder = GetCanonicalLogDir();
	if (logsFolder.empty()) return;   // 拿不到规范目录就不写日志，避免"东一个西一个"

	std::error_code ec;
	std::filesystem::create_directories(logsFolder, ec);
	try {
		const auto logFilePath = logsFolder / "PossessiveLocalization.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
		auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
		logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
		logger->set_level(spdlog::level::info);
		logger->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(logger));
	} catch (const std::exception&) {
		// 日志不可写时静默继续：插件功能本身不依赖日志
	}
}

// ----------------------------------------------------------------------------------
// 配置：ini 与 dll 同目录；整份删除即用默认值
// ----------------------------------------------------------------------------------
void LoadConfig()
{
	auto iniPath = GetPluginPath();
	iniPath.replace_extension(".ini");

	std::ifstream file(iniPath);
	if (!file.is_open()) {
		spdlog::info("未找到配置文件 {}，使用默认语言 {}。", iniPath.string(), g_cfg.language);
		return;
	}

	std::string line;
	while (std::getline(file, line)) {
		const auto comment = line.find_first_of(";#");
		if (comment != std::string::npos) line.resize(comment);
		const auto eq = line.find('=');
		if (eq == std::string::npos) continue;

		auto key = line.substr(0, eq);
		auto val = line.substr(eq + 1);
		const auto trim = [](std::string& s) {
			const auto notSpace = [](char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
			const auto b = std::find_if(s.begin(), s.end(), notSpace);
			const auto e = std::find_if(s.rbegin(), s.rend(), notSpace).base();
			s = (b < e) ? std::string(b, e) : std::string{};
		};
		trim(key);
		trim(val);

		if (key == "Language") {
			g_cfg.language = val.empty() ? std::string{ kDefaultLanguage } : val;
		} else if (key == "CustomAposReplacement") {
			g_cfg.customApos = val;
		}
	}
}

// 解析语言：预设优先，CustomAposReplacement 可覆盖；随后由标记推导格式串。
void ResolveLanguage()
{
	std::string_view marker;
	for (const auto& [key, value] : kLanguagePresets) {
		if (key == g_cfg.language) {
			marker = value;
			break;
		}
	}
	if (marker.empty()) {
		spdlog::warn("未知 Language=\"{}\"（可用：zh / ja / ko），回退到 {}。",
			g_cfg.language, kDefaultLanguage);
		for (const auto& [key, value] : kLanguagePresets) {
			if (key == kDefaultLanguage) {
				marker = value;
				break;
			}
		}
	}

	if (!g_cfg.customApos.empty()) {
		if (g_cfg.customApos.size() == patch_bytes::kMarkerBytes) {
			marker = g_cfg.customApos;
		} else {
			spdlog::warn("CustomAposReplacement 必须是 {} 字节（当前 {} 字节），已忽略。",
				patch_bytes::kMarkerBytes, g_cfg.customApos.size());
		}
	}

	g_lang.apos.assign(marker);
	g_lang.fmt = std::string("%s").append(g_lang.apos).append("%s");
}

// ----------------------------------------------------------------------------------
// 字面量扫描：只扫主 exe 自身的映像（BaseOfImage .. BaseOfImage + SizeOfImage）
// 命中要求：起点前一字节为 NUL（或紧接 UTF-8 BOM）、且以 NUL 结束 —— 即一个完整的 C 字符串字面量。
// 说明：不做"从字符串往回找 lea"的校验 —— 引用字符串的 lea 位于 .text、字符串位于 .rdata，
//       两者并不相邻，往回扫命中率恒为 0（这是早期方案的错误前提）。
// ----------------------------------------------------------------------------------
std::vector<std::uintptr_t> FindStringRVA(std::string_view a_needle)
{
	if (a_needle.empty()) return {};

	const HMODULE mod = ::GetModuleHandleA(nullptr);
	if (!mod) return {};

	const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
	if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return {};
	const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
		reinterpret_cast<const std::uint8_t*>(mod) + dosHeader->e_lfanew);
	if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return {};

	const auto base = reinterpret_cast<std::uintptr_t>(mod);
	const auto imageEnd = base + static_cast<std::uintptr_t>(ntHeaders->OptionalHeader.SizeOfImage);

	std::vector<std::uintptr_t> result;
	for (auto cur = base; cur < imageEnd;) {
		MEMORY_BASIC_INFORMATION mbi{};
		if (!::VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi))) break;

		const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		const auto scanEnd = (regionEnd < imageEnd) ? regionEnd : imageEnd;

		if (mbi.State == MEM_COMMIT && scanEnd > cur &&
			(mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READ)) != 0) {
			const auto* region = reinterpret_cast<const std::uint8_t*>(cur);
			const auto len = static_cast<std::size_t>(scanEnd - cur);

			if (len > a_needle.size()) {
				const auto firstByte = a_needle.front();
				for (std::size_t i = 0; i + a_needle.size() < len; ++i) {
					if (region[i] != firstByte) continue;   // 首字节快速过滤
					if (std::memcmp(region + i, a_needle.data(), a_needle.size()) != 0) continue;
					if (region[i + a_needle.size()] != 0) continue;   // 必须是完整字面量（尾随 NUL）
					const bool atLiteralStart =
						(cur + i == base) ||
						(i > 0 && (region[i - 1] == 0 ||
							(i >= 3 && region[i - 1] == 0xBF && region[i - 2] == 0xBB && region[i - 3] == 0xEF)));
					if (atLiteralStart) {
						result.push_back(cur + i);
					}
				}
			}
		}
		cur = (scanEnd > cur) ? scanEnd : (cur + 0x1000);   // 兜底推进，避免死循环
	}
	return result;
}

// ----------------------------------------------------------------------------------
// 等长替换
// ----------------------------------------------------------------------------------
bool ApplyStringPatch(std::string_view a_expected, std::string_view a_replacement, std::string_view a_label)
{
	if (a_expected.size() != a_replacement.size()) {
		spdlog::error("[{}] 内部错误：替换串（{} 字节）与原文（{} 字节）长度不一致，放弃。",
			a_label, a_replacement.size(), a_expected.size());
		return false;
	}

	const auto targets = FindStringRVA(a_expected);
	if (targets.empty()) {
		spdlog::error("[{}] 未在 exe 映像内找到字面量 \"{}\"，本补丁跳过。", a_label, a_expected);
		return false;
	}

	std::size_t patched = 0;
	for (const auto addr : targets) {
		DWORD oldProtect = 0;
		if (!::VirtualProtect(reinterpret_cast<LPVOID>(addr), a_replacement.size(), PAGE_READWRITE, &oldProtect)) {
			spdlog::error("[{}] VirtualProtect 失败（0x{:X}，GetLastError={}），跳过该处。",
				a_label, addr, ::GetLastError());
			continue;
		}
		std::memcpy(reinterpret_cast<void*>(addr), a_replacement.data(), a_replacement.size());
		DWORD tmp = 0;
		::VirtualProtect(reinterpret_cast<LPVOID>(addr), a_replacement.size(), oldProtect, &tmp);
		::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), a_replacement.size());

		spdlog::info("[{}] 已补丁：0x{:X}：\"{}\" -> \"{}\"（等长 {} 字节）。",
			a_label, addr, a_expected, a_replacement, a_replacement.size());
		++patched;
	}

	if (patched == 0) {
		spdlog::error("[{}] 所有候选写入都失败了。", a_label);
		return false;
	}
	spdlog::info("[{}] 完成：共补丁 {} 处。", a_label, patched);
	return true;
}

// ----------------------------------------------------------------------------------
// SKSE 入口：AE 读 SKSEPlugin_Version，SE / VR 走 SKSEPlugin_Query
// ----------------------------------------------------------------------------------
namespace
{
	constexpr std::string_view kPluginName{ "PossessiveLocalization" };
	constexpr std::string_view kPluginAuthor{ "adhksosne" };   // 作者名（可改成你想显示的名字）
	constexpr REL::Version kPluginVersion{ 2, 0, 0, 0 };
}

extern "C" __declspec(dllexport) constinit auto SKSEPlugin_Version = []() {
	SKSE::PluginVersionData version;
	version.PluginVersion(kPluginVersion);
	version.PluginName(kPluginName);
	version.AuthorName(kPluginAuthor);
	version.UsesAddressLibrary();
	version.UsesAddressLibraryV5();
	version.UsesNoStructs();
	return version;
}();

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->name = kPluginName.data();
	a_info->version = 2;
	if (a_skse->IsEditor()) {
		return false;
	}
	// SKSE VR 报告的是 1.4.15.1，所以只按运行时族判断
	[[maybe_unused]] const auto family = REL::Module::RuntimeFor(a_skse->RuntimeVersion());
#ifdef ENABLE_SKYRIM_SE
	if (family == REL::Module::Runtime::SE) {
		return true;
	}
#endif
#ifdef ENABLE_SKYRIM_VR
	if (family == REL::Module::Runtime::VR) {
		return true;
	}
#endif
	return false;
}

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	// 第一步就建日志：即使后面任何一步提前退出，也一定留下可排查的记录。
	SetupLog();

	const auto runtime = a_skse->RuntimeVersion();
	spdlog::info("{} v{} build={}（编译于 {} {}）",
		kPluginName, kPluginVersion.string("."), PLUGIN_BUILD_ID, __DATE__, __TIME__);
	spdlog::info("已加载模块：{}", GetPluginPath().string());
	spdlog::info("运行时 {}。", runtime.string());

	LoadConfig();
	ResolveLanguage();
	spdlog::info("语言：{}（格式串 \"{}\" → \"{}\"，所有格 \"{}\" → \"{}\"）。",
		g_cfg.language, patch_bytes::kFmtExpected, g_lang.fmt,
		patch_bytes::kAposExpected, g_lang.apos);

	// 补丁 1：召唤物 / 有主 Actor 的名字组装（printf 式复合格式串）
	ApplyStringPatch(patch_bytes::kFmtExpected, g_lang.fmt, "召唤物路径");

	// 补丁 2：有主容器的名字组装（独立所有格串，strcat 拼接）
	ApplyStringPatch(patch_bytes::kAposExpected, g_lang.apos, "有主容器路径");

	spdlog::info("{} 初始化完成。", kPluginName);
	return true;
}
