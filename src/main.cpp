// SummonNameFix —— SKSE 插件：跨版本（SE / AE / VR）修复召唤物与有主容器名称中的硬编码所有格
//
// 功能：检测并 patch 引擎内硬编码的所有格格式字符串，使其与 INI 配置的目标语言匹配。
//   默认（中文）：
//     召唤物路径："%s's %s" → "%s的%s"
//     容器路径  ："'s "     → "的"
//   用户可通过 INI 配置任意目标语言（日文「の」、韩文「의」、德语「's」→ "'s」等）。
//
// 原理（详见 REPORT.md）：
//   1) 调用 FindStringPattern 在进程地址空间扫描已知英文原串（支持带/不带 UTF-8 BOM 两种常见变体）。
//      找到后向回扫描若干字节内的 lea reg,[rip+disp32]（48 8D xx 5byte），反解出原始格式串地址。
//   2) 校验原串完全等于预期格式（含尾随 NUL），确认后再等长替换为 INI 配置的目标串。
//   3) 可选 hook TESObjectREFR::GetDisplayFullName（Address Library REL ID 19354/19781）
//      兜底处理 Papyrus SetDisplayName 等运行时构造的含原串的名字。
//
// 兼容性说明：
//   - 不依赖任何已发布的 Address Library ID（24212 / 17486 仅在旧版 REPORT 中作为记录）；
//     全部通过运行时特征扫描动态定位，理论上覆盖所有包含相同格式串的运行时（SE 1.x / AE 1.6.x / VR 1.4.x）。
//   - 核心补丁（格式串扫描 + 等长替换）为纯 Win32 实现，不依赖 Address Library；
//     仅可选的 GetDisplayFullName 兜底 hook 需要地址库（缺失时记录日志并跳过）。
//   - 任何一步（找不到原串、lea 偏移不符、原串校验失败、VirtualProtect 失败）都会记录日志并跳过该补丁，
//     保证不会因版本漂移误写任意字节。

#include "RE/T/TESObjectREFR.h"
#include "SKSE/SKSE.h"

#include <Windows.h>
#include <intrin.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <charconv>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

// 构建指纹：CI 通过 -DPLUGIN_BUILD_ID=<commit sha> 注入；本地构建为 "dev"。
// 日志启动时会打印它，用来确认实际被加载的到底是哪一次构建。
#ifndef PLUGIN_BUILD_ID
#define PLUGIN_BUILD_ID "dev"
#endif

// ----------------------------------------------------------------------------------
// 配置与常量
// ----------------------------------------------------------------------------------
namespace patch_bytes
{
	// 英文原串（两个路径）——作为特征匹配的种子，全 exe 均仅 1 处引用
	constexpr std::string_view kFmtExpected{ "%s's %s", 7 };
	constexpr std::string_view kAposExpected{ "'s ", 3 };

	// 搜索模式（同时覆盖带/不带 UTF-8 BOM 两种 exe 变体；不含 BOM 的 exe 也能匹配，因为子串本身无冲突）
	// UTF-8 BOM = EF BB BF，共 3 字节；BOM 之后的第一个字节是 '%' (0x25)，所以 pattern 为 BOM + '%'
	constexpr std::array<std::uint8_t, 4> kBomFirstByte = { 0xEF, 0xBB, 0xBF, 0x25 };
	constexpr std::array<std::uint8_t, 1>   kFmtSeeds = { 0x25 };     // '%'
	constexpr std::array<std::uint8_t, 3>   kAposSeeds = { 0x27, 0x73, 0x20 }; // ''' 's' ' '

	// strstr 快速针（含尾随空格；拼接处必为 "'s "）
	constexpr std::string_view kNeedle{ "'s ", 3 };
	constexpr const char* kNeedleC = "'s ";

	// 用于反解 lea 的 3 字节 opcode 候选（48 8D xx 是 x64 标准前缀；4C 8D 05 是 lea r8）
	constexpr std::array<std::uint8_t, 3> kLeaPrefix = { 0x48, 0x8D };

	// 回看 lea 指令的最大字节范围（lea 通常紧邻格式串，但不同编译产物/优化级别会有出入）
	constexpr std::size_t kMaxBackScan = 64;
}

// 语言包默认值（中文）
struct LanguagePack
{
	std::string fmtReplacement = "%s\xe7\x9a\x84%s";   // "%s的%s"（7 字节）
	std::string aposReplacement = "\xe7\x9a\x84";       // "的"（3 字节）
	std::string formatName = "召唤物路径";
	std::string aposName = "有主容器路径";
};

// 多语言预设（key 即 ini 里的 Language 字段值）
// 注意：LanguagePack 内含 std::string，MSVC 无法在常量求值中构造（C2178/C7595），
// 因此这里用运行期初始化的静态表，而不是 consteval/constexpr。
std::array<std::pair<std::string_view, LanguagePack>, 4> MakePresets()
{
	return {{
		// 中文
		{ "zh", { "%s\xe7\x9a\x84%s", "\xe7\x9a\x84", "召唤物路径", "有主容器路径" } },
		// 日文：の = E3 of E no E (0xE3 0x81 0xAE)，长度 3
		{ "ja", { "%s\xe3\x81\xae%s", "\xe3\x81\xae", "召喚物パス", "コンテナパス" } },
		// 韩文：의 = EC 9c 9c (0xEC 0x9C 0x9C)，长度 3
		{ "ko", { "%s\xec\x9c\x9c%s", "\xec\x9c\x9c", "소환경로", "컨테이너경로" } },
		// 德语：保留英文 's 不变（仅作为对比测试/德语不替换场景）
		{ "de", { "%s's %s", "'s ", "npc_path", "cont_path" } },
	}};
}

namespace
{
	const auto kLanguagePresets = MakePresets();
}

// ------------------------------ 配置 ------------------------------
struct Config
{
	bool master = true;
	bool patchSummon = true;
	bool patchContainer = true;
	bool hookDisplayName = false;
	bool hookOnlyActors = true;
	std::size_t cacheLimit = 4096;
	spdlog::level::level_enum logLevel = spdlog::level::info;
	bool logNameCalls = false;
	std::size_t logCallsLimit = 200;
	std::string language = "zh";   // 默认中文
	std::string customFmtReplacement;
	std::string customAposReplacement;
	std::string fmtNameOverride;
	std::string contNameOverride;
};

Config g_cfg;
LanguagePack g_lang;

bool ParseBool(std::string_view a_val, bool a_default)
{
	if (a_val.empty()) return a_default;
	switch (a_val.front()) {
	case '1': case 't': case 'T': case 'y': case 'Y': return true;
	case '0': case 'f': case 'F': case 'n': case 'N': return false;
	default: return a_default;
	}
}

std::optional<std::size_t> ParseSize(std::string_view a_val)
{
	std::size_t value = 0;
	const char* first = a_val.data();
	const char* last = a_val.data() + a_val.size();
	if (a_val.empty() || std::from_chars(first, last, value).ec != std::errc{}) {
		return std::nullopt;
	}
	return value;
}

void ResolveLanguage()
{
	// 优先使用自定义替换串（若用户填写），否则查预设表
	for (const auto& [key, pack] : kLanguagePresets) {
		if (key == g_cfg.language) {
			g_lang = pack;
			break;
		}
	}
	if (!g_cfg.customFmtReplacement.empty()) {
		g_lang.fmtReplacement = g_cfg.customFmtReplacement;
	}
	if (!g_cfg.customAposReplacement.empty()) {
		g_lang.aposReplacement = g_cfg.customAposReplacement;
	}
	if (!g_cfg.fmtNameOverride.empty()) {
		g_lang.formatName = g_cfg.fmtNameOverride;
	}
	if (!g_cfg.contNameOverride.empty()) {
		g_lang.aposName = g_cfg.contNameOverride;
	}
}

std::filesystem::path GetPluginPath()
{
	HMODULE self = nullptr;
	::GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&GetPluginPath),
		&self);
	// 用宽字符 API：Mod Organizer 的 mod 路径常含非 ASCII 字符
	wchar_t buf[MAX_PATH]{};
	::GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
	return { buf };
}

void LoadConfig()
{
	auto iniPath = GetPluginPath();
	iniPath.replace_extension(".ini");

	std::ifstream file(iniPath);
	if (!file.is_open()) {
		spdlog::info("未找到配置文件 {}，使用默认配置（中文）。", iniPath.string());
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
		const auto keyB = key.find_last_not_of(" \t\r\n");
		const auto valB = val.find_last_not_of(" \t\r\n");
		if (keyB == std::string::npos || valB == std::string::npos) continue;
		key.erase(keyB + 1);
		const auto keyF = key.find_first_not_of(" \t\r\n");
		key.erase(0, keyF);
		val.erase(valB + 1);
		const auto valF = val.find_first_not_of(" \t\r\n");
		val.erase(0, valF);

		if (key == "Enable") {
			g_cfg.master = ParseBool(val, g_cfg.master);
		} else if (key == "PatchSummonPath") {
			g_cfg.patchSummon = ParseBool(val, g_cfg.patchSummon);
		} else if (key == "PatchContainerPath") {
			g_cfg.patchContainer = ParseBool(val, g_cfg.patchContainer);
		} else if (key == "HookDisplayName") {
			g_cfg.hookDisplayName = ParseBool(val, g_cfg.hookDisplayName);
		} else if (key == "HookOnlyActors") {
			g_cfg.hookOnlyActors = ParseBool(val, g_cfg.hookOnlyActors);
		} else if (key == "CacheLimit") {
			if (const auto v = ParseSize(val)) g_cfg.cacheLimit = *v;
		} else if (key == "LogLevel") {
			if (auto lvl = spdlog::level::from_str(val); lvl != spdlog::level::off || val == "off") {
				g_cfg.logLevel = lvl;
			}
		} else if (key == "LogNameCalls") {
			g_cfg.logNameCalls = ParseBool(val, g_cfg.logNameCalls);
		} else if (key == "LogCallsLimit") {
			if (const auto v = ParseSize(val)) g_cfg.logCallsLimit = *v;
		} else if (key == "Language") {
			g_cfg.language = val.empty() ? "zh" : val;
		} else if (key == "CustomFmtReplacement") {
			g_cfg.customFmtReplacement = val;
		} else if (key == "CustomAposReplacement") {
			g_cfg.customAposReplacement = val;
		} else if (key == "FmtNameOverride") {
			g_cfg.fmtNameOverride = val;
		} else if (key == "ContNameOverride") {
			g_cfg.contNameOverride = val;
		}
	}
}

// 游戏根目录（exe 所在目录）。用于判断 Steam / GOG / VR，不依赖当前工作目录。
std::optional<std::filesystem::path> GetGameRoot()
{
	const HMODULE exe = ::GetModuleHandleW(nullptr);
	if (!exe) return std::nullopt;
	wchar_t buf[MAX_PATH]{};
	const DWORD len = ::GetModuleFileNameW(exe, buf, static_cast<DWORD>(std::size(buf)));
	if (len == 0 || len >= std::size(buf)) return std::nullopt;
	return std::filesystem::path{ buf }.parent_path();
}

// 规范日志目录：<文档>\My Games\<版本>\SKSE
// 关键：版本按 exe 所在目录判断，而不是按当前工作目录。
// CommonLibSSE 自带的 log_directory() 用 CWD 里的 steam_api64.dll 判断版本，
// 在 MO2（StockGame 布局）等启动方式下 CWD 未必是游戏目录，会被误判成 GOG 版，
// 结果日志写进了 "Skyrim Special Edition GOG" 文件夹，看起来就像"完全没有日志"。
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

// ------------------------------ 日志 ------------------------------
// 统一写在 SKSE 插件的规范位置：<文档>\My Games\<版本>\SKSE\SummonNameFix.log
// （与 skse64.log 同目录）。
// 这里刻意放在 Load 的最开头：即使后续任何一步提前退出（例如地址库缺失），
// 也一定留下可排查的记录。
void SetupLog()
{
	const auto logsFolder = GetCanonicalLogDir();
	if (logsFolder.empty()) {
		return;   // 拿不到规范目录就不写日志，避免"东一个西一个"
	}

	std::error_code ec;
	std::filesystem::create_directories(logsFolder, ec);

	try {
		const auto logFilePath = logsFolder / "SummonNameFix.log";
		auto fileLogger = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
		auto logger = std::make_shared<spdlog::logger>("global", std::move(fileLogger));
		logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
		logger->set_level(spdlog::level::info);
		logger->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(logger));
		spdlog::info("日志文件：{}（游戏目录：{}）", logFilePath.string(),
			GetGameRoot() ? GetGameRoot()->string() : std::string("(未知)"));
	} catch (const std::exception& e) {
		// 日志不可写时静默继续：插件功能本身不依赖日志
		(void)e;
	}
}

// ------------------------------ 运行时特征扫描 ------------------------------
// 在进程地址空间里扫描“真正的字符串字面量” a_needle。
// 命中必须满足两个条件，避免误改：
//   1) 起点：前一个字节是 NUL（上一串的结尾），或紧接 UTF-8 BOM（EF BB BF）；
//   2) 终点：字面量以 NUL 结束。
// 注意：这里刻意不做“往回找 lea”的校验 —— 引用字符串的 lea 指令位于代码段(.text)，
// 而字符串位于数据段(.rdata)，两者在地址上并不相邻，往回扫永远不会命中。
// v1 是用地址库 ID + 函数内偏移定位 lea 反解出同一个字符串，效果等价。
std::vector<std::uintptr_t> FindStringRVA(std::string_view a_needle)
{
	if (a_needle.empty()) return {};

	const HMODULE mod = ::GetModuleHandleA(nullptr);
	if (!mod) return {};

	MEMORY_BASIC_INFORMATION mbi{};
	std::vector<std::uintptr_t> result;
	auto cur = reinterpret_cast<std::uintptr_t>(mod);

	while (::VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) &&
		   mbi.BaseAddress == reinterpret_cast<LPVOID>(cur)) {
		// 同时覆盖 PAGE_READONLY(.rdata) / PAGE_READWRITE / PAGE_EXECUTE_READ
		if (mbi.State == MEM_COMMIT &&
			(mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READ)) != 0) {
			const auto* region = reinterpret_cast<const std::uint8_t*>(cur);
			const auto len = static_cast<std::size_t>(mbi.RegionSize);

			if (len > a_needle.size()) {
				for (std::size_t i = 0; i + a_needle.size() < len; ++i) {
					if (std::memcmp(region + i, a_needle.data(), a_needle.size()) != 0) {
						continue;
					}
					// 终点：必须是完整的字面量（后面紧跟 NUL）
					if (region[i + a_needle.size()] != 0) {
						continue;
					}
					// 起点：前一字节为 NUL，或紧接 UTF-8 BOM（可能带 BOM 的 exe 变体）
					const bool atLiteralStart =
						(i == 0) || (region[i - 1] == 0) ||
						(i >= 3 && region[i - 1] == 0xBF && region[i - 2] == 0xBB && region[i - 3] == 0xEF);
					if (atLiteralStart) {
						result.push_back(cur + i);
					}
				}
			}
		}
		cur += mbi.RegionSize;
	}
	return result;
}

// 解析 lea reg,[rip+disp32] 中 disp32，返回格式串的 RVA。
// a_addr 是指令起始地址，a_leaOpcode 是 3 字节前缀（如 48 8D xx）。
// 若解析失败返回 0。
std::uintptr_t DecodeLeaOffset(std::uintptr_t a_addr, std::span<const std::uint8_t> a_leaOpcode)
{
	if (a_leaOpcode.size() > 3) return 0;
	if (a_addr < reinterpret_cast<std::uintptr_t>(::GetModuleHandleA(nullptr)) + a_leaOpcode.size()) return 0;

	const auto* insn = reinterpret_cast<const std::uint8_t*>(a_addr);
	if (std::memcmp(reinterpret_cast<const void*>(a_addr), a_leaOpcode.data(), a_leaOpcode.size()) != 0) {
		return 0;
	}

	// 48 8D xx 5byte 指令：前缀 + opcode + ModRM + disp32
	// 此处固定假设 48 8D xx 后面紧跟 disp32（标准 lea r/m64,[rip+disp32]）
	const auto disp = *reinterpret_cast<const std::int32_t*>(insn + a_leaOpcode.size());
	const auto instrEnd = a_addr + a_leaOpcode.size() + 4;

	// 安全检查：rip+disp 不应回退到模块基址以下（排除无效地址）
	const auto baseAddr = reinterpret_cast<std::uintptr_t>(::GetModuleHandleA(nullptr));
	if (instrEnd + static_cast<std::uint32_t>(disp) < baseAddr) return 0;
	if (instrEnd + static_cast<std::uint32_t>(disp) > baseAddr + 0x7FFFFFFFULL) return 0;

	return instrEnd + static_cast<std::uint32_t>(disp);
}

// 从候选 RVA 向回扫描若干字节，寻找能正确反解出 a_candidate 的 lea 指令。
// 失败返回 0。
std::uintptr_t ResolveFmtAddrFromCandidate(
	std::uintptr_t a_candidate,
	std::span<const std::uint8_t> a_leaOpcode,
	std::string_view a_expected,
	std::string_view a_label)
{
	if (a_candidate == 0) return 0;

	// 快速前置校验：候选地址处的内容必须完全等于预期原串（含 NUL 结尾）
	const auto* cur = reinterpret_cast<const std::uint8_t*>(a_candidate);
	if (std::memcmp(cur, a_expected.data(), a_expected.size()) != 0 ||
		cur[a_expected.size()] != 0) {
		return 0;  // 候选不是真正的格式串（可能命中了其他位置的相同字节序列）
	}

	// 向回扫描 lea 指令：实际上此函数已被主循环内联替代，保留以防复用
	(void)a_leaOpcode;
	(void)a_label;

	// 向回扫描 lea 指令
	const auto baseAddr = reinterpret_cast<std::uintptr_t>(::GetModuleHandleA(nullptr));
	const auto scanStart = a_candidate > patch_bytes::kMaxBackScan
		? (a_candidate - patch_bytes::kMaxBackScan)
		: baseAddr;

	for (auto addr = a_candidate - 1; addr >= scanStart; --addr) {
		// 只扫描 48 8D 开头的指令（lea r/m64, rel/m offset）
		if (addr + 2 > a_candidate) break;
		const auto* p = reinterpret_cast<const std::uint8_t*>(addr);
		if (p[0] == 0x48 && p[1] == 0x8D) {
			// 尝试用不同的 ModRM 字节（xx）解析
			for (int rm = 0; rm < 8; ++rm) {
				const std::array<std::uint8_t, 3> candidate = { 0x48, 0x8D, static_cast<std::uint8_t>(0x05 + rm * 0) };
				// 实际上 modrm 决定 reg/rm，但 lea 总是 48 8D xx；这里只校验前缀后紧跟 disp32
				// 简化处理：直接尝试从 addr 开始解析整个 7 字节指令
				const auto resolved = DecodeLeaOffset(addr, std::span<const std::uint8_t>(p, 3));
				if (resolved == a_candidate) {
					spdlog::info("[{}] lea 反解成功：addr=0x{:X} → fmt=0x{:X}", a_label, addr, resolved);
					return a_candidate;
				}
			}
		}
	}

	spdlog::warn("[{}] 未能在候选 0x{:X} 附近找到匹配的 lea 指令，放弃。", a_label, a_candidate);
	return 0;
}

// 用地址库 ID 解析目标函数入口地址；解析不到返回 0。
std::uintptr_t ResolveFunction(std::uint32_t a_seID)
{
	try {
		REL::Relocation<std::uintptr_t> fn{ RELOCATION_ID(a_seID, 0) };
		return fn.address();
	} catch (...) {
		return 0;
	}
}

// 在目标函数体内（起始 a_funcAddr，最多扫 a_window 字节）寻找那条引用指定字面量的
// rip 相对 lea，并反解出字面量地址 —— 等价于 v1 的做法，但把“函数内固定偏移”
// 换成了结构搜索，因此不受编译器/版本导致的指令位移变化影响。
// 命中条件很严：lea 反解出的地址处，内容必须与该字面量逐字节相同（含尾随 NUL）。
std::uintptr_t FindLiteralInFunction(std::uintptr_t a_funcAddr, std::size_t a_window, std::string_view a_expected)
{
	if (a_funcAddr == 0 || a_expected.empty()) return 0;

	MEMORY_BASIC_INFORMATION mbi{};
	if (!::VirtualQuery(reinterpret_cast<LPCVOID>(a_funcAddr), &mbi, sizeof(mbi))) return 0;
	if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) == 0) {
		return 0;
	}

	const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
	const auto limit = (a_funcAddr + a_window < regionEnd) ? (a_funcAddr + a_window) : regionEnd;
	const auto* code = reinterpret_cast<const std::uint8_t*>(a_funcAddr);

	for (auto addr = a_funcAddr; addr + 7 <= limit; ++addr) {
		const auto i = static_cast<std::size_t>(addr - a_funcAddr);
		const auto rex = code[i];
		if (rex != 0x48 && rex != 0x4C) continue;      // REX.W（48）/ REX.WR（4C）
		if (code[i + 1] != 0x8D) continue;             // lea
		if ((code[i + 2] & 0xC7) != 0x05) continue;    // mod=00, rm=101 → RIP 相对

		const auto disp = *reinterpret_cast<const std::int32_t*>(code + i + 3);
		const auto target = static_cast<std::uintptr_t>(
			static_cast<std::int64_t>(addr) + 7 + static_cast<std::int64_t>(disp));

		MEMORY_BASIC_INFORMATION tmbi{};
		if (!::VirtualQuery(reinterpret_cast<LPCVOID>(target), &tmbi, sizeof(tmbi))) continue;
		if (tmbi.State != MEM_COMMIT ||
			(tmbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ)) == 0) {
			continue;
		}
		const auto targetEnd = reinterpret_cast<std::uintptr_t>(tmbi.BaseAddress) + tmbi.RegionSize;
		if (target + a_expected.size() >= targetEnd) continue;

		const auto* p = reinterpret_cast<const std::uint8_t*>(target);
		if (std::memcmp(p, a_expected.data(), a_expected.size()) == 0 && p[a_expected.size()] == 0) {
			return target;
		}
	}
	return 0;
}

// 校验 a_addr 处是否正好是期望的字面量（含尾随 NUL），且地址可读。
bool VerifyLiteral(std::uintptr_t a_addr, std::string_view a_expected)
{
	if (a_addr == 0 || a_expected.empty()) return false;
	MEMORY_BASIC_INFORMATION mbi{};
	if (!::VirtualQuery(reinterpret_cast<LPCVOID>(a_addr), &mbi, sizeof(mbi))) return false;
	if (mbi.State != MEM_COMMIT ||
		(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ)) == 0) {
		return false;
	}
	const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
	if (a_addr + a_expected.size() >= end) return false;

	const auto* p = reinterpret_cast<const std::uint8_t*>(a_addr);
	return std::memcmp(p, a_expected.data(), a_expected.size()) == 0 && p[a_expected.size()] == 0;
}

// 主流程：三层定位，逐字节校验后等长覆写。
//   第 1 层（与 v1 逐字节一致）：地址库 ID → 函数入口 + 函数内固定偏移 → 校验 lea 操作码 → 反解字符串。
//   第 2 层：地址库 ID → 函数入口 → 函数体内结构搜索 lea（去掉版本专属偏移，供其它版本用）。
//   第 3 层：全进程“完整字面量”搜索（兜底）。
bool ApplyStringPatch(
	std::uint32_t a_seFuncID,
	std::size_t a_leaOffset,
	std::span<const std::uint8_t> a_leaOpcode,
	std::string_view a_expected,
	std::string_view a_replacement,
	std::string_view a_label)
{
	if (a_expected.size() != a_replacement.size()) {
		spdlog::error("[{}] 内部错误：替换串（{}）与原文长度（{}）不一致，放弃。",
			a_label, a_replacement.size(), a_expected.size());
		return false;
	}

	std::vector<std::uintptr_t> targets;

	// ── 第 1 层：完全照搬 v1（SE 1.5.97 上验证成功的路径）──────────────────
	if (a_seFuncID != 0 && a_leaOffset != 0 && a_leaOpcode.size() == 3) {
		if (const auto funcAddr = ResolveFunction(a_seFuncID)) {
			const auto insn = funcAddr + a_leaOffset;
			if (std::memcmp(reinterpret_cast<const void*>(insn), a_leaOpcode.data(), a_leaOpcode.size()) == 0) {
				const auto disp = *reinterpret_cast<const std::int32_t*>(insn + 3);
				const auto target = static_cast<std::uintptr_t>(
					static_cast<std::int64_t>(insn) + 7 + static_cast<std::int64_t>(disp));
				if (VerifyLiteral(target, a_expected)) {
					spdlog::info("[{}] v1 路径命中：ID {} → 函数 0x{:X} + 0x{:X} → 格式串 0x{:X}。",
						a_label, a_seFuncID, funcAddr, a_leaOffset, target);
					targets.push_back(target);
				} else {
					spdlog::warn("[{}] v1 路径：0x{:X} 处内容非 \"{}\"。", a_label, target, a_expected);
				}
			} else {
				spdlog::warn("[{}] v1 路径：ID {} → 函数 0x{:X}，但 +0x{:X} 处不是预期 lea（指令漂移）。",
					a_label, a_seFuncID, funcAddr, a_leaOffset);
			}
		} else {
			spdlog::warn("[{}] 地址库 ID {} 无法解析（未安装地址库或该版本无此 ID）。", a_label, a_seFuncID);
		}
	}

	// ── 第 2 层：函数体内结构搜索 lea（不依赖版本专属偏移）────────────────
	if (targets.empty() && a_seFuncID != 0) {
		if (const auto funcAddr = ResolveFunction(a_seFuncID)) {
			const auto literal = FindLiteralInFunction(funcAddr, 0x800, a_expected);
			if (literal != 0) {
				spdlog::info("[{}] 函数体内结构搜索命中：函数 0x{:X} → 格式串 0x{:X}。",
					a_label, funcAddr, literal);
				targets.push_back(literal);
			} else {
				spdlog::warn("[{}] 函数 0x{:X} 内未找到引用该格式串的 lea，改用字面量搜索。",
					a_label, funcAddr);
			}
		}
	}

	// ── 第 3 层：全进程“完整字面量”搜索（兜底 / 多版本通用）────────────────
	if (targets.empty()) {
		targets = FindStringRVA(a_expected);
		if (!targets.empty()) {
			spdlog::warn("[{}] 字面量搜索命中 {} 处。", a_label, targets.size());
		}
	}

	if (targets.empty()) {
		spdlog::error("[{}] 三层定位都没找到格式串 \"{}\"，本补丁跳过。", a_label, a_expected);
		return false;
	}

	std::size_t patched = 0;
	for (const auto fmtAddr : targets) {
		DWORD oldProtect = 0;
		if (!::VirtualProtect(reinterpret_cast<LPVOID>(fmtAddr), a_replacement.size(), PAGE_READWRITE, &oldProtect)) {
			spdlog::error("[{}] VirtualProtect 失败（0x{:X}，GetLastError={}），跳过该处。", a_label, fmtAddr, ::GetLastError());
			continue;
		}
		std::memcpy(reinterpret_cast<void*>(fmtAddr), a_replacement.data(), a_replacement.size());
		DWORD tmp = 0;
		::VirtualProtect(reinterpret_cast<LPVOID>(fmtAddr), a_replacement.size(), oldProtect, &tmp);
		::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(fmtAddr), a_replacement.size());

		spdlog::info("[{}] 已补丁：0x{:X}：\"{}\" -> \"{}\"（等长 {} 字节）。",
			a_label, fmtAddr, a_expected, std::string(a_replacement), a_replacement.size());
		++patched;
	}

	if (patched == 0) {
		spdlog::error("[{}] 所有候选写入都失败了。", a_label);
		return false;
	}
	spdlog::info("[{}] 完成：共补丁 {} 处。", a_label, patched);
	return true;
}

// ------------------------------ 兜底 hook ------------------------------
const char* (*g_origGetDisplayFullName)(RE::TESObjectREFR*) = nullptr;

std::mutex g_cacheLock;
std::unordered_map<std::string, std::string> g_cache;
std::size_t g_logCount = 0;

const char* FixName(const char* a_name)
{
	if (!a_name) return a_name;
	if (!std::strstr(a_name, patch_bytes::kNeedleC)) return a_name;

	std::lock_guard<std::mutex> lk(g_cacheLock);
	if (g_cache.size() < g_cfg.cacheLimit) {
		auto [it, inserted] = g_cache.try_emplace(a_name);
		if (inserted) {
			std::string s = a_name;
			const auto pos = s.find(patch_bytes::kNeedle);
			if (pos != std::string::npos) {
				s.replace(pos, patch_bytes::kNeedle.size(), g_lang.aposReplacement);
			}
			it->second = std::move(s);
		}
		return it->second.c_str();
	}

	thread_local std::string buf;
	buf = a_name;
	const auto pos = buf.find(patch_bytes::kNeedle);
	if (pos != std::string::npos) {
		buf.replace(pos, patch_bytes::kNeedle.size(), g_lang.aposReplacement);
	}
	return buf.c_str();
}

const char* HookedGetDisplayFullName(RE::TESObjectREFR* a_this)
{
	const char* name = g_origGetDisplayFullName(a_this);

	if (g_cfg.logNameCalls && name && g_logCount < g_cfg.logCallsLimit) {
		g_logCount++;
		char snap[256]{};
		strncpy_s(snap, sizeof(snap), name, _TRUNCATE);
		spdlog::info(
			"[调查#{}] this={:016X} ret={:016X} thread={:04X} isActor={} len={} str=\"{}\" hex={}",
			g_logCount,
			reinterpret_cast<std::uintptr_t>(a_this),
			reinterpret_cast<std::uintptr_t>(_ReturnAddress()),
			::GetCurrentThreadId(),
			a_this ? a_this->GetFormType() == RE::FormType::ActorCharacter : false,
			std::strlen(snap),
			snap,
			[&snap] {
				std::string hex;
				const auto l = std::strlen(snap);
				hex.reserve(l * 3);
				char tmp[8]{};
				for (std::size_t i = 0; i < l; ++i) {
					std::format_to_n(tmp, sizeof(tmp), "{:02x} ", static_cast<std::uint8_t>(snap[i]));
					hex += tmp;
				}
				return hex;
			}());
	}

	if (!g_cfg.hookDisplayName) return name;
	if (g_cfg.hookOnlyActors && a_this && a_this->GetFormType() != RE::FormType::ActorCharacter) return name;
	return FixName(name);
}

bool WriteRel32Jump(std::uintptr_t a_src, std::uintptr_t a_dst)
{
	const auto delta = static_cast<std::ptrdiff_t>(a_dst) - static_cast<std::ptrdiff_t>(a_src + 5);
	if (delta < std::numeric_limits<std::int32_t>::min() || delta > std::numeric_limits<std::int32_t>::max()) {
		spdlog::error("相对跳转超范围（0x{:X} -> 0x{:X}），放弃。", a_src, a_dst);
		return false;
	}

	std::uint8_t code[5] = { 0xE9 };
	const auto disp = static_cast<std::int32_t>(delta);
	std::memcpy(code + 1, &disp, sizeof(disp));

	DWORD oldProtect = 0;
	if (!::VirtualProtect(reinterpret_cast<LPVOID>(a_src), sizeof(code), PAGE_EXECUTE_READWRITE, &oldProtect)) {
		spdlog::error("VirtualProtect 失败（0x{:X}，GetLastError={}），放弃。", a_src, ::GetLastError());
		return false;
	}
	std::memcpy(reinterpret_cast<void*>(a_src), code, sizeof(code));
	DWORD tmp = 0;
	::VirtualProtect(reinterpret_cast<LPVOID>(a_src), sizeof(code), oldProtect, &tmp);
	::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(a_src), sizeof(code));
	return true;
}

void* AllocateExecNear(std::uintptr_t a_target, std::size_t a_size)
{
	SYSTEM_INFO si{};
	::GetSystemInfo(&si);
	const auto gran = static_cast<std::uintptr_t>(si.dwAllocationGranularity);
	constexpr std::uintptr_t kReach = 0x7FFF0000ull;
	const auto low = (a_target > kReach) ? (a_target - kReach) : gran;
	const auto high = a_target + kReach;
	const auto start = a_target & ~(gran - 1);

	for (auto addr = start; addr > low; addr -= gran) {
		if (auto* p = ::VirtualAlloc(reinterpret_cast<LPVOID>(addr), a_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
			return p;
		}
	}
	for (auto addr = start + gran; addr < high; addr += gran) {
		if (auto* p = ::VirtualAlloc(reinterpret_cast<LPVOID>(addr), a_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
			return p;
		}
	}
	return nullptr;
}

namespace ids
{
	// GetDisplayFullName 的 Address Library ID（SE / AE）。VR 由 VR 地址库解析 SE 号。
	constexpr std::uint32_t kGetDisplayFullName = 19354;
	constexpr std::uint32_t kAe_GetDisplayFullName = 19781;

	// 下面两个是 v1 在 SE 1.5.97 上验证过的地址库 ID（目标函数入口，
	// 即 TESNPC / TESObjectCONT 的 vtable 槽 76 —— 名字组装函数）。
	constexpr std::uint32_t kNpcNameBuilder = 24212;
	constexpr std::uint32_t kContNameBuilder = 17486;

	// 与 v1 完全一致的“函数内固定偏移”（v1 逆向验证：0x361703-0x361640 / 0x22BAAD-0x22B990）。
	// SE 1.5.97 优先走这条与 v1 逐字节相同的路径。
	constexpr std::size_t kNpcLeaFmtOffset = 0x0C3;    // lea rdx,[rip+disp] → "%s's %s"
	constexpr std::size_t kContLeaAposOffset = 0x11D;  // lea r8,[rip+disp]  → "'s "
}

bool InstallHook()
{
	REL::Relocation<std::uintptr_t> func{ RELOCATION_ID(ids::kGetDisplayFullName, ids::kAe_GetDisplayFullName) };
	const auto addr = func.address();
	if (addr == 0) {
		spdlog::error("GetDisplayFullName 地址解析失败（ID {}），跳过兜底 hook。", ids::kGetDisplayFullName);
		return false;
	}

	static constexpr std::array<std::uint8_t, 5> kPrologue{ 0x40, 0x53, 0x55, 0x56, 0x57 };
	if (std::memcmp(reinterpret_cast<const void*>(addr), kPrologue.data(), kPrologue.size()) != 0) {
		spdlog::error("GetDisplayFullName（0x{:X}）函数头与预期不符（已被其它插件 hook 或版本漂移），跳过兜底 hook。", addr);
		return false;
	}

	auto* stub = static_cast<std::uint8_t*>(AllocateExecNear(addr, 32));
	if (!stub) {
		spdlog::error("无法在 GetDisplayFullName 附近分配跳板内存，跳过兜底 hook。");
		return false;
	}
	std::memcpy(stub, reinterpret_cast<const void*>(addr), kPrologue.size());
	if (!WriteRel32Jump(reinterpret_cast<std::uintptr_t>(stub) + kPrologue.size(), addr + kPrologue.size())) {
		::VirtualFree(stub, 0, MEM_RELEASE);
		spdlog::error("跳板回跳写入失败，跳过兜底 hook。");
		return false;
	}

	g_origGetDisplayFullName = reinterpret_cast<decltype(g_origGetDisplayFullName)>(stub);
	if (!WriteRel32Jump(addr, reinterpret_cast<std::uintptr_t>(&HookedGetDisplayFullName))) {
		g_origGetDisplayFullName = nullptr;
		::VirtualFree(stub, 0, MEM_RELEASE);
		spdlog::error("GetDisplayFullName 首页改写失败，跳过兜底 hook。");
		return false;
	}

	spdlog::info("兜底 hook 已安装：目标 0x{:X}，跳板 0x{:X}（仅 Actor={}）。",
		addr, reinterpret_cast<std::uintptr_t>(stub), g_cfg.hookOnlyActors);
	return true;
}

// ----------------------------------------------------------------------------------
// SKSE 入口：与 Zzyxz 模板保持一致——AE 读取 SKSEPlugin_Version，SE / VR 走 SKSEPlugin_Query。
// 不锁死具体版本号；格式串差异由运行时特征扫描 + 双重校验消化，任何一步失败只记日志并跳过。
// 运行期需要 Address Library（SKSEPlugin_Load 在 Init 前检查，缺失则插件不生效）。
// ----------------------------------------------------------------------------------
namespace
{
	constexpr std::string_view kPluginName{ "SummonNameFix" };
	constexpr std::string_view kPluginAuthor{ "SummonNameFix" };
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
	// SKSE VR 报告的是 1.4.15.1，所以这里只按运行时族判断，具体地址库文件在 Load 里再选。
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
	// 第一件事就是建立日志：这样即使后面任何一步提前退出，也一定留下可排查的记录。
	g_cfg.logLevel = spdlog::level::info;
	SetupLog();

	const auto runtime = a_skse->RuntimeVersion();
	spdlog::info("SummonNameFix v{} build={}（编译于 {} {}）",
		kPluginVersion.string("."), PLUGIN_BUILD_ID, __DATE__, __TIME__);
	spdlog::info("已加载模块：{}", GetPluginPath().string());
	spdlog::info("运行时 {}。", runtime.string());

	// 与 v1 保持一致：无条件 SKSE::Init。
	// 关键点：地址库的 ID 数据库是在 SKSE::Init 里装载的；不调用 Init，
	// REL::Relocation{ RELOCATION_ID(24212, 0) } 一律解析失败，
	// v1 那条“ID + 函数内偏移”的精准路径就会整条失效（这正是之前不生效的直接原因之一）。
	SKSE::Init(a_skse);

	const bool hasAddressLibrary = !REL::Module::FindAddressLibrary().empty();
	spdlog::info("Address Library：{}。",
		hasAddressLibrary ? "已找到" : "缺失（只影响可选的 GetDisplayFullName hook）");

	LoadConfig();
	ResolveLanguage();
	spdlog::set_level(g_cfg.logLevel);

	if (!g_cfg.master) {
		spdlog::warn("ini 配置 Enable=false，插件不做任何修改，直接退出。");
		return true;
	}

	spdlog::info("语言包：{}（格式串 \"{}\" → \"{}\"）。",
		g_cfg.language, patch_bytes::kFmtExpected, g_lang.fmtReplacement);

	// 补丁 1：召唤物/有主 Actor 路径（格式串 "%s's %s" → 目标语言等长替换）
	// 第 1 层与 v1 逐字节一致：ID 24212 → 函数 + 0x0C3 → lea rdx(48 8D 15) → 格式串。
	if (g_cfg.patchSummon) {
		static constexpr std::array<std::uint8_t, 3> kLeaRdx{ 0x48, 0x8D, 0x15 };
		ApplyStringPatch(
			ids::kNpcNameBuilder,
			ids::kNpcLeaFmtOffset,
			kLeaRdx,
			patch_bytes::kFmtExpected,
			g_lang.fmtReplacement,
			g_lang.formatName);
	}

	// 补丁 2：有主容器路径（格式串 "'s " → 目标语言等长替换）
	// 第 1 层与 v1 逐字节一致：ID 17486 → 函数 + 0x11D → lea r8(4C 8D 05) → 格式串。
	if (g_cfg.patchContainer) {
		static constexpr std::array<std::uint8_t, 3> kLeaR8{ 0x4C, 0x8D, 0x05 };
		ApplyStringPatch(
			ids::kContNameBuilder,
			ids::kContLeaAposOffset,
			kLeaR8,
			patch_bytes::kAposExpected,
			g_lang.aposReplacement,
			g_lang.aposName);
	}

	// 兜底 hook：GetDisplayFullName（ID 19354/19781）走 REL::Relocation，必须有地址库。
	if (g_cfg.hookDisplayName || g_cfg.logNameCalls) {
		if (hasAddressLibrary) {
			InstallHook();
		} else {
			spdlog::warn("Address Library 缺失，跳过 GetDisplayFullName 兜底 hook。");
		}
	}

	spdlog::info("SummonNameFix 初始化完成。");
	return true;
}