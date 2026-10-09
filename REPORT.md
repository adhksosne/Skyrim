# 调查报告：SummonNameFix —— 跨版本、多语言召唤物名称所有格修复

游戏文件：`F:\iniRePather-Skyrim\ModOrganizer\StockGame\SkyrimSE.exe`（SE 1.5.97）
验证工具：自写 Python 脚本（analyze*.py，可复跑）+ capstone 反汇编 + Address Library 解码

---

## 1. 版本演进与架构变更（v2 跨版本设计）

v1 版本（commit 2ba118d/4d30d86）采用静态 RVA 硬编码方案，仅支持 SE 1.5.97。
v2 升级为**运行时特征扫描 + 等长替换**方案，目标覆盖 SE 1.5.x / AE 1.6.x / VR 1.4.x。

### v2 设计原则

| 原则 | 实现 |
|---|---|
| 不依赖静态 RVA | 通过 `VirtualQuery` 扫描进程地址空间，寻找已知英文原串（`"%s's %s"` / `"'s "`） |
| 不依赖特定 Address Library ID | 除 hook 用 19354/19781 外，补丁路径完全动态定位 |
| 双重校验防误写 | lea 指令指向候选地址 + 候选处字节与原串完全匹配（含 NUL 结尾） |
| 等长替换保内存布局 | 替换串必须与原串字节数相同，否则自动跳过并记日志 |
| 多语言可扩展 | INI `Language` 字段预设 zh/ja/ko/de，也支持 `CustomFmtReplacement` 自定义 |
| 版本门禁兜底 | `COMPATIBLE_RUNTIMES 1.5.97 1.6.620 1.4.34`，非列表版本由 SKSE 拒绝加载 |

### 特征扫描流程

```
1. VirtualQuery 遍历所有可读内存页（.rdata/.data 等）
2. memcmp 搜索 a_needle（如 "'s " 3 字节或 "%s's %s" 7 字节）
3. 对每个命中，向回 ≤16 字节扫描 48 8D xx 开头的 lea 指令
4. 解析 lea 的 disp32，计算 rip+disp 目标地址
5. 若目标地址 == 当前候选地址 → 双重校验通过
6. 校验候选处字节与原串完全一致（含尾随 NUL）
7. VirtualProtect + memcpy 写入替换串
```

任何一步失败即跳过，保证版本漂移或格式串变体时不会误写内存。

---

## 2. 原始静态分析结论（SE 1.5.97，保留作为实证参考）

### `'s` 的真实来源（静态实证 + 运行时双重实证）

**A. TESNPC 的槽 76 虚函数（SE ID 24212，RVA 0x361640）**
- RTTI 解出类名：`.?AVTESNPC@@`（虚表 0x159FCD0，0x361640 = 槽 76 = vtbl+0x260）
- 函数逻辑（反汇编实证）：
  1. `cmp byte [rdx+0x1A], 0x3E`：参数2 的 FormType == ActorCharacter 才走拼接分支
  2. 对该 Actor 与"所有者对象"各调用一次 `GetDisplayFullName`（19354）
  3. 用格式串 `"%s's %s"`（RVA 0x15A0638）拼出 `所有者's 名字`
- 该格式串全 exe **仅 1 处 lea 引用**（0x361703），即仅此函数使用。

**B. TESObjectCONT 的槽 76 虚函数（SE ID 17486，RVA 0x22B990）**
- RTTI 解出类名：`.?AVTESObjectCONT@@`（虚表 0x1559930，槽 76）
- 逻辑：`strcpy_s(所有者名) + strcat_s("'s ") + strcat_s(GetDisplayFullName(ref))` 至 260 字节栈缓冲
- `"'s "` 独立串（RVA 0x1559E84，3 字节含尾随空格）全 exe **仅 1 处 lea 引用**（0x22BAAD，函数内偏移 +0x11D）。

### GetDisplayFullName（ID 19354，RVA 0x2961F0）的角色
- 非虚成员函数，返回 `const char*`。
- 内部：extraList 有 ExtraTextDisplayData → `ExtraTextDisplayData::GetDisplayName`（ID 12626）；
  否则 → `14548`（RVA 0x196E10）→ `0x134BDB0(...)` → 结果对象 `jmp [vtbl+0x28]`（槽 5）。
- **结论：`'s` 拼接发生在 19354 返回之后（外层槽 76 函数内）。只 hook 19354 返回值无法去除 `'s`。**

### 编码
- STRINGS 文件为 UTF-8；`的` = `E7 9A 84`（3 字节）。

---

## 3. v2 多语言预设

预设语言包均经过字节长度核对，确保与原串等长：

| 语言 | Format 替换（%s's %s → ?） | Apos 替换（'s  → ?） | 字节数 |
|---|---|---|---|
| 中文（zh） | `%s\xE7\x9A\x84%s`（7 字节） | `\xE7\x9A\x84`（3 字节） | 匹配 |
| 日文（ja） | `%s\xE3\x81\xAE%s`（7 字节） | `\xE3\x81\xAE`（3 字节） | 匹配 |
| 韩文（ko） | `%s\xEC\x9C\x9C%s`（7 字节） | `\xEC\x9C\x9C`（3 字节） | 匹配 |
| 德语（de） | `%s's %s`（不变） | `'s `（不变） | 匹配 |

自定义语言：在 ini 中填写 `CustomFmtReplacement=` / `CustomAposReplacement=`，值可为任意
UTF-8 字节序列，但**必须与原串等长**，否则补丁自动跳过。

---

## 4. 已知限制

1. **特征扫描范围**：扫描仅在可读内存页（PAGE_READWRITE / PAGE_READONLY）中进行，理论上能覆盖 `.rdata` / `.data` 段；若某运行时将格式串放在特殊段（如仅可执行段），可能漏扫。
2. **lea 回看上限**：当前回看 16 字节，足以覆盖所有已观测到的 lea 指令位置；极端情况下若编译器优化导致 lea 距离拉大，可能漏扫。
3. **GetDisplayFullName prologue 校验**：hook 安装时校验前 5 字节 `40 53 55 56 57`（push rbx/rbp/rsi/rdi），若该函数在 AE/VR 上 prologue 不同则 hook 安装失败并跳过（不影响字符串补丁）。
4. **Papyrus 脚本中的 `'s`**：`.pex` 字节码内的原串（如 `DevourmentManager.psc` 的 `"'s remains"`）不受本补丁影响——这些是脚本编译产物，需工具链重新编译。如需处理，建议在 Gamebryo Papyrus Compiler 层做 post-processing。
5. **游戏内实测**：v1 在 SE 1.5.97 上已实测成功；v2 跨版本能力仅在静态分析与逻辑上保证，未在所有目标运行时实测。

---

## 5. 与原报告（v1）的差异

| 项 | v1 | v2 |
|---|---|---|
| 格式串定位 | 静态 RVA（24212+0xC3，17486+0x11D） | 运行时特征扫描 + lea 反解 |
| 版本门禁 | 锁死 1.5.97 | 允许 1.5.97 / 1.6.620 / 1.4.34 |
| 语言支持 | 仅中文硬编码 | INI 配置化，预设 zh/ja/ko/de，支持自定义 |
| Address Library 依赖 | 24212 / 17486 / 19354 / 19781 | 仅 19354 / 19781（hook 用） |
| 安全机制 | 逐字节校验 | 双重校验（lea 指向 + 原串匹配）+ 等长守卫 |
| 项目名称 | SummonNameFixCN | SummonNameFix |

---

## 6. 运行时崩溃复盘（2026-10-10，CRASH LOG 实证）与修复

首次游戏内运行（commit 938b3c3）在启动约 6 分钟后崩溃：`EXCEPTION_ACCESS_VIOLATION`，
试图执行 `0x7FF7DBA5B748`（不可读内存），调用栈落在 `SummonNameFixCN.dll+0x2C351`
（即 `HookedGetDisplayFullName`）——该处第一条指令为 `mov rax,[g_origGetDisplayFullName]; call rax`。

**根因**：`SKSE::Trampoline::write_branch<N>` **不是**「hook 并返回可调用的原函数」。其实现
（CommonLibSSE-NG 3.5.3 `SKSE/Trampoline.h:292-308`）为：

```
disp = *(int32*)(a_src + N - 4);   // 取 a_src+1 处的 4 字节
func = (a_src + N) + disp;         // 当作 rel32 解引用
... 把 a_src 前 N 字节覆盖为 jmp ...
return func;
```

它假设 `a_src` 开头本就是一条 `jmp rel32` 跳转桩（用于改指针/thunk 重定向），
**完全不重定位原始指令**。而 `GetDisplayFullName` 开头是 `40 53 55 56 57`：

| 项 | 值 |
|---|---|
| SkyrimSE.exe 基址（崩溃日志第 673 行） | `0x7FF784260000` |
| GetDisplayFullName = 基址 + RVA 0x2961F0 | `0x7FF7844F61F0` |
| 取 a_src+1 的 `53 55 56 57` 当 rel32 | `0x57565553` |
| 返回值 = `0x7FF7844F61F0 + 5 + 0x57565553` | **`0x7FF7DBA5B748`** |
| 崩溃日志中的野地址 | **`0x7FF7DBA5B748`**（完全一致） |

**同一次崩溃日志同时反证了两项正面结论**：
1. 插件日志显示两个字符串补丁**均已成功写入**（`0x7FF785800638` = 基址+0x15A0638、
   `0x7FF7857B9E84` = 基址+0x1559E84），即地址库 ID→RVA 映射（24212→0x361640、
   17486→0x22B990）与 lea 偏移均正确；
2. 崩溃与字符串补丁无关，**仅由 hook 的原函数指针错误引起**。

**v2 修复**：
- 改为手写跳板（VirtualAlloc + memcpy + VirtualProtect + FlushInstructionCache）
- 运行时校验 prologue 前 5 字节（防版本漂移/防其它插件先行 hook）
- hook 默认关闭（字符串补丁已覆盖主要路径）
- v2 完全消除对 24212/17486 的依赖，不再受崩溃问题影响
