# SummonNameFix

跨版本（SE / AE / VR）SKSE 插件：修复召唤物与有主容器名称中硬编码的英文所有格 `'s`，支持中文/日文/韩文/德语等多语言。

- 召唤物：`本怡's 骷髅战士` → `本怡的骷髅战士`（中文）、`本怡の骷髅战士`（日文）等
- 有主容器：`Sven's Chest` → `Sven的Chest` / `SvenのChest` 等

纯 SKSE 插件：**不依赖任何 ESP/ESL/ESM，不依赖 Papyrus 脚本，不修改存档**。

## 安装

1. 安装 [SKSE64](https://skse.silverlock.org/) 与 [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)。
2. 根据游戏版本下载对应工件（SE/AE/VR），将 `SummonNameFix.dll` 与 `SummonNameFix.ini` 放入 `Data/SKSE/Plugins/`。
3. 在 `SummonNameFix.ini` 中设置 `Language`（`zh` / `ja` / `ko` / `de` 或自定义），重启游戏。

日志输出至 `Documents/My Games/Skyrim Special Edition/SKSE/SummonNameFix.log`。

## 工作原理（概要）

`'s` 并非可翻译字符串（GMST / STRINGS 中均无此串），而是硬编码在引擎两个
「表单虚表槽 76」名字组装函数中的运行时拼接结果：

| 路径 | 机制 | 原文 |
|---|---|---|
| 召唤物 / 有主 Actor | TESNPC 虚表槽 76（运行时动态定位） | `"%s's %s"` |
| 有主容器 | TESObjectCONT 虚表槽 76（运行时动态定位） | `"'s "` |

**跨版本设计**：本插件不依赖任何特定运行时版本的静态 RVA 或 Address Library ID（除 hook 用 19354/19781 外）。补丁流程：

1. 扫描进程地址空间，寻找英文原串（`"%s's %s"` 或 `"'s "`）的 RVA。
2. 向回扫描若干字节，寻找 `lea reg,[rip+disp32]` 指令，反解格式串地址。
3. **双重校验**：lea 指令指向候选地址 + 候选处字节与原串完全一致（含 NUL 结尾）。
4. 等长覆写为语言包对应字符串。

任何一步失败即记录日志并跳过，保证版本漂移或格式串变体时不会误写内存。

另含可选兜底 hook：`TESObjectREFR::GetDisplayFullName`（ID 19354/19781，Address Library），
对返回值含 `'s ` 的名字做缓存化替换，覆盖残余路径（Papyrus SetDisplayName 等）。

详细逆向证据见 [REPORT.md](REPORT.md)。

## 构建

```powershell
git clone <本仓库>
cd SummonNameFixCN

cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md

cmake --build build
# 产物：build/SummonNameFix.dll
```

无本地环境时可使用 GitHub Actions 云编译：push 后在 Actions 页下载 `SummonNameFix-SE` / `SummonNameFix-AE` / `SummonNameFix-VR` 工件。

## 配置（SummonNameFix.ini）

| 键 | 默认 | 说明 |
|---|---|---|
| `Enable` | `true` | 总开关 |
| `Language` | `zh` | 语言包：`zh`（中文）、`ja`（日文）、`ko`（韩文）、`de`（德语） |
| `CustomFmtReplacement` | （空） | 自定义格式化串替换（覆盖 Language），必须等长 |
| `CustomAposReplacement` | （空） | 自定义所有格串替换，必须等长 |
| `PatchSummonPath` | `true` | 召唤物/有主 Actor 路径格式串补丁 |
| `PatchContainerPath` | `true` | 有主容器路径串补丁 |
| `HookDisplayName` | `false` | 兜底 hook（GetDisplayFullName） |
| `HookOnlyActors` | `true` | 兜底 hook 仅处理 Actor，避免误改物品名 |
| `CacheLimit` | `4096` | 兜底缓存上限，超限走 thread_local 降级 |
| `LogLevel` | `info` | 日志级别 |
| `LogNameCalls` | `false` | 调查模式：记录返回值字节/调用地址/线程 ID |
| `LogCallsLimit` | `200` | 调查日志条数上限 |

## 兼容性与安全设计

- **跨运行时门禁**：`COMPATIBLE_RUNTIMES 1.5.97 1.6.620 1.4.34`，非这些版本由 SKSE 拒绝加载。
- **运行时特征定位**：不使用静态 RVA，通过 `VirtualQuery` + `memcmp` 扫描 + lea 反解定位格式串，自动适配所有包含相同原串的运行时。
- **双重校验**：lea 指向 + 原串字节匹配，任一项不符即放弃。
- **等长替换**：替换串必须与原串字节数相同（否则自动跳过），保证不破坏内存布局。
- 无 `'s ` 的名字走 `strstr` 快速返回，零内存分配。
- 兜底缓存：`mutex + unordered_map`（节点地址稳定、只增不删），返回 `c_str()` 安全。

## 多语言说明

预设语言包的替换串均经过字节长度核对，确保与原串等长：

| 语言 | Format 替换（%s's %s → ?） | Apos 替换（'s  → ?） | 字节数 |
|---|---|---|---|
| 中文（zh） | `%s的%s`（7 字节） | `的`（3 字节） | 匹配 |
| 日文（ja） | `%sの%s`（7 字节） | `の`（3 字节） | 匹配 |
| 韩文（ko） | `%s의%s`（7 字节） | `의`（3 字节） | 匹配 |
| 德语（de） | `%s's %s`（不变） | `'s `（不变） | 匹配 |

自定义语言：在 ini 中填写 `CustomFmtReplacement=` / `CustomAposReplacement=`，
值可为 Unicode 字符串（UTF-8 编码），但**必须与原串等长**，否则补丁自动跳过。

## 许可

MIT（CommonLibSSE-NG 为其自有许可）。
