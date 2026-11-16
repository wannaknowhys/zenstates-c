# Attribution / 致谢与来源

本项目是 **ZenStates 的派生重写（derivative rewrite）**，不是 fork：

- 没有 fork 上游仓库，没有复制上游的任何一行 C# 代码；
- `zenstates-c.c` / `zenstates-c-gui.c` 是从零用纯 C 重写的，不同语言、不同架构（单进程命令行程序 + Win32 GUI，而不是 .NET WinForms + Windows 服务）；
- 但它实现的是从上游 GPL 源码和反编译研究中得到的**同一套 SMU 协议**（消息码、mailbox 地址、PawnIO IOCTL 算术），且发行包里附带了 LGPL 的 PawnIO 模块二进制，因此整体按 GPLv3 发布（见 `LICENSE`）。这不是法律意见，只是基于 license 文本的判断。

## 用了什么 / 没用什么

### 1. irusanov/ZenStates — GPLv3

- 作者：Ivan Rusanov（GitHub [@irusanov](https://github.com/irusanov)）
- 仓库：<https://github.com/irusanov/ZenStates>
- License：GPLv3（本仓库的 `LICENSE` 即从该仓库 `LICENSE` 逐字复制）
- **用了**：SMU 协议的研究成果——Matisse（Zen2）的 mailbox 地址与消息码（旧版 0.x 的 `Zen2CPUSettings`：`0x29` 单核 Boost 上限、`0x2B` 全核 Boost 上限、`0x53/0x54/0x55` PPT/TDC/EDC、`0x6E` 回读）、`0x61/0x28` OC VID 命令码的存在性、slow mode 的行为逻辑（见 `SLOW_MODE.md`，它只是手动超频里的 800 MHz 跛行模式，与本程序无关）、以及 ZenStates-Core 里 PawnIO 的接线方式（设备名、IOCTL 算术）。
- **没用**：没有复制任何 C# 源码，没有 fork 仓库，没有使用它的 GUI、服务或 .NET 核心库。`investigation/` 目录里的上游源码与反编译输出**不随本项目发布**（见 `.gitignore`），只留在本地作证据链。

### 2. irusanov/ZenStates-Core（ZenStates 的子模块）— GPLv3

- 仓库：<https://github.com/irusanov/ZenStates-Core>
- License：GPLv3（与主仓库的 `LICENSE` 逐字相同，674 行）
- **用了**：`ZenStates.Core/Hardware/Smu/Settings/Zen2Settings.cs`（RSMU `SetOverclockCpuVid = 0x61`、MP1 `SetOverclockCpuVid = 0x28`；以及 Zen2 在 2.0 核心里**没有** `SetBoostLimitFrequency`，只有 `GetBoostLimitFrequency = 0x6E`——这正是本程序去恢复旧版 0x29/0x2B 的原因）、`Commands/SetOverclockCpuVid.cs`（先发 RSMU、失败再发 MP1 的顺序）、`PawnIo/PawnIo.cs`（IOCTL 算术）。
- **没用**：没有复制、链接或内嵌该库的任何代码。

### 3. namazso/PawnIO 与 PawnIO.Modules — 驱动 GPL-2.0（带例外），模块 LGPL-2.1-or-later

- 驱动：<https://pawnio.eu/> / <https://github.com/namazso/PawnIO>（**不随本项目发行**，用户需自行安装）
- 模块：<https://github.com/namazso/PawnIO.Modules>，本项目附带的两个模块来自其 0.2.11 release
- License：模块为 LGPL-2.1-or-later（全文见 `modules/COPYING-LGPL-2.1.txt`，由上游仓库 `Resources/PawnIo/COPYING` 逐字复制）
- **用了**：`modules/RyzenSMU.bin`（50,764 字节）与 `modules/AMDFamily17.bin`（10,652 字节），**未修改、已签名**，SHA-256 与 ZenStates-Core 仓库内嵌的资源文件逐字节相同：
  - `RyzenSMU.bin`：`301d9ca397108e09f31bfbd5ac4c9bb4f352a5de68532c32db3ba7ddcde93450`
  - `AMDFamily17.bin`：`dae74615761b78bdf064dfb3e136252ddcc6fc727d88f14738d0e5800d427a91`
- **没用**：没有修改模块，没有重新编译模块源码，没有把 PawnIO 驱动本身放进发行包。

### 4. 研究脉络（间接引用，不是依赖，没有用其代码）

- **FlyGoat / ryzen_nb_smu**（<https://github.com/FlyGoat/ryzen_nb_smu>）：ZenStates 上游 README 明确列为 SMU 命令表的研究来源之一。本项目的 C 代码没有直接参考它，协议事实是经 ZenStates 源码转述的；列在这里是为了让研究链条完整，不代表本项目依赖它。
- **FlyGoat / RyzenAdj**：本次只作对照研究（它面向移动版 Ryzen、只调功耗上限，没有桌面 Matisse 的 Boost 频率上限功能），**没有用它的代码，也没有用它的协议**，因此不作为致谢对象列入，仅在此说明以免混淆。

## License 选择说明

- 本项目 `LICENSE` = **GPLv3 全文**（从上游逐字复制，674 行）。
- 为什么不选 MIT / Apache-2.0：本程序虽然代码是 clean-room 重写，但（a）它实现的是从 GPL 源码研究得来的协议，（b）发行包附带 LGPL 模块且通过 PawnIO 驱动与内核交互，（c）上游 ZenStates 与 ZenStates-Core 均为 GPLv3。MIT/Apache 会让“这算不算 GPL 衍生作品”留下一个合规灰区；GPLv3 与上游同 license，是最稳妥、与上游兼容的选择。
- 以上不是法律意见。如需正式的法律判断，请咨询律师。
