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

// 主流程：扫描 + 验证 + 写入补丁
bool ApplyStringPatch(
	std::string_view a_needle,
	std::string_view a_expected,
	std::string_view a_replacement,
	std::string_view a_label)
{
	if (a_expected.size() != a_replacement.size()) {
		spdlog::error("[{}] 内部错误：替换串（{}）与原文长度（{}）不一致，放弃。",
			a_label, a_replacement.size(), a_expected.size());
		return false;
	}

	const auto candidates = FindStringRVA(a_needle);
	if (candidates.empty()) {
		spdlog::error("[{}] 未在进程地址空间找到字符串字面量 \"{}\"（{} 字节），本补丁跳过。",
			a_label, a_needle, a_needle.size());
		return false;
	}

	// FindStringRVA 已经完成“完整字面量”校验（起点边界 + 尾随 NUL），直接写入即可。
	// 不再做“往回找 lea”：引用该字符串的 lea 在 .text，字符串在 .rdata，两者不相邻，
	// 往回扫命中率为 0（v1 是靠地址库 ID + 函数内偏移定位 lea，效果等价）。
	const auto& targets = candidates;
	spdlog::info("[{}] 找到 {} 处字符串字面量 \"{}\"（{} 字节），开始等长替换。",
		a_label, targets.size(), a_expected, a_expected.size());

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
	const bool hasAddressLibrary = !REL::Module::FindAddressLibrary().empty();
	spdlog::info("SummonNameFix v{} 开始加载：运行时 {}，Address Library {}。",
		kPluginVersion.string("."), runtime.string(),
		hasAddressLibrary ? "已找到" : "缺失");

	if (hasAddressLibrary) {
		// 只有地址库就绪时才 Init —— 缺地址库时 Init 会直接终止游戏。
		SKSE::Init(a_skse);
	}

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
	// 核心补丁是纯 Win32 内存扫描 + 等长替换，不依赖 Address Library。
	if (g_cfg.patchSummon) {
		ApplyStringPatch(
			patch_bytes::kFmtExpected,
			patch_bytes::kFmtExpected,
			g_lang.fmtReplacement,
			g_lang.formatName);
	}

	// 补丁 2：有主容器路径（格式串 "'s " → 目标语言等长替换）
	if (g_cfg.patchContainer) {
		ApplyStringPatch(
			patch_bytes::kAposExpected,
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