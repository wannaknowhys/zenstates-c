# ZenStates-C

[English version](README.md)

纯 C 写的 Boost 上限工具，面向 AMD Ryzen 3000 / 5000 / 7000 系列桌面 CPU（Zen 2 / Zen 3 / Zen 4），0.2.1。

它通过 HSMP 命令限制最高 Boost 频率。低于上限时 CPU 照样动态 Boost，待机时照样降频降压。这是一个上限（cap），不是定频锁：程序不设固定频率、不设固定电压、不设固定 VID。HSMP 只有一个 socket 级的 cap，所以 INI 里的单核与全核两项会合并成一个有效 cap：min(SingleCoreMaxMHz, AllCoreMaxMHz)。

## 支持的 CPU

| 代 | CPU | cap 支持 |
|---|---|---|
| Zen 2 | Matisse（Ryzen 3000）、Castle Peak（Threadripper 3000） | **走 HSMP 可用**（见“排障”）：RSMU 0x29/0x2B 在 SMU 46.73.0 上被拒，但 HSMP WriteBoostLimitAllCores（0x9）能设 cap——2026-11-16 已验证，接口版本 2，设 4000MHz，HSMP 读回 4000MHz，CPU 在 HWMonitor 里卡在约 4000MHz。RSMU 0x6E 仍读 fused 的 4200MHz（预期：0x6E 读 fused 上限，HSMP 是另一层 socket cap）。cap 持续到改动或重启 |
| Zen 3 | Vermeer（Ryzen 5000）、Chagall（Threadripper 5000）、Milan（EPYC 7003） | HSMP 0x9，尝试但未验证（和 Zen 2 同一邮箱同一消息）；接口版本检查在运行时决定 |
| Zen 4 | Raphael / Dragon Range（Ryzen 7000）、Genoa、Storm Peak | HSMP 0x9，尝试但未验证（和 Zen 2 同一邮箱同一消息）；接口版本检查在运行时决定 |
| Zen 1、Zen+、Rome、APU、Zen 5 | — | 不支持：来源表里根本没有 boost-limit 命令，程序探测到 CPU 后直接拒绝并说明原因，而不是发一条在那代上意思不同（或根本不存在）的命令 |

Family 17h Model 31h 有歧义：Rome（EPYC，不支持 cap）和 Castle Peak（Threadripper 3000，用 Matisse 表）共用同一个 CPUID。程序会按 Matisse 表尝试；HSMP 接口版本检查会做决定——SMU 没有实现 HSMP 的话，程序在设任何东西之前就停住（什么都没改，退出码 3）。

## 工作原理

- 频率上限：Zen 2/3/4 都用 HSMP 消息 `SetBoostLimitSocket`（0x9，socket 级，参数就是 MHz）——三代同一邮箱同一消息。设完用 HSMP `GetBoostLimit`（0xA，参数为 APIC ID）读回验证。功耗上限（只在 INI 里设了才发）：Zen 2/3 是 PPT/TDC/EDC（0x53/0x54/0x55），Zen 4 是 0x56/0x57/0x58，走 RSMU。原厂上限用 RSMU 0x6E 读（见下）。旧的 RSMU cap 码（0x29/0x2B、0x70）不再用了：0x29/0x2B 在当前 SMU 固件上被拒（见“排障”）。
- 原厂上限是读出来的，不是写死的：启动时、发任何 cap 之前，程序先读 0x6E `GetBoostLimitFrequency`——上游 `Cpu.GetFMax()` 用的就是这个调用。0x6E 读的是 fused 上限，HSMP socket cap 不会改动它，所以即使上一次跑的 cap 还在，它依然是正确的天花板。读到的值是 INI 上限的天花板；读失败或值离谱（<3000 或 >6000 MHz），程序在发任何 SET 命令之前就退出。不再有任何写死的频率。
- 电压自动跟着下来：Boost 模式下 SMU 按自己的 V/F 表查出对应频率的 VID。把频率 cap 住，SMU 就永远不会去要最高那档频率的 VID，最高电压也就跟着降下来。Boost 模式下没有单独 cap VID 的 SMU 命令；唯一能设 VID 的命令属于手动超频模式（固定 VID），用了就等于放弃 Boost，和 cap 的目标冲突。
- 什么都不写进 BIOS，重启后全部恢复原厂行为；想每次开机都应用，用下面的计划任务。

## 系统要求

- Windows 10 / 11，x64。
- 支持的 Ryzen 桌面 CPU（见上表），其他 CPU 会被拒绝，这是故意的安全限制。
- 另外安装 PawnIO 驱动（<https://pawnio.eu/>），本项目不带驱动。
- 运行 `zenstates-c.exe` / `zenstates-vf.exe` 需要管理员权限。

## 文件清单

| 文件 | 说明 |
|---|---|
| `zenstates-c.exe` | 主程序：读 INI，应用频率上限与功耗限制 |
| `zenstates-c-gui.exe` | 配置 GUI（纯 Win32）：只改 INI，不碰驱动 |
| `zenstates-vf.exe` | V/F 扫表工具：实测电压/频率曲线（见下文） |
| `zenstates-c.ini` | 主程序配置 |
| `zenstates-vf.ini` | 扫表工具配置 |
| `modules/RyzenSMU.bin`、`modules/AMDFamily17.bin` | 已签名的 PawnIO 模块（LGPL-2.1），运行时必需 |
| `install-autostart.bat` / `uninstall-autostart.bat` | 安装 / 卸载开机计划任务（需管理员运行） |
| `build.bat` / `build.sh` | 编译脚本（Windows MinGW / Linux 交叉编译） |
| `SLOW_MODE.md` | ZenStates slow mode 的说明，以及它为什么和 cap 无关 |
| `LICENSE`、`ATTRIBUTION.md` | GPLv3 许可全文；致谢与来源 |

## 安装

1. 从 Releases 页面下载 zip 并解压。所有文件保持在同一文件夹里——exe、ini、`modules/` 子目录缺一不可。任何文件夹都可以，没有强制安装路径。
2. 没装 PawnIO 驱动先从 <https://pawnio.eu/> 装上。
3. 先手动试一次：右键 `zenstates-c.exe` → 以管理员身份运行，看 `zenstates-c.log` 最后一行是不是 `OK: boost cap applied`。
4. 按下面“验证”核对 HWMonitor；没问题再右键 `install-autostart.bat` → 以管理员身份运行，装开机任务（任务名 `ZenStates-C`，开机时以 SYSTEM 身份运行，延迟 30 秒，应用完就退出）。`uninstall-autostart.bat` 只删任务；cap 本身重启即失效。

## 配置（`zenstates-c.ini`）

查找顺序：exe 旁边的 `zenstates-c.ini` 优先，其次 `%ProgramData%\ZenStates-C\zenstates-c.ini`。找不到文件不报错：记一条警告日志，用默认值继续。越界值会被钳到允许范围并记日志。

| 段 | 键 | 默认 | 允许范围 | 说明 |
|---|---|---|---|---|
| `[Boost]` | `SingleCoreMaxMHz` | 4000 | 3000–原厂上限（从 CPU 读出） | 单核 Boost 上限，MHz。有效 cap = min(单核, 全核)，见下注 |
| `[Boost]` | `AllCoreMaxMHz` | 3900 | 3000–原厂上限（从 CPU 读出） | 全核 Boost 上限，MHz（高于单核值时会被钳到单核值）。有效 cap = min(单核, 全核) |
| `[Power]` | `PPT_W` | （不设） | 45–300 | PPT，W（发给 SMU 时换算成 mW）。只在设了才发；不设就保持 BIOS 的值 |
| `[Power]` | `TDC_A` | （不设） | 30–200 | TDC，A（换算成 mA）。只在设了才发 |
| `[Power]` | `EDC_A` | （不设） | 45–300 | EDC，A（换算成 mA）。只在设了才发 |
| `[Startup]` | `AutoApply` | 1 | 0/1 | 是否装开机任务；主程序本身运行时永远应用配置，不看这个值 |
| `[Logging]` | `LogPath` | （空） | — | 空 = exe 目录下的 `zenstates-c.log`；计划任务以 SYSTEM 跑，不要填用户 profile 下的路径 |

功耗限制从 0.2.0 起是可选的：0.1.0 永远发 Ryzen 5 3600 的原厂值（88 W / 60 A / 90 A），在别的 CPU 上是错的，所以现在不设就不动。想锁死就按自己 CPU 的原厂值填（看 HWMonitor 或 BIOS）。0.1.0 写出的 INI 里这些键本来就有，所以老安装的行为不变。

关于 Boost 两项的注（0.2.1）：HSMP 只有一个 socket 级 cap，所以有效 cap 是 min(SingleCoreMaxMHz, AllCoreMaxMHz)。用上面的默认值（4000 / 3900），cap 就是 3900MHz。想要单个 4000MHz 的 cap，把两项都设成 4000（或 AllCoreMaxMHz=4000 且 SingleCoreMaxMHz 不低于 4000）。旧的 RSMU 两条命令分法（0x29 单核 / 0x2B 全核）已不存在。

读当前配置（0.2.2）：`zenstates-c.exe --read` 只读不应用——读 CPU 当前配置写进 INI：Boost 两项写当前 HSMP socket cap，功耗三项写成空 entry（Zen 2 上已应用的 PPT/TDC/EDC 读不回来，所以留空、不设）。GUI 里有“Read current from CPU”按钮做同一件事，跑完 GUI 会从 INI 重新读一遍刷新显示。功耗栏没设时显示为空（空值 = 保持 BIOS 原样）；保存时功耗栏留空会删掉对应键。

GUI 重写（0.2.3）：GUI 不再用每 tab 一个子窗口。旧版用 TabCtrl_AdjustRect 把一个“ZSC_Page”子窗口盖在 tab 显示区上，在用户那台机上这些 page 窗口一直没出现，导致每个 tab 都是空白（tab 条、Apply/Save/Close、状态栏都正常，只有 page 内容缺失）。现在所有控件都是主窗口的直接子窗口、按 tab 显示/隐藏，没有 page 窗口这一层可出问题。

GUI 布局（0.2.4）：窗口加宽到 560x400，标签都给够宽度、文字不再被截断（0.2.3 里“Single-core boost cap (MHz):”和“Log path (empty = exe dir):”被截断，是因为标签比文字窄、输入框起得太靠左）。另有一个 `zenstates-c-gui-xl.exe`，长宽和字体全部 x2（同一份源码，`-DGUI_SCALE=2` 编出来），想要更大布局的用它。

GUI 修复（0.2.5）：XL 版发货时 manifest 是坏的——它的 XML 注释里夹了 `--`，XML 规范禁止注释体内出现 `--`，所以 Windows 拒绝启动（“并行配置不正确”）。已把注释改写，五个 manifest 现都通过 XML 校验。另：两个频率 spinner（MHz）现在每点一下 ±25（UDN_DELTAPOS 改步长）；功耗 spinner（W/A）仍是 ±1。

GUI（`zenstates-c-gui.exe`）改同一份 INI：CPU/Boost、Power、Settings 三个 tab；Apply Now 保存 INI 后提权跑主程序；Save 只保存；Close 关闭。GUI 本身不用管理员权限，每个特权动作会弹一次 UAC。退出码：0 成功；1 驱动/模块/配置错误；2 不是管理员或 CPU 不支持；3 至少一条 SMU 命令失败。以日志为准。

## 验证

1. 跑之前先开 HWMonitor，记下原厂状态下的最高频率和最高电压（Ryzen 5 3600 上：约 4200 MHz 和约 1.45 V）。
2. 提权跑 `zenstates-c.exe`，日志里 HSMP socket cap 应显示 `SET TOOK EFFECT`（外加 INI 里设了的功耗限制）。
3. 再看 HWMonitor：最高频率应停在 cap 值（3600 用默认值：约 3900MHz，即 4000/3900 两者中低的那个），最高电压应降下来（3600：约 1.35 V），待机频率/电压仍会掉下来——这才是 cap 不是定频的证据。

如果 cap 没生效（日志里 HSMP 命令失败，或频率照冲原厂上限）：先确认 PawnIO 驱动已安装且版本与模块匹配、程序确实是管理员跑的，再看日志里失败的是哪一条命令。旧的 RSMU cap 路（0x29/0x2B）在当前固件上已死，不再用了：2026-11-16 已在真机上验证，Ryzen 5 3600、SMU 固件 46.73.0 拒绝了这两条（win32=1359），读回 0x6E 证实上限全程 4200 未变。首次跑完后 CPU 一度锁在约 800MHz / 0.6V，是一次性 transient，重启清除、重跑未复现（原因未明）；万一再出现，重启就是恢复手段。HSMP 接口版本查询（0x3）失败的话，说明这颗 CPU/固件没有实现 HSMP，程序在设任何东西之前就停住（退出码 3）。上游 2.0 的 Zen 2 表里根本没有 boost-limit 的 SET 命令（只有 0x6E 读），与 RSMU 这次拒绝一致。

cap 走 HSMP（WriteBoostLimitAllCores = 0x9，“Set socket maximum frequency level”，参数就是 MHz；Linux 内核 amd_hsmp.h；ZenStates-Core HSMPMailbox.cs）。Ryzen Master 能设全核 cap，而 HSMP 正是 AMD 官方接口为它留的邮箱。RSMU 模块内置的发命令函数到不了 HSMP（它写死了 RSMU），所以程序用模块的原始寄存器读写（ioctl_read/write_smu_register）重实现了通用邮箱协议（SMU.cs SendSmuCommand）来发 HSMP 0x9。它先查 HSMP 接口版本（0x3）；这颗 CPU 上没有 HSMP 的话就停住，什么都不设。2026-11-16 在 Ryzen 5 3600（SMU 46.73.0）上验证：HSMP 接口版本 2，设 4000MHz，HSMP 读回 4000MHz，CPU 在 HWMonitor 里卡在约 4000MHz。RSMU 0x6E 设完仍读 fused 的 4200MHz——预期，不是矛盾：0x6E 读 fused 上限，HSMP 是另一层 socket cap，SMU 取两者中低的执行。cap 持续到改动或重启。`zenstates-hsmp.exe` 是同一条 HSMP 路的独立探针（参数为目标 MHz；恢复用原厂上限，如 4200；0.2.1 已把它的“恢复”提示改成用 0x6E 的 fused 上限）。

## V/F 扫表（`zenstates-vf.exe`）

SMU 里没有“导出整张 V/F 表”的命令，所以这个工具改成实测：逐档把 Boost cap 设到目标频率，同时在一个核上跑标量整数负载让它真的顶到 cap，再读该核的 VID：MSR `0xC0010293`（HW_PSTATE_STATUS，Fid、DfsId、Vid 换算：频率 = 2×Fid/DfsId×100 MHz，电压 = 1.55 − Vid×0.00625 V，SVI2）。

用法：管理员跑 `zenstates-vf.exe`，默认从原厂上限（从 CPU 读出，RSMU 0x6E 的 fused 上限）扫到上限减 600 MHz，步进 100 MHz，每档稳定 1.5 秒后采 5 个样。每档设完上限后会读回 HSMP 0xA，日志里写明新上限是否真的生效。结果写 `zenstates-vf.csv`（每档一行采样，含读数核与负载核两列），每档的中位数就是该档的 V/F 点，日志里也会逐档打印中位数。读数核是负载核真正的 SMT 兄弟核（按 APIC ID 找，不是假定 LoadCore^1）。起止频率、步进、采样数、负载核都在 `zenstates-vf.ini` 里改。

注意：① 扫完会把 socket cap 恢复到扫之前 HSMP 0xA 读到的值，所以已经装了开机 cap 的话扫完它原样回来，扫表不破坏已有设置。② 扫表支持 Zen 2、Zen 3 和 Zen 4（HSMP 0x9 能压住任何一个被加载的核）；Zen 3 和 Zen 4 走 HSMP 未经验证。③ 测的是当前功耗限制下的实测曲线，PPT/TDC/EDC 顶住了，顶部几个点会比“纯曲线”低。④ BCLK 按 100 MHz 算。负载是纯标量整数循环（不用 AVX）。

压住一个核（0.2.2）：`zenstates-vf.exe --hold [核号]` 把标量负载绑在一个核上，每秒打印一次该核的频率和电压，按 Ctrl+C 停。它不设 cap——只是对当前已装的 cap 加负载，看 HWMonitor 确认 cap 在持续负载下真的压得住。扫表运行时本来就在跑同一条负载线程，`--hold` 是独立版。

## 从源码编译

Windows（MinGW-w64 gcc + windres，或 MSVC cl + rc）：跑 `build.bat`。Linux 交叉编译：`./build.sh`（MinGW-w64 gcc 13.2）。都产出四个 exe（`zenstates-c.exe`、`zenstates-c-gui.exe`、`zenstates-vf.exe`、`zenstates-hsmp.exe`），0 警告。每次 push / PR 都会在 `windows-latest` 上由 CI 编译（`.github/workflows/build.yml`）；push `v*` 标签会打出 Release zip（`release.yml`）。

## 限制

- SMU 命令来自逆向工程（AMD 没有公开文档），不同 BIOS/AGESA 版本行为可能不同。
- 只支持上表里的 Zen 2 / Zen 3 / Zen 4 桌面 CPU；Zen 1、Zen+、Rome、APU、Zen 5 直接拒绝。Zen 3 和 Zen 4 走 HSMP 未经验证。RSMU cap 路（0x29/0x2B）已在 SMU 固件 46.73.0（Ryzen 5 3600，2026-11-16）上观测到被拒，不再使用；cap 走 HSMP 0x9。
- 所有设置重启即失效（这是设计：重启就是恢复手段）。
- 真机第一次运行就是一次测试：按上面“验证”一节用 HWMonitor 核对。

## License / 致谢

- License：GPLv3，见 `LICENSE`。本项目是派生重写，不是 fork：没有复制 [irusanov/ZenStates](https://github.com/irusanov/ZenStates) 的任何代码，C 代码是从零写的，实现的是从上游 GPL 源码与反编译研究中得到的同一套 SMU 协议。完整致谢（用了什么、没用什么）见 `ATTRIBUTION.md`。
- `modules/` 里的两个 PawnIO 模块是 LGPL-2.1，未修改、已签名，来自 PawnIO.Modules 0.2.11。PawnIO 驱动本身不在本项目里发行，请从 <https://pawnio.eu/> 安装。
