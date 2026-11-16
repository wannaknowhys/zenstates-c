# ZenStates 2.0 `_2` 调查与 slow mode 反编译报告

日期：2026-11-16。调查在 Linux VM 上做，只做静态分析与交叉编译，
没有在真机（Ryzen 5 3600）上加载驱动或发 SMU 命令。所有“未确认”
都已标出，不要当成已验证的事实。

## 0. 结论先行（TL;DR）

1. **源码是公开的，而且就是 `_2` 构建的源码。** tag `without-inpout`
   指向 commit `ca745167`（同时也是 `dev` 分支头），`_2` 构建的改动
   `Fix VID detection and slow mode restore`（ca74516）就在这个 commit
   里。所谓“没发布源码”是误会：源码在，只是新版核心
   （ZenStates-Core）是子模块，`git clone` 后要再
   `git submodule update --init` 才能看到，子模块 commit 为 `a370422`
   （Core 1.93 dev）。
2. **slow mode 不是你想的那个东西。** 它跟 Boost 上限、跟 SMU 的
   Slow Limit（PPT slow）都没有关系。它是**手动超频（Manual OC）
   模式里的一个临时“跛行模式”**：点一下，全核压到 **800 MHz @ 约
   1.0 V**；再点一下，把之前存下来的每 CCX 频率和 OC VID 恢复回去。
   它只在 Manual OC 模式下可用（按钮平时是灰的），对 Auto/Boost
   模式下的 3600 毫无用处。
3. **ZenStates 2.0 根本做不了你要的 Boost cap。** 2.0 的 GUI 里
   没有任何 Boost 上限入口（`MainForm.cs` 里搜 `FMax`/`Boost`
   是 0 命中），核心库 `Zen2Settings` 里也没有给 Matisse 填
   `SetBoostLimitFrequency(AllCores)` 的消息码——`SetFMax()` 在
   3600 上是个死胡同（命令码 0 → `UNKNOWN_CMD`）。能 cap 的
   0x29 / 0x2B 只存在于**旧版 0.x 服务版**代码里（`Zen2CPUSettings`）。
   本目录的 pure C 程序恢复的正是那两个旧码，见 §5。
4. **反编译做出来了，而且和源码对得上。** ilspycmd 8.2 + .NET 6
   runtime 解出两个程序集；slow mode 的反编译结果和源码在语义上
   完全一致（0x98 = 152，见 §2）；`ZenStates-Core.dll` 里内嵌的
   4 个 PawnIO `.bin` 模块和源码仓库里的文件 SHA-256 逐字节相同，
   说明发布二进制就是用这份源码编的。

## 1. Release 与文件校验

| 项目 | 值 |
|---|---|
| tag | `without-inpout`，名 `v2.0 x64 Win 10/11`，2026-10-03 发布 |
| `_2` 文件 | `ZenStates-2.0-20261003_2_x64.zip`，373528 字节 |
| SHA-256（实测） | `42c2d326fe08945ccc3bdad289d67b41558fcb3968e79f4ebecc9e4625e99cf7`，与任务给出的值一致 |
| release 说明 | workaround build，不用 inpoutx64；新核心用 PawnIO 作内核驱动，只限 Win10/11 x64；需 PawnIO（https://pawnio.eu/）；“`_2` build should fix initial VID detection and slow mode button restore, but it is not tested extensively.” |

zip 内容（6 个文件）：

| 文件 | 大小 | 说明 |
|---|---|---|
| `ZenStates.exe` | 122688 | .NET Framework 4.8 GUI，AssemblyVersion 2.0.0.0 |
| `ZenStates-Core.dll` | 976192 | 新核心库（含 PawnIO 封装 + SMU 层） |
| `ZenStatesControls.dll` | 27136 | WinForms 控件库 |
| `ZenStatesControls.pdb` | 77312 | **唯一的 .pdb**，只有控件库有符号；exe 和 Core.dll 没有 pdb |
| `ZenStates.exe.config` | 206 | `<supportedRuntime version="v4.0" sku=".NETFramework,Version=v4.8"/>` |
| `README.md` / `LICENSE` | — | README 是 ZenStates-Core 的说明（2022 年的旧文） |

注意：zip 里**没有 `ZenStatesSrv.exe`**。2.0 已不是旧的
“GUI + Windows 服务”两件套，而是单进程 GUI + 内嵌核心库。
“有 pdb 而且是 C# 应该好搞定”只对了一半：确实是 C#、确实好反编译，
但 pdb 只有一个且不是主程序的；真正起作用的是源码本身已公开（§3），
反编译只是交叉验证手段。

## 2. slow mode 到底是啥（证据链）

### 2.1 位置

- 按钮：`ZenStatesControlLib/ManualOverclockItem.Designer.cs:100-114`，
  一个 `CheckBox`，`Appearance = Button`，文本就是 `"Slow Mode"`，
  只有 `OCmode`（手动超频模式）打开时才启用
  （`ManualOverclockItem.cs:313`：`checkBoxSlowMode.Enabled = OCmode`）。
- 点击只转发事件：`CheckBoxSlowMode_Click` → `SlowModeClicked`
  （`ManualOverclockItem.cs:345-347`）。
- 真正的逻辑在 `MainForm.cs:1067` 起的
  `ManualOverclockItem_SlowModeClicked`。

### 2.2 勾选（进入 slow mode）时做的事——源码原文

`MainForm.cs`（commit ca745167）：

```csharp
if (cb.Checked)
{
    double[] ccx_frequencies = new double[ccxCount];
    for (int i = 0; i < ccxCount; ++i)
    {
        int logicalCore = Math.Min(i * step, Math.Max(0, totalThreads - 1));
        ccx_frequencies[i] = cpu.GetCoreMulti(logicalCore);   // 逐 CCX 读当前倍频
    }
    Storage.Add("ccx_frequencies", ccx_frequencies);          // 存起来，内存里
    Storage.Add("oc_vid", manualOverclockItem.Vid);           // 存当前 OC VID

    SetFrequencyAllCore(800);                                  // 全核 800 MHz
    if (cpu.info.family <= Cpu.Family.FAMILY_17H)
        SetOCVid(0x98);                                        // Zen/Zen+/Zen2: VID 0x98
    else
        SetOCVid(Core.Utils.VoltageToVidSVI3(0.980));          // Zen3+: 0.980 V
}
```

关键数字：

- **800 MHz**：`SetFrequencyAllCore(800)`，走 SMU 的定频命令
  （RSMU `SetOverclockFrequencyAllCores`，Matisse 为 0x5C），不是
  Boost 上限，是**定频**——只不过定在一个很低的值上。
- **电压约 1.0 V**：`0x98` = 152。按 `Utils.VoltageToVidSVI3`
  的编码（`VID = (V - 0.245) / 0.005`），152 对应
  `0.245 + 152 × 0.005 = 1.005 V`；另一分支直接算
  `VoltageToVidSVI3(0.980)` = 147（0.980 V）。两分支目标一致：
  约 0.98–1.01 V。**不是 0.60 V**——别用 SVI2 的
  `1.55 - VID × 0.00625` 公式去算这个数，那是另一套编码。
- **保存的东西只在内存里**：`Storage` 就是一个静态 `Hashtable`
  （`Utils/Storage.cs`），进程退出就没了。slow mode 的“恢复”
  只能在同一次运行里点取消，不能跨重启。

### 2.3 取消勾选（退出 slow mode）时做的事

```csharp
else
{
    // 单核模式：直接重跑一遍手动超频设置
    if (manualOverclockItem.ControlMode == 0 && !manualOverclockItem.AllCores)
        ApplyManualOcSettings();
    else
    {
        double[] ccx_frequencies = Storage.Get<double[]>("ccx_frequencies");
        if (ccx_frequencies == null) return;                  // 没存过就直接返回
        uint vid = Storage.Get<uint>("oc_vid");
        if (SetOCVid(vid))                                    // 先恢复 VID
            for (uint i = 0; i < ccx_frequencies.Length; ++i)
            {
                uint mask = cpu.MakeCoreMask(0, i / ccxPerCcd, i % ccxPerCcd);
                uint targetFreq = Convert.ToUInt32(ccx_frequencies[i] * 100.00);
                SetFrequencyCCX(mask, targetFreq);            // 再逐 CCX 恢复频率
            }
    }
}
```

注意恢复顺序是**先电压后频率**，且 `SetOCVid` 失败就整个不恢复。
`ccx_frequencies` 为空时静默返回（不报错）。

### 2.4 `_2` 修的正是恢复逻辑的 bug

三个相关 commit（`git log`，tag `without-inpout` 一线）：

| commit | 内容 |
|---|---|
| `4d1e532` Restore slow mode button（2025-03-01） | 把按钮的事件重新接上；同时把进入值从 550 MHz 改成 800 MHz，Zen3+ 分支从 `SetOCVid(0)` 改成 `VoltageToVidSVI3(0.980)`；恢复分支当时是**注释掉的**（`//else { ... //}`），即点取消什么都不做 |
| `ef610d3` Make it possible to run without inpoutx64（2026-10-03） | 2.0 改用 PawnIO；引入 ZenStates-Core 子模块 |
| `ca74516` Fix VID detection and slow mode restore（2026-10-03，`_2`） | 两处：① `GetCurrentHwVid` 返回值加 `& 0xFF` 掩码（VID 检测修的就是这个）；② **把恢复分支取消注释并重写**：用 `MakeCoreMask(0, i / ccxPerCcd, i % ccxPerCcd)` 算 CCX 掩码（`ccxPerCcd`：Family > 17H 为 1，否则为 2），替代了原来按 `i / 4 - 2*ccd` 手算掩码的旧代码；读取每 CCX 频率时加了 `Math.Min(i * step, totalThreads - 1)` 防越界 |

所以 release 说明里的 “slow mode button restore” 指的就是：
**点取消后能把原来的频率/电压恢复回来**——在 `_2` 之前这段
代码是被注释掉的，点了 Slow Mode 就只能留在 800 MHz，要手动
重设才能回去。

### 2.5 反编译验证

工具链（都装成功了，只是走了点弯路）：

- `dotnet-sdk-8.0`（Ubuntu apt，8.0.134）。
- `ilspycmd` 最新版装不上（“settings file format version is higher
  than supported”），改装 **8.2.0.7535** 成功，但它要 **.NET 6
  runtime**——用 `dotnet-install.sh --channel 6.0 --runtime dotnet`
  装到 `~/.dotnet6` 后可运行。
- 命令：`ilspycmd -o decompiled/ZenStates.exe.decompiled -p ZenStates.exe`
  及 Core.dll 同理。输出在
  `investigation/decompiled/`（2.3 MB，含 4 个内嵌 `.bin`）。

核对结果：

- 反编译出的 `AppWindow.ManualOverclockItem_SlowModeClicked`
  与源码逐行语义一致：`SetFrequencyAllCore(800u)`、
  `SetOCVid(152u)`（0x98 = 152 的十进制写法）、
  `VoltageToVidSVI3(0.98)`、存取 `ccx_frequencies` / `oc_vid`、
  `MakeCoreMask` 恢复循环都在。差异只有 ILSpy 生成的变量名
  （`num`/`array`）和花括号风格，没有语义差异。
- 反编译出的 `Zen2Settings`：RSMU 地址十进制
  61932836 / 61932912 / 61934144 = 0x03B10524 / 0x03B10570 /
  0x03B10A40；`SetFastLimit` 83 = 0x53、TDC 84 = 0x54、EDC 85 =
  0x55、`GetBoostLimitFrequency` 110 = 0x6E——**没有任何
  `SetBoostLimitFrequency(AllCores)` 赋值**，和源码一致。
- 内嵌 `.bin` 模块 SHA-256 与源码仓库的
  `ZenStates.Core/Resources/PawnIo/*.bin` 逐字节相同：
  RyzenSMU.bin `301d9ca3…e93450`、AMDFamily17.bin `dae74615…427a91`、
  LpcIO.bin `b3896a1c…eb45e`、SmbusPIIX4.bin `91f9b4b1…dff56e5`。

### 2.6 slow mode 不是什么

- **不是 SMU Slow Limit / PPT slow。** `SMU_MSG_SetSlowLimit`
  这个命令确实存在于邮箱定义里（`Cpu.SetSlowLimit`），但它是
  功耗慢速上限，和 Slow Mode 按钮是两回事；按钮代码里一次
  都没碰它。
- **不是 Eco Mode、不是 P-State、不是降频节能。** 它只在手动
  超频定频模式下有意义：把你正在跑的定频临时换成 800 MHz
  保平安（比如摸散热、排查不稳定），再一键换回来。
- **对你要的 cap 没有任何帮助。** 它是定频 800，不是上限
  4000；它要进 Manual OC 模式，而你要的是 Auto/Boost 模式。

## 3. 源码有没有发布（结论）

有，而且对得上 `_2`：

- 主仓库：https://github.com/irusanov/ZenStates ，tag
  `without-inpout` → commit `ca745167`（2026-10-03），同时是
  `dev` 分支头；`master` 还停在 2022 年的 `d35a93bb`（2.0-beta）。
- 子模块：`ZenStates.Core` → https://github.com/irusanov/ZenStates-Core ，
  commit `a370422`（Core 1.93 dev）。**只 clone 主仓库是看不到
  核心源码的**，必须 `git submodule update --init`——这很可能
  就是“以为没发布源码”的原因。
- `_2` 的全部改动都在源码里：`ca74516`（VID 掩码 + slow mode
  恢复重写）、`ef610d3`（PawnIO 化）、`4d1e532`（按钮恢复）。
  反编译与源码一致（§2.5），不存在“二进制里有、源码里没有”
  的暗改。

## 4. PawnIO 接口（C 程序照抄的依据）

来源：`ZenStates.Core/PawnIo/PawnIo.cs`（源自
LibreHardwareMonitor 的同名文件）、`PawnIo/RyzenSmu.cs`、
`PawnIo/AmdFamily17.cs`，以及 PawnIO.Modules 上游
`RyzenSMU.p`（https://github.com/namazso/PawnIO.Modules/blob/main/RyzenSMU.p）。

| 项目 | 值 | 来源 |
|---|---|---|
| 设备 | `\\?\GLOBALROOT\Device\PawnIO`，GENERIC_READ\|GENERIC_WRITE，共享读写 | PawnIo.cs |
| 安装检测 | 注册表 `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO` 的 `DisplayVersion`（64 位视图优先） | PawnIo.cs |
| `IOCTL_PIO_LOAD_BINARY` | `(41394<<16) \| (0x821<<2)` = **0xA1B22084**，输入 = `.bin` 文件原字节 | PawnIo.cs 算术 |
| `IOCTL_PIO_EXECUTE_FN` | `(41394<<16) \| (0x841<<2)` = **0xA1B22104**，输入 = 32 字节 ASCII 函数名（NUL 填充）+ int64 参数数组；输出 = int64 数组 | PawnIo.cs 算术 |
| 模块加载 | 每个模块独占一个设备句柄；`RyzenSMU.bin`（SMU）、`AMDFamily17.bin`（MSR/SMN）分别加载 | RyzenSmu.cs / AmdFamily17.cs |
| `ioctl_send_smu_command` | 输入 7 个 int64：`[cmd, arg0..arg5]`（cmd 必须 ≤ 0xFF）；输出 6 个 int64：返回的 args | RyzenSMU.p:1601-1628 |
| `ioctl_get_smu_version` | 输入 0 个，输出 1 个 | RyzenSmu.cs |
| `ioctl_read_msr` / `ioctl_write_msr` | 读：`[index]` → `[value]`；写：`[index, value]` → `[]` | AmdFamily17.cs |
| SMU 邮箱协议 | 在 RyzenSMU.bin 模块**内部**完成：等 RSP ≠ 0 → 写 RSP=0 → 写 6 个参数 → 写 MSG → 等 RSP ≠ 0 → 读状态（0x01 = OK）；C 代码不直接碰邮箱寄存器 | RyzenSMU.p `send_command`；SMU.cs `SendSmuCommand` |
| PCI 锁 | 命名互斥 `Global\Access_PCI`，SMU 调用前后加锁，5 s 超时 | Mutexes.cs / PciBusLock.cs；RyzenSMU.p 也有 `@warning` 要求先拿这个锁 |
| 权限 | 设备 DACL 只允许 SYSTEM 和 Administrators；非管理员打开设备会失败 | PawnIO 上游文档（另见 README 的管理员问题） |

**未确认/存疑**：个别 AI 生成的文档把上面两个 IOCTL 写成
0x0A148208 / 0x0A148210。这与源码算术矛盾，本程序**没有采用**；
如果用户装的 PawnIO 版本改了设备类型，`LoadBinary` 会直接失败
并打日志，不会去猜另一个数。

## 5. Matisse（3600）的完整操作链——以及 2.0 的缺口

### 5.1 2.0 核心里 Matisse 的设定（`Zen2Settings`，已核对二进制）

| 邮箱 | 地址 |
|---|---|
| RSMU MSG / RSP / ARG | 0x03B10524 / 0x03B10570 / 0x03B10A40 |
| MP1 MSG / RSP / ARG | 0x3B10530 / 0x3B1057C / 0x3B109C4 |
| HSMP MSG / RSP / ARG | 0x3B10534 / 0x3B10980 / 0x3B109E0 |

RSMU 消息码（Matisse）：PPT/FastLimit 0x53、TDC 0x54、EDC 0x55、
TctlMax 0x56、EnableOcMode 0x5A、DisableOcMode 0x5B、
SetOverclockFreqAllCores 0x5C、PerCore 0x5D、SetOverclockCpuVid 0x61、
PBOScalar 0x58、GetPBOScalar 0x6C、IsOverclockable 0x6F、
GetBoostLimitFrequency 0x6E。

### 5.2 缺口：Boost 上限命令在 2.0 里是空的

- `RSMU.SMU_MSG_SetBoostLimitFrequencyAllCores` 和
  `MP1Smu.SMU_MSG_SetBoostLimitFrequencyAllCores` 在
  `Zen2Settings`、`Zen3Settings` 里**都没有赋值**，保持默认 0。
  只有 Zen4/Zen5（0x70）和 Phoenix（0x47）填了。
- `Cpu.SetFMax(freq)` → `SetBoostLimitAllCore.Execute`：RSMU 码
  为 0 就走 MP1，MP1 码也是 0，于是 `SendMp1Command(0, …)`，
  而 `SMU.SendSmuCommand` 明确拒绝 `msg == 0`
  （`ValidateMailbox` → `UNKNOWN_CMD` 0xFE）。
- **单核上限连命令类都没有**：新核心里只有 `SetBoostLimitAllCore`，
  没有 `SetBoostLimitFrequency`（单核）的 setter。
- 2.0 的 GUI 也没有 Boost 上限界面：`MainForm.cs` 全文搜
  `FMax`/`Boost` 0 命中。旧版 0.x 的 Auto 模式 Boost 频率
  控件在 2.0 重写时被拿掉了。

### 5.3 本目录 C 程序恢复的旧码（来自 0.x 源码，不是 2.0）

旧版 `ZenStatesSrv/CPUSettings.cs`（`Zen2CPUSettings`，master
`d35a93bb` 仍可查到）对 Matisse 用的是同一套 RSMU 地址，
消息码：

| 命令 | 码 | 参数（单位） |
|---|---|---|
| SetBoostLimitFrequency（单核 cap） | 0x29 | 频率，MHz（如 4000） |
| SetBoostLimitFrequencyAllCores（全核 cap） | 0x2B | 频率，MHz（如 3900） |
| SetPPTLimit / SetFastLimit | 0x53 | 功率，mW（88 W → 88000） |
| SetTDCLimit | 0x54 | 电流，mA（60 A → 60000） |
| SetEDCLimit | 0x55 | 电流，mA（90 A → 90000） |
| GetBoostLimitFrequency（回读验证） | 0x6E | 无参数，返回一个频率 |

旧版 `CPUHandler.setBoostFrequencySingleCore/AllCores` 还做了两件
本程序照抄/取舍的事：

- **每线程先写 MSR_HWCR（0xC0010015）bit 21 置 1**（“P0 fix”），
  再发 SMU 命令。本程序照抄（AMDFamily17 模块读写 MSR），但
  失败只记警告不致命——因为 2.0 核心根本不做这步，SMU 命令
  可能不需要它。
- 单核那路旧版还顺带 `WritePstate(0/1, data)` 写 P0/P1 MSR。
  本程序**没抄这步**（见 §6 未确认项）：那是 Zen/Zen+ 的机制，
  Zen2 应以 SMU 命令为准；如果实测 cap 不生效，这是第一顺位
  的排查点。

调用顺序（本程序）：查管理员 → 查 CPUID（只认 Family 17h
Model 71h）→ 打开 PawnIO、加载两个模块 → 读 SMU 版本（为 0
就中止）→ HWCR fix → 拿 `Global\Access_PCI` 锁 → 依次发
0x2B（3900）→ 0x29（4000）→ 0x53（88000）→ 0x54（60000）→
0x55（90000）→ 回读 0x6E → 放锁 → 写日志退出。
**不写定频、不写电压、不写 VID。**

## 6. 未确认项（不要当成定论）

1. **0x29 / 0x2B 在这块 3600 + 这版 BIOS/AGESA 上是否还被 SMU
   接受**，没法在 VM 里验证（没有 3600 硬件，也没有加载驱动做
   真机写入）。旧版代码（2019-2022 年的 ZenStates 0.x）用它
   是有据可查的，但 AGESA 变化可能导致 SMU 拒绝（返回
   0xFD/0xFE）。首次运行即测试：看 HWMonitor 最高频率是否卡
   在 4000 MHz；若命令返回非 OK 或频率照冲 4200，见 README
   的排查清单。
2. **0x6E 回读返回的是单核上限还是全核上限**，在 Matisse 上
   未确认——程序只打日志，不以此判成功。
3. **HWCR bit 21 是否必需**，未确认——程序按旧版做了，失败
   也不中止。
4. **P0/P1 MSR 写入是否必需**（§5.3），未确认——程序没做，
   列为首选排查点。
5. **PawnIO 的 IOCTL 码是否随版本变化**，未确认——程序用
   ZenStates-Core 源码里的算术；若 `LoadBinary` 失败，检查
   PawnIO 版本，不要改数字去猜。
6. `_2` 发布说明说 “not tested extensively”，slow mode 恢复
   在 `_2` 里是刚重写的代码，同样没经过充分测试。
7. 本报告引用的 PawnIO.Modules `RyzenSMU.p` 是上游当前
   main 分支；ZenStates-Core 内嵌的 `.bin` 标注来自
   PawnIO.Modules v0.2.2，两者接口名一致，但模块内部实现
   可能有版本差——以 `.bin` 的实际行为为准。

## 7. 本次调查用到的证据位置

- `investigation/ZenStates-2.0-20261003_2_x64.zip`（已校验 SHA-256）
- `investigation/extracted/`（zip 解压结果）
- `investigation/repo/`（主仓库，checkout `ca745167`）
- `investigation/repo/ZenStates.Core/`（子模块，`a370422`）
- `investigation/decompiled/`（ilspycmd 8.2 输出，exe + Core.dll）
- `modules/RyzenSMU.bin`、`modules/AMDFamily17.bin`（随本程序
  附带的模块，与源码资源逐字节相同，见 §2.5 的 SHA-256）
