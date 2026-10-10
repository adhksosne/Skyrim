# 归属名本地化 · OwnedNameLocalization

给「有主对象」的显示名补上本地化：把引擎**硬编码的英文所有格 `'s`** 换成本地语言的所有格标记。

| 场景 | 原文 | 中文（zh） | 日文（ja） | 韩文（ko） |
|---|---|---|---|---|
| 召唤物 / 有主 Actor | `本怡's 骷髅战士` | `本怡的骷髅战士` | `本怡の骷髅战士` | `本怡의骷髅战士` |
| 有主容器 | `Sven's Chest` | `Sven的Chest` | `SvenのChest` | `Sven의Chest` |

> 严格来说这**不是引擎缺陷（bug）**，而是引擎把英文所有格写死在可执行文件里、
> 没做本地化处理，导致本地化版本里始终夹着一个英文 `'s` —— 属于**本地化不完整**。
> 本插件的定位就是把这个缺口补上。

纯 SKSE 插件：**不依赖 ESP / ESL / ESM、不使用 Papyrus、不改动存档**。

---

## 安装

1. 安装 [SKSE64](https://skse.silverlock.org/)。
2. 把 `OwnedNameLocalization.dll` 与 `OwnedNameLocalization.ini` 放进 `Data/SKSE/Plugins/`（MO2 用户直接作为普通 mod 安装即可）。
3. 需要哪种语言，就在 ini 里把 `Language` 改成 `zh` / `ja` / `ko` 后重启游戏。

就这三步。**同一个 DLL 同时支持 SE 1.5.x / AE 1.6.x / VR 1.4.x**，不需要按版本挑文件。

## 配置（`OwnedNameLocalization.ini`）

只有一个有效配置项：

| 键 | 默认 | 说明 |
|---|---|---|
| `Language` | `zh` | 所有格标记：`zh`=的、`ja`=の、`ko`=의 |
| `CustomAposReplacement` | （空） | 可选：自定义 **3 字节 UTF-8** 标记，填写后覆盖 `Language` |

整份 ini 删掉也能用（默认中文）。日志写在
`文档\My Games\Skyrim Special Edition\SKSE\OwnedNameLocalization.log`。

## 工作原理

`'s` 并不存在于任何可翻译的数据里（GMST、STRINGS、ESP/ESM 中都没有），
它是引擎代码里两个**编译进 `SkyrimSE.exe` 的字符串字面量**：

| 路径 | 字面量 | 拼接方式 |
|---|---|---|
| 召唤物 / 有主 Actor 名字 | `"%s's %s"` | printf 式复合格式 |
| 有主容器名字 | `"'s "` | 独立串，strcat 追加 |

本插件的做法：

1. 启动时扫描 **exe 自身映像**，按字节精确定位这两个字面量（要求是完整字面量：此前一字节为 `NUL` 或 `BOM`，且以 `NUL` 结束）；
2. 用目标语言标记**等长覆写**（7 字节 → 7 字节，3 字节 → 3 字节），改完立即恢复内存页保护。

关键点：**不需要 Address Library、不需要任何按版本写死的地址或偏移**。
格式串由标记推导（`"%s" + 标记 + "%s"`，正好 7 字节），所以不同语言共用同一份代码。
定位到 0 处、长度不等、内存不可写等任何异常，都只记录日志并跳过，不会误写内存。

实测取证（SE 1.5.97）：`"%s's %s"` 与 `"'s "` 在全 exe 中各**仅出现 1 次**，
且前一字节均为 `0x00`，定位结果与早期逆向得出的地址完全一致。

## 兼容性

| 运行时 | 状态 |
|---|---|
| SE 1.5.97 | ✅ 已实测（中文、日文） |
| AE 1.6.x | ⚠️ 设计上通用（不依赖版本地址），但未实测 |
| VR 1.4.x | ⚠️ 同上，未实测 |

未实测的版本请以日志为准：命中会写「已补丁」，未命中会写「未在 exe 映像内找到字面量」并安全跳过。
另外，如果某个版本的游戏本体已经把该串本地化（不再含英文原串），插件也会安全地什么都不做。

**关于 Address Library**：本插件自身不使用它。但 SKSE 本体或你的其它插件可能要求安装，
那与本插件不冲突——装了不影响，没装也不影响本插件工作。

## 已知边界

- 只处理**非拉丁文字**语言。英/德/法等拉丁文字语言本身用 `'s` 或词尾 `-s`，视觉上属正常，故不做处理。
- 由 Papyrus `SetDisplayName` 等在运行时**自行构造**的名字不经过上述字面量，不在处理范围内。
- 仅做内存内等长替换，不写磁盘、不碰存档，卸载后立即恢复原状。

## 从源码构建

需要 vcpkg（manifest 模式）与 MSVC：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build build
# 产物：build/OwnedNameLocalization.dll
```

也可以直接用 GitHub Actions 云编译：push 后到 Actions 页下载 `OwnedNameLocalization` 工件。

## License

MIT（CommonLibSSE-NG 采用其自身许可）。

---

<details>
<summary>English</summary>

**OwnedNameLocalization** — an SKSE plugin that completes the localization of owned-object
display names by replacing the engine's hardcoded English possessive `'s`
(`"%s's %s"` and `"'s "`, both compiled into `SkyrimSE.exe`) with the possessive marker of
the target language: `zh` 的, `ja` の, `ko` 의.

- One DLL for SE / AE / VR. No ESP/ESM/Papyrus, no save edits, **no Address Library needed by this plugin**.
- Locates the two literals by scanning the executable's own image, then overwrites them
  in place with an equal-length replacement. No hardcoded addresses or offsets of any kind.
- Config is a single line: `Language=zh|ja|ko` (plus an optional 3-byte `CustomAposReplacement`).
- Any unexpected condition (0 matches, length mismatch, unwritable memory) is logged and skipped.

</details>
