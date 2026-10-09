# 调查报告：Skyrim SE 1.5.97 召唤物名称 "'s" 来源与 hook 策略（静态分析结论）

游戏文件：`F:\iniRePather-Skyrim\ModOrganizer\StockGame\SkyrimSE.exe`（SE 1.5.97）
验证工具：自写 Python 脚本（analyze*.py，可复跑）+ capstone 反汇编 + Address Library 解码
所有 ID 均来自 Address Library (version-1-5-97-0.bin) 解码与 CommonLibSSE-NG 源码交叉验证。

## 1. `'s` 的真实来源（静态实证，游戏内实测未做）

### 排除项
- **GMST / STRINGS 翻译表**：英文与中文两套 STRINGS 文件共 26728 条，`%s's` 0 命中 → 排除。
- **ExtraTextDisplayData 常规路径**：召唤物名字并非存入该 extra data（其内容来自下列运行时拼接）。

### 真正来源：两个"槽 76 虚函数"（同一虚函数在不同表单类的覆写）

**A. TESNPC 的槽 76 虚函数（ID 24212，RVA 0x361640）** —— 召唤物/有主 Actor 路径
- RTTI 解出类名：`.?AVTESNPC@@`（虚表 0x159FCD0，0x361640 = 槽 76 = vtbl+0x260）
- 函数逻辑（反汇编实证）：
  1. `cmp byte [rdx+0x1A], 0x3E`：参数2 的 FormType == ActorCharacter 才走拼接分支
  2. 对该 Actor 与"所有者对象"各调用一次 `GetDisplayFullName`（19354）
  3. 用格式串 `"%s's %s"`（RVA 0x15A0638）拼出 `所有者's 名字`
- 该格式串全 exe **仅 1 处 lea 引用**（0x361703），即仅此函数使用。
- 另一分支用 `"%s - %s"`（0x15A0640），无 `'s`，不需处理。

**B. TESObjectCONT 的槽 76 虚函数（ID 17486，RVA 0x22B990）** —— 有主容器路径（"Sven's Chest"）
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

## 2. 最终 hook 策略（等长原位补丁 + 可选兜底 hook）

### 补丁 1（召唤物路径，核心）
- 目标：`"%s's %s"`（7 字节）→ `"%s的%s"`（UTF-8 下同为 7 字节）
- 地址推导（版本自适应）：`func_rva(RELOCATION_ID(se=24212)) + 0xC3` 处的 `lea rdx,[rip+disp]`
  → 字符串地址 = 指令地址+7+disp（指令 7 字节：48 8D 15 xx xx xx xx）
- 写入前**逐字节校验** `"%s's %s\0"`，不匹配则放弃该补丁并记日志（防版本漂移）。

### 补丁 2（有主容器路径）
- 目标：`"'s "`（3 字节）→ `"的"`（3 字节）
- 地址推导：`func_rva(RELOCATION_ID(se=17486)) + 0x11D` 处的 lea → 同上。
- 写入前校验 `"'s \0"`。

### 兜底 hook（可选，ini 开关）
- trampoline `write_branch<5>` 于 19354（prologue 前 5 字节 `40 53 55 56 57` = 3 条完整 push，无 rip-relative，可安全重定位）。
- 返回值含 `'s ` 时替换为 `的`（strstr 快速退出 + mutex + unordered_map 稳定节点缓存 + 上限）。
- 覆盖残余路径（如 Quest 运行时写入 ExtraTextDisplayData 的含 `'s ` 名字）。
- Actor 判定：`GetFormType() == FormType::ActorCharacter`（引擎在 24212 内同法判断，+0x1A==0x3E）。

### 为什么补丁优于 hook 槽 76
- 槽 76 是虚函数且输出参数类型未经完全验证（经 ID 10978/10979 的 printf 风格 helper 写入）；
  detour 虚函数需处理输出对象生命周期，风险更高。
- 等长补丁零运行时开销、零内存分配、同时覆盖两个路径、不触碰函数逻辑。

## 3. 已定位的其余格式串（不修改）
- `"%s\n%s\n%s%s"`（0x1557B18，4 xref）、`"%s\n%s"`（0x155151C，11 xref）：激活提示的偷窃/交互修饰。
- `"%s's current shout varia..."`、`"%s's %s: Animation contr..."`：调试/断言消息，与 UI 无关。

## 4. ID/偏移汇总（全部经地址库+反汇编双重验证）
| 名称 | SE ID | RVA | 说明 |
|---|---|---|---|
| TESObjectREFR::GetDisplayFullName | 19354 | 0x2961F0 | NG Offset:: 已含（RELOCATION_ID(19354,19781)） |
| TESNPC::槽76（召唤物 's 组装） | 24212 | 0x361640 | NG 无语义名 |
| TESObjectCONT::槽76（容器 's 组装） | 17486 | 0x22B990 | NG 无语义名 |
| ExtraTextDisplayData::GetDisplayName | 12626 | 0x13C740 | NG 已含 |
| 槽76 取名辅助 14548 | 14548 | 0x196E10 | NG 无语义名 |
| printf 风格格式化 helper | 10978/10979 | 0xF9E60/0xF9E90 | NG 无语义名 |
| IsCrimeToActivate | 19400 | 0x29A330 | 17486 内引用 |
| 字符串 A "%s's %s" | — | 0x15A0638 | 1 xref |
| 字符串 B "'s " | — | 0x1559E84 | 1 xref |
| 指令偏移：24212 内 lea A | — | +0xC3 | 0x361703-0x361640 |
| 指令偏移：17486 内 lea B | — | +0x11D | 0x22BAAD-0x22B990 |

**NG 中 SE 版 ID 与 AE 版 ID 的对应关系未验证**：24212/17486 的 AE ID 未知（NG 源码无此函数）。
本插件按任务书仅支持 SE 1.5.97，运行时版本门禁，其他版本拒绝加载。
（若未来要支持 AE，需要重新逆向 AE 侧 ID，静态推导不可靠。）

## 5. 未验证项 / 已知限制（诚实清单）
1. **游戏内实测全部未做**（无法在本机运行游戏）：准星/血条/TrueHUD/击杀提示/魔法效果列表各自是否经过 24212/17486/19354，需装插件开日志实测。静态结论：激活提示（准星名）确定走槽 76；普通血条大概率走 19354 直调（无 's）。
2. 补丁字节的**运行时校验**已设计，但补丁生效后的实际显示效果未在游戏内确认。
3. `"%s - %s"`（0x15A0640）分支的语义（cmded actor？）仅静态推断，未实测。
4. 槽 76 的完整调用方（哪些 UI 层调用 `call [vtbl+0x260]`）未逐一枚举；已确认 24212/17486 各自格式串仅 1 处 xref，风险受控。
5. TrueHUD 等第三方 HUD 的取名路径未验证（其用 CommonLibSSE GetDisplayFullName 的话即 19354，不含 's）。
6. 30 分钟稳定性、误改检查等测试清单项目需用户实测。
