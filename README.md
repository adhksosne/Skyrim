# SummonNameFixCN

Skyrim Special Edition（**仅 1.5.97**）SKSE 插件：修复中文汉化环境下召唤物 / 有主容器名称中未翻译的英文所有格 `'s`。

- 召唤物：`本怡's 骷髅战士` → `本怡的骷髅战士`
- 有主容器：`Sven's Chest` → `Sven的Chest`

纯 SKSE 插件：**不依赖任何 ESP/ESL/ESM，不依赖 Papyrus 脚本，不修改存档**。

## 安装

1. 安装 [SKSE64](https://skse.silverlock.org/) 与 [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)（`version-1-5-97-0.bin`）。
2. 将 `SummonNameFixCN.dll` 与 `SummonNameFixCN.ini` 放入 `Data/SKSE/Plugins/`。
3. 启动游戏（通过 SKSE 加载器）。

日志输出至 `Documents/My Games/Skyrim Special Edition/SKSE/SummonNameFixCN.log`。

## 工作原理（概要）

`'s` 并非可翻译字符串（GMST / STRINGS 中均无此串），而是硬编码在引擎两个
「表单虚表槽 76」名字组装函数中的运行时拼接结果：

| 路径 | 函数（SE Address Library ID） | 原文 | 补丁 |
|---|---|---|---|
| 召唤物 / 有主 Actor | 24212（TESNPC 虚表槽 76） | `"%s's %s"` | `"%s的%s"`（等长 7 字节） |
| 有主容器 | 17486（TESObjectCONT 虚表槽 76） | `"'s "` | `"的"`（等长 3 字节） |

两个原文串在全 exe 中各只有 1 处引用，且均只被对应函数使用。补丁流程：
从地址库解析函数地址 → 定位函数内 `lea reg,[rip+disp32]` → 反解字符串地址 →
**逐字节校验原文**（不匹配即放弃，防版本漂移）→ 等长覆写。

另含可选兜底 hook：`TESObjectREFR::GetDisplayFullName`（ID 19354，CommonLibSSE-NG
官方偏移），对返回结果含 `'s ` 的名字做缓存化替换，覆盖残余路径。所有行为可通过
ini 开关调整。

详细逆向证据、ID 表与未验证项见 [REPORT.md](REPORT.md)。

## 构建

依赖：Git、CMake ≥ 3.21、Visual Studio 2022（C++ 桌面开发）、vcpkg。

```powershell
git clone <本仓库>
cd SummonNameFixCN

# 依赖通过 vcpkg manifest + Color-Glass Studios registry 自动拉取（commonlibsse-ng）
cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md

cmake --build build
# 产物：build/SummonNameFixCN.dll
```

无本地环境时可使用 GitHub Actions 云编译：push 后在 Actions 页下载
`SummonNameFixCN-SE197` 工件（含 dll 与示例 ini）。

## 配置（SummonNameFixCN.ini）

| 键 | 默认 | 说明 |
|---|---|---|
| `Enable` | `true` | 总开关 |
| `PatchSummonPath` | `true` | 召唤物/有主 Actor 路径格式串补丁 |
| `PatchContainerPath` | `true` | 有主容器路径串补丁 |
| `HookDisplayName` | `true` | 兜底 hook（GetDisplayFullName） |
| `HookOnlyActors` | `true` | 兜底 hook 仅处理 Actor，避免误改物品名 |
| `CacheLimit` | `4096` | 兜底缓存上限，超限走 thread_local 降级 |
| `LogLevel` | `info` | 日志级别 |
| `LogNameCalls` | `false` | 调查模式：记录返回值字节/调用地址/线程 ID |
| `LogCallsLimit` | `200` | 调查日志条数上限 |

配置读取失败或缺省时使用默认值。

## 兼容性与安全设计

- 插件声明 `COMPATIBLE_RUNTIMES 1.5.97`：其他运行时由 SKSE 拒绝加载；加载后二次
  校验 `REL::Module` 版本，不匹配则写日志并安全退出。
- 无 `'s ` 的名字走 `strstr` 快速返回，零内存分配。
- 兜底缓存：`mutex + unordered_map`（节点地址稳定、只增不删），返回 `c_str()` 安全。
- 两个补丁均有逐字节校验，版本漂移时自动放弃并记录日志，不会误写内存。

## 许可

MIT（CommonLibSSE-NG 为其自有许可）。
