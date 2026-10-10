# 归属名本地化 · PossessiveLocalization

把引擎硬编码的英文所有格 `'s` 换成本地语言的所有格标记，让召唤物与有主容器的名字完整本地化。

| 场景 | 原文 | 中文 | 日文 | 韩文 |
|---|---|---|---|---|
| 召唤物 / 有主 Actor | `囚犯's 骷髅战士` | `囚犯的骷髅战士` | `囚犯の骷髅战士` | `囚犯의骷髅战士` |
| 有主容器 | `Sven's Chest` | `Sven的Chest` | `SvenのChest` | `Sven의Chest` |

严格说这不是 bug，而是引擎把英文所有格写死进了可执行文件，导致本地化版本里始终夹着一个 `'s`——属于**本地化不完整**。

纯 SKSE 插件：不依赖 ESP / ESL / ESM，不使用 Papyrus，不改动存档。

## 安装

1. 安装 [SKSE64](https://skse.silverlock.org/)。
2. 把 `PossessiveLocalization.dll` 和 `PossessiveLocalization.ini` 放进 `Data/SKSE/Plugins/`。
3. 在 ini 里把 `Language` 填成 `zh` / `ja` / `ko`，重启游戏。

同一个 DLL 同时支持 SE 1.5.x / AE 1.6.x / VR 1.4.x，不需要按版本挑文件。

## 配置

| 键 | 默认 | 说明 |
|---|---|---|
| `Language` | `zh` | `zh`=的、`ja`=の、`ko`=의 |
| `CustomAposReplacement` | （空） | 可选：自定义 **3 字节 UTF-8** 标记，覆盖 `Language` |

日志在
`文档\My Games\Skyrim Special Edition\SKSE\PossessiveLocalization.log`。

## 原理

`'s` 不在任何可翻译的数据里，而是编译进 `SkyrimSE.exe` 的两个字符串字面量：
`"%s's %s"`（召唤物 / 有主 Actor）与 `"'s "`（有主容器）。

插件启动时在 **exe 自身映像内按字节定位**这两个字面量，再用目标语言标记**等长覆写**。
不依赖 Address Library，也没有任何按版本写死的地址或偏移；定位不到、长度不符、内存不可写，
都只记日志并跳过，不会误写。

## 兼容性

| 版本 | 状态 |
|---|---|
| SE 1.5.97 | ✅ 已实测 |
| AE 1.6.x | ✅ 已实测 |
| VR 1.4.x | ⚠️ 未实测（设计上通用） |

VR 请以日志为准：未命中会记录并安全跳过。

## 已知边界

- 只处理**非拉丁文字**语言（英/德/法等本身用 `'s` 或词尾 `-s`，视觉上正常）。
- Papyrus `SetDisplayName` 等运行时自行构造的名字不经过上述字面量，不在处理范围内。
- 只做内存内等长替换：不写磁盘、不碰存档，卸载即恢复。

## 与其它 mod 的关系

同类实现最早见 powerof3 的 [Language Fixes](https://github.com/powerof3/LanguageFixes)——它从 2024 年起就修 NPC 与容器名字的所有格。两者解决的问题相同，取舍不同：

| | Language Fixes | 本插件 |
|---|---|---|
| 手段 | hook 引擎函数 + 正则后处理生成的字符串 | 改 exe 内两个字面量，等长覆写，不 hook |
| 依赖 | 需要 Address Library | 零依赖 |
| 版本文件 | SE / AE 分开构建 | 单个 DLL 通用 SE / AE / VR |
| 语言 | 9 种（德/法/西/葡为语法化输出） | 中 / 日 / 韩 + 任意 3 字节标记 |
| 语言选择 | 自动检测游戏语言 | ini 指定（游戏语言是英文时同样可用） |

两者**可共存**（本插件改完后字符串里不再含 `'s `，对方会直接跳过），但同时安装是冗余的，装一个即可。

## License

MIT。
