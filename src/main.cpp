// SummonNameFixCN —— SKSE 插件（目标运行时：Skyrim SE 1.5.97）
//
// 功能：修复中文汉化环境下召唤物 / 有主容器等名称中未翻译的英文所有格 "'s"。
//   召唤物：  "本怡's 骷髅战士" -> "本怡的骷髅战士"
//   有主容器："Sven's Chest"   -> "Sven的Chest"
//
// 原理（详见 REPORT.md，全部经静态逆向验证）：
//   1) TESNPC 槽76 虚函数（Address Library SE ID 24212）用格式串 "%s's %s"
//      拼出 "所有者's 名字"，该串全 exe 仅此一处引用；
//      补丁："%s's %s"(7字节) -> "%s的%s"（"的"=E7 9A 84，UTF-8 下同为 7 字节）。
//   2) TESObjectCONT 槽76 虚函数（SE ID 17486）用独立串 "'s "（3字节）strcat 拼接，
//      全 exe 仅此一处引用；补丁："'s " -> "的"（等长 3 字节）。
//   3) 兜底（可选，ini 控制）：hook TESObjectREFR::GetDisplayFullName（SE ID 19354，
//      NG 已含偏移声明），对返回结果含 "'s " 的名字做缓存化替换。
//
// 硬性约束：不依赖 ESP/ESL/ESM/Papyrus，不修改存档；版本不匹配时写日志并拒绝加载。

// 注意：CommonLibSSE-NG 3.5.3 没有 RE/RE.h 聚合头；按实际类型包含具体头
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
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

// ----------------------------------------------------------------------------------
// 逆向验证常量。来源：Address Library version-1-5-97-0.bin + capstone 反汇编 + RTTI，
// 详见仓库内 REPORT.md。除 NG 已声明偏移（19354/19781）外，AE 侧 ID 未经逆向往证，
// 一律填 0；本插件经 COMPATIBLE_RUNTIMES 门禁仅在 SE 1.5.97 加载。
// ----------------------------------------------------------------------------------
namespace ids
{
	constexpr std::uint32_t kGetDisplayFullName = 19354;  // RE::TESObjectREFR::GetDisplayFullName（NG: RELOCATION_ID(19354, 19781)）
	constexpr std::uint32_t kAe_GetDisplayFullName = 19781;

	constexpr std::uint32_t kNpcNameBuilder = 24212;      // TESNPC 虚表槽76：召唤物/有主 Actor 名字组装（"%s's %s"）
	constexpr std::size_t kNpcLeaFmtOffset = 0x0C3;       // 函数内 lea rdx,[rip+disp] 指令偏移（0x361703-0x361640）

	constexpr std::uint32_t kContNameBuilder = 17486;     // TESObjectCONT 虚表槽76：有主容器名字组装（"'s " strcat）
	constexpr std::size_t kContLeaAposOffset = 0x11D;     // 函数内 lea r8,[rip+disp] 指令偏移（0x22BAAD-0x22B990）
}

namespace patch_bytes
{
	// "%s's %s"  ->  "%s的%s"     （7 字节 -> 7 字节，"的" = U+7684 = \xE7\x9A\x84）
	constexpr std::string_view kFmtExpected{ "%s's %s", 7 };
	constexpr std::string_view kFmtReplacement{ "%s\xE7\x9A\x84%s", 7 };

	// "'s "      ->  "的"          （3 字节 -> 3 字节）
	constexpr std::string_view kAposExpected{ "'s ", 3 };
	constexpr std::string_view kAposReplacement{ "\xE7\x9A\x84", 3 };

	// 兜底 hook 的 strstr 快速判断针（含尾随空格；拼接处必为 "'s "）
	constexpr std::string_view kNeedle{ "'s ", 3 };
	// strstr 需要的是 NUL 结尾串：直接用字面量指针，与 kNeedle 内容一致
	constexpr const char* kNeedleC = "'s ";
}

namespace
{
	// ------------------------------ 配置 ------------------------------
	struct Config
	{
		bool master = true;             // Enable：总开关
		bool patchSummon = true;        // PatchSummonPath："%s's %s" -> "%s的%s"
		bool patchContainer = true;     // PatchContainerPath："'s " -> "的"
		bool hookDisplayName = true;    // HookDisplayName：兜底 hook GetDisplayFullName
		bool hookOnlyActors = true;     // HookOnlyActors：兜底 hook 仅处理 Actor（FormType==ActorCharacter）
		std::size_t cacheLimit = 4096;  // CacheLimit：兜底缓存上限，超限后走 thread_local 降级
		spdlog::level::level_enum logLevel = spdlog::level::info;
		bool logNameCalls = false;      // LogNameCalls：调查模式，记录 GetDisplayFullName 返回值详情
		std::size_t logCallsLimit = 200;// LogCallsLimit：调查日志最大条数
	};

	Config g_cfg;

	bool ParseBool(std::string_view a_val, bool a_default)
	{
		if (a_val.empty()) {
			return a_default;
		}
		switch (a_val.front()) {
		case '1':
		case 't':
		case 'T':
		case 'y':
		case 'Y':
			return true;
		case '0':
		case 'f':
		case 'F':
		case 'n':
		case 'N':
			return false;
		default:
			return a_default;
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

	std::filesystem::path GetPluginPath()
	{
		HMODULE self = nullptr;
		::GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&GetPluginPath),
			&self);
		char buf[MAX_PATH]{};
		::GetModuleFileNameA(self, buf, static_cast<DWORD>(std::size(buf)));
		return { buf };
	}

	// 极简 ini 读取：仅支持 key=value 与 ;/# 注释，节名忽略；缺文件/缺键用默认值。
	void LoadConfig()
	{
		const auto iniPath = GetPluginPath();
		iniPath.replace_extension(".ini");

		std::ifstream file(iniPath);
		if (!file.is_open()) {
			spdlog::info("未找到配置文件 {}，使用默认配置。", iniPath.string());
			return;
		}

		std::string line;
		while (std::getline(file, line)) {
			const auto comment = line.find_first_of(";#");
			if (comment != std::string::npos) {
				line.resize(comment);
			}
			const auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			const auto key = line.substr(0, eq);
			auto val = line.substr(eq + 1);
			const auto keyB = key.find_last_not_of(" \t\r\n");
			const auto valB = val.find_last_not_of(" \t\r\n");
			if (keyB == std::string::npos || valB == std::string::npos) {
				continue;
			}
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
				if (const auto v = ParseSize(val)) {
					g_cfg.cacheLimit = *v;
				}
			} else if (key == "LogLevel") {
				if (auto lvl = spdlog::level::from_str(val); lvl != spdlog::level::off || val == "off") {
					g_cfg.logLevel = lvl;
				}
			} else if (key == "LogNameCalls") {
				g_cfg.logNameCalls = ParseBool(val, g_cfg.logNameCalls);
			} else if (key == "LogCallsLimit") {
				if (const auto v = ParseSize(val)) {
					g_cfg.logCallsLimit = *v;
				}
			}
		}
	}

	// ------------------------------ 日志 ------------------------------
	void SetupLog()
	{
		auto logsFolder = SKSE::log::log_directory();
		if (!logsFolder) {
			SKSE::stl::report_and_fail("SKSE 未提供 log_directory，无法初始化日志。"sv);
		}
		auto pluginName = SKSE::PluginDeclaration::GetSingleton()->GetName();
		auto logFilePath = *logsFolder / std::format("{}.log", pluginName);
		auto fileLogger = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
		auto logger = std::make_shared<spdlog::logger>("global", std::move(fileLogger));
		spdlog::set_default_logger(std::move(logger));
		spdlog::set_level(g_cfg.logLevel);
		spdlog::flush_on(spdlog::level::info);
	}

	// ------------------------------ 等长字符串补丁 ------------------------------
	// 从目标函数（地址库 ID）内已知偏移处的 lea reg,[rip+disp32] 反解出字符串地址，
	// 逐字节校验原文后再等长覆写。任何一步不符即放弃该补丁（版本漂移防护），绝不离错改写。
	bool ApplyStringPatch(std::uint32_t a_funcID, std::size_t a_leaOffset,
		std::span<const std::uint8_t> a_leaOpcode,
		std::string_view a_expected, std::string_view a_replacement,
		std::string_view a_label)
	{
		if (a_expected.size() != a_replacement.size()) {
			spdlog::error("[{}] 内部错误：替换串与原文长度不一致，放弃。", a_label);
			return false;
		}

		REL::Relocation<std::uintptr_t> func{ RELOCATION_ID(a_funcID, 0) };
		const auto funcAddr = func.address();
		if (funcAddr == 0) {
			spdlog::error("[{}] 地址库 ID {} 无法解析（未安装 Address Library 或版本不符），放弃补丁。", a_label, a_funcID);
			return false;
		}

		const auto insn = funcAddr + a_leaOffset;
		// 校验 lea 指令操作码（0x361703: 48 8D 15；0x22BAAD: 4C 8D 05）
		if (std::memcmp(reinterpret_cast<const void*>(insn), a_leaOpcode.data(), a_leaOpcode.size()) != 0) {
			spdlog::error("[{}] 0x{:X} 处指令与预期 lea 不符（版本漂移？），放弃补丁。", a_label, insn);
			return false;
		}

		const auto disp = *reinterpret_cast<const std::int32_t*>(insn + 3);
		const auto target = insn + a_leaOpcode.size() + 4 + static_cast<std::uint32_t>(disp);

		// 逐字节校验原串（含结尾 NUL），防误写
		const auto* cur = reinterpret_cast<const std::uint8_t*>(target);
		if (std::memcmp(cur, a_expected.data(), a_expected.size()) != 0 ||
			cur[a_expected.size()] != 0) {
			spdlog::error("[{}] 0x{:X} 处内容非 \"{}\"，放弃补丁。", a_label, target, a_expected);
			return false;
		}

		DWORD oldProtect = 0;
		if (!::VirtualProtect(reinterpret_cast<LPVOID>(target), a_replacement.size(), PAGE_READWRITE, &oldProtect)) {
			spdlog::error("[{}] VirtualProtect 失败（GetLastError={}），放弃补丁。", a_label, ::GetLastError());
			return false;
		}
		std::memcpy(reinterpret_cast<void*>(target), a_replacement.data(), a_replacement.size());
		DWORD tmp = 0;
		::VirtualProtect(reinterpret_cast<LPVOID>(target), a_replacement.size(), oldProtect, &tmp);

		spdlog::info("[{}] 已补丁：0x{:X}：\"{}\" -> 替换完成（等长 {} 字节）。", a_label, target, a_expected, a_replacement.size());
		return true;
	}

	// ------------------------------ 兜底 hook ------------------------------
	const char* (*g_origGetDisplayFullName)(RE::TESObjectREFR*) = nullptr;

	std::mutex g_cacheLock;
	// 节点地址稳定（rehash 不移动节点，且从不删除），返回 c_str() 安全。
	std::unordered_map<std::string, std::string> g_cache;
	std::size_t g_logCount = 0;

	const char* FixName(const char* a_name)
	{
		if (!a_name) {
			return a_name;
		}
		// 廉价快速退出：绝大多数调用不含 "'s "，原指针原样返回，零分配。
		if (!std::strstr(a_name, patch_bytes::kNeedleC)) {
			return a_name;
		}

		std::lock_guard<std::mutex> lk(g_cacheLock);
		if (g_cache.size() < g_cfg.cacheLimit) {
			auto [it, inserted] = g_cache.try_emplace(a_name);
			if (inserted) {
				std::string s = a_name;
				const auto pos = s.find(patch_bytes::kNeedle);
				if (pos != std::string::npos) {
					s.replace(pos, patch_bytes::kNeedle.size(), patch_bytes::kAposReplacement);  // 只替换第一个
				}
				it->second = std::move(s);
			}
			return it->second.c_str();
		}

		// 缓存超限：thread_local 缓冲降级（每线程独立，无竞争）。
		thread_local std::string buf;
		buf = a_name;
		const auto pos = buf.find(patch_bytes::kNeedle);
		if (pos != std::string::npos) {
			buf.replace(pos, patch_bytes::kNeedle.size(), patch_bytes::kAposReplacement);
		}
		return buf.c_str();
	}

	const char* HookedGetDisplayFullName(RE::TESObjectREFR* a_this)
	{
		const char* name = g_origGetDisplayFullName(a_this);

		if (g_cfg.logNameCalls && name && g_logCount < g_cfg.logCallsLimit) {
			g_logCount++;
			// 拷贝快照，避免日志期间数据被改
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
					char tmp[8];
					for (std::size_t i = 0; i < l; ++i) {
						std::format_to_n(tmp, sizeof(tmp), "{:02x} ", static_cast<std::uint8_t>(snap[i]));
						hex += tmp;
					}
					return hex;
				}());
		}

		if (!g_cfg.hookDisplayName) {
			return name;
		}
		if (g_cfg.hookOnlyActors && a_this && a_this->GetFormType() != RE::FormType::ActorCharacter) {
			return name;
		}
		return FixName(name);
	}

	bool InstallHook()
	{
		REL::Relocation<std::uintptr_t> func{ RELOCATION_ID(ids::kGetDisplayFullName, ids::kAe_GetDisplayFullName) };
		const auto addr = func.address();
		if (addr == 0) {
			spdlog::error("GetDisplayFullName 地址解析失败（ID {}），跳过兜底 hook。", ids::kGetDisplayFullName);
			return false;
		}
		// 函数头前 5 字节为 40 53 55 56 57（push rbx/rbp/rsi，3 条完整指令，无 rip-relative），可安全重定位。
		SKSE::AllocTrampoline(64);
		auto& trampoline = SKSE::GetTrampoline();
		g_origGetDisplayFullName = stl::unrestricted_cast<decltype(g_origGetDisplayFullName)>(
			trampoline.write_branch<5>(addr, HookedGetDisplayFullName));
		if (!g_origGetDisplayFullName) {
			spdlog::error("兜底 hook 安装失败，跳过。");
			return false;
		}
		spdlog::info("兜底 hook 已安装于 0x{:X}（GetDisplayFullName，仅 Actor={}）。", addr, g_cfg.hookOnlyActors);
		return true;
	}
}

// ----------------------------------------------------------------------------------
// SKSE 入口。插件声明（名称/版本/兼容运行时）由 add_commonlibsse_plugin 在编译期生成：
// COMPATIBLE_RUNTIMES 1.5.97 —— 其他运行时 SKSE 直接拒绝加载本插件，天然安全降级。
// ----------------------------------------------------------------------------------
SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	// 先以默认日志级别建日志，再读 ini 调整级别
	g_cfg.logLevel = spdlog::level::info;
	SetupLog();
	LoadConfig();
	spdlog::set_level(g_cfg.logLevel);

	spdlog::info("SummonNameFixCN v{} 加载中（仅支持 Skyrim SE 1.5.97）。",
		SKSE::PluginDeclaration::GetSingleton()->GetVersion().string("/"));

	if (!g_cfg.master) {
		spdlog::warn("ini 配置 Enable=false，插件不做任何修改，直接退出。");
		return true;  // 正常退出，不影响游戏
	}

	// 运行时版本二次确认（与生成声明一致才继续；不匹配则拒绝加载）
	const auto runtime = REL::Module::get().version();
	if (runtime != REL::Version(1, 5, 97, 0)) {
		spdlog::error("检测到运行时版本 {}，非 1.5.97，插件拒绝加载（安全退出）。",
			runtime.string("-"));
		return false;
	}

	// 补丁 1：召唤物/有主 Actor 路径（TESNPC 槽76，ID 24212）
	if (g_cfg.patchSummon) {
		static constexpr std::array<std::uint8_t, 3> kLeaRdx{ 0x48, 0x8D, 0x15 };
		ApplyStringPatch(ids::kNpcNameBuilder, ids::kNpcLeaFmtOffset, kLeaRdx,
			patch_bytes::kFmtExpected, patch_bytes::kFmtReplacement, "召唤物路径");
	}

	// 补丁 2：有主容器路径（TESObjectCONT 槽76，ID 17486）
	if (g_cfg.patchContainer) {
		static constexpr std::array<std::uint8_t, 3> kLeaR8{ 0x4C, 0x8D, 0x05 };
		ApplyStringPatch(ids::kContNameBuilder, ids::kContLeaAposOffset, kLeaR8,
			patch_bytes::kAposExpected, patch_bytes::kAposReplacement, "有主容器路径");
	}

	// 兜底 hook：GetDisplayFullName（ID 19354）
	if (g_cfg.hookDisplayName || g_cfg.logNameCalls) {
		InstallHook();
	}

	spdlog::info("SummonNameFixCN 初始化完成。");
	return true;
}
