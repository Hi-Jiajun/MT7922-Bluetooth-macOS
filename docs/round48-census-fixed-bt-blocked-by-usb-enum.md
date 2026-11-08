# round48 — IE 普查修好了；蓝牙被"USB 没枚举到"挡住（不是驱动问题）

boot 19:40:55，uptime 3 分钟起测，无 panic（最新 panic 报告仍是 06:16；`DumpPanic` 报
"previous shutdown cause 5" = 上次是正常关机重启）。

运行中的两个 kext（与 ESP 一致）：

| kext | ESP sha | 运行 |
| --- | --- | --- |
| AirportMtk | `d9809af9038056fc` | `local.hiliang.AirportMtk (0.1.0)` UUID 633EC40E-… |
| SimpleMtkBtFw | `4292f66c3f198c3e` | **没加载**（见 §2） |

---

## 1. WiFi：IE 普查修复确认有效（第一次拿到完整 IE 表）

```
mwx[mwx0]: ies: beacons=16 elems=403 seen=
  0:16 1:16 3:16 5:16 7:13 11:13 32:15 35:9 42:10 45:16 47:2 48:15 50:9 51:13
  61:16 70:13 74:6 127:13 191:13 192:13 195:13 221:55 244:9 255:73
lastseq= 0,1,3,5,7,32,35,195,70,51,221,48,11,45,61,191,192,127,255,255,255,255,255,255,255,221,221,221
```

修之前是 `elems=72 / 4 个 id`；现在是 `elems=403 / 24 个 id`，**RSN(48)、HT Cap(45)、HT Op(61)、
VHT(191/192)、Ext(255)、vendor(221)** 全部进表——这正是"跨 AP 对比 IE"需要的输入。

顺手在同一 boot 里做了一次重关联（`networksetup -setairportnetwork en1 QIN_5G`），
拿到这台 AP 的关联期面包屑（之前被环形缓冲冲掉了）：

```
join_bss: selbs="QIN_5G" chan=44 rssi=30 rsnprotos=0x1 rsnakms=0x2 ciphers=0x8
          groupcipher=0x8 groupmgmt=0x20 rsncaps=0x8c rsnielen=411 ic_state=1
assoc_req_ies:  subtype=0x0 n=3 len=40 ids=0,1,48
assoc_resp_ies: n=8 ids=1,221,70,90,127,244,221,221
key add id=0 cipher=8 … done      (PTK)
key add id=1 cipher=8 … done      (GTK)
key add complete port valid mac=00:11:22:33:44:55 node_flags=0x3
```

即：关联、密钥安装都完成了，**assoc 响应里没有 RSN IE(48)**（beacon 里有），
而 `ip4=0 / dgrp=0` 依旧——CASE-B 原样。下一次要的就是**同一 boot 里的小米热点对照**，
两条 `ies:` 一减，就知道这颗 AP 有哪些 IE/参数我们没有处理。

台账：`d9809af9038056fc` 已 `--verified`（note 里写了上面这些观测）。

---

## 2. 蓝牙：US 设备这次根本没上 USB 总线 ⇒ kext 无从匹配

事实链（本 boot）：

1. `kmutil showloaded` 里**没有** `com.laobamac.SimpleMtkBtFw`；
2. `ioreg -p IOUSB` 全树只有手机/鼠标/键盘/麦克风/耳机，**没有 `Wireless_Device`**；
3. 内核日志（19:40:58）：

```
AppleUSBXHCICommandRing::executeCommand: command[10] … timeout after 1033ms
AppleUSBXHCICommandRing::abortCommand: command[10] did not generate an event
hardware became unavailable while waiting for command
AppleUSBXHCICommandRing::setAddress: completed with result code 0
HS11@01000000: AppleUSBHostPort::createDevice: failed to create device (0xe00002bc)
HS01@00600000: AppleUSBXHCIPort::powerOn: deadline passed (PORTSC 0x00000000)
SS02@00200000: AppleUSBXHCIPort::powerOn: deadline passed
AppleUSBXHCI::createPorts: speed index 4 has inconsistent speed IDs (5 and 6)
```

对比上一个 boot（18:24）同一端口是成功的：

```
HS11@01000000: AppleUSBHostPort::enumerateDeviceComplete_block_invoke:
    enumerated 0x0e8d/0616/0100 (Wireless_Device / 3) at 480 Mbps
```

⇒ 端口号一致（内置蓝牙 = XH00 的 **HS11 / port 16 / locationID 0x01000000**），
差异只在这一次 XHCI 上电/命令环超时，设备创建失败（`0xe00002bc`）。
**不是 kext 问题、不是端口映射问题、不是 config 问题**：

- `config.plist` 与部署前备份逐字段一致（`Kernel:Add` 16 条、`Kernel:Patch` 29 条、
  `Kernel:Block` 2 条、`XhciPortLimit: True`、boot-args 一致）；
- `UTBMap.kext` 与它自己的 `.pre15.8.1-24H32-20261011-175345` 备份内容一致（HS11 = 255 两版相同），
  18:30 之后被改动的文件只有我们两次部署碰过的 4 个 Info.plist + config.plist；
- 我的改动只给设备加注册表属性，且此时设备还不存在。

结论：**这是一次 USB 枚举偶发失败**（AMD XHCI 上电/命令环超时），
重启重试即可；BT 的"识别判据修补"这一轮**还没被验证过**。

---

## 3. 下次重启（把两件事一次抓完）

1. 蓝牙三行：`HS11…enumerated 0x0e8d/0616` → `Found internal Bluetooth USB Host Controller -- vendorID = 0x0e8d, produceID = 0x0616`
   → `Configuring bluetooth device` / `Address` 非 NULL；
2. WiFi：连小米热点（WPA2，用户手动开），采一条 `ies:` 做与 QIN_5G 的减法。

---

## 4. 第二次重启（20:25）仍然没有：而且这不是"偶发"

同一现象复现，且拿到了历史对照——今天整天按小时统计
`HS11@01000000 | hardware became unavailable | enumerated 0x0e8d` 三类行：

| 时刻 | 该行数量 | 判读 |
| --- | --- | --- |
| 05:15 / 05:17 / 06:16 / 07:13 / 07:28 / 07:59 / 08:37 / 09:20 / 15:46 / 16:51 / **18:24** | 各 1 行 | 全部是**成功**的 `enumerated 0x0e8d/0616` |
| **19:40 起** | 每分钟 50–56 行，持续到 20:28+ | **失败循环**：`createDevice: failed (0xe00002bc)` ×63，控制器 `command failure` → `attempting recovery`，反复重建 |

也就是说：**19:40 之前每一次开机，内置蓝牙的 USB 侧都能正常枚举；19:40 之后每次开机都卡在
XHCI 命令环超时**，且控制器的自动恢复不成功（一直重试）。

两次重启之间变了什么，逐条排除：

| 候选 | 是否成立 | 依据 |
| --- | --- | --- |
| 我们的 WiFi 新构建（普查修复） | 不成立 | 它只改 beacon 解析里的一个 `return`，与 USB 无关；且失败发生在 USB 层 |
| 我们的蓝牙新 kext（加 `USB Product Name`） | 不成立 | 该 boot 里**设备根本没被创建**，kext 无从 probe，`kmutil showloaded` 也没有它 |
| config.plist / UTBMap | 不成立 | 与部署前备份逐字段一致；UTBMap 与其自身 `.pre` 备份内容一致；18:30 后仅 4 个 Info.plist + config.plist 被动过 |
| **18:24 那次 boot 里，kext 第一次把新固件下进了芯片** | **唯一与时间点吻合的变量** | 18:24 之前芯片一直是 ROM 状态（USB 正常），18:24 之后（重启 5+ 次都没断过电）一直是"固件已驻留"状态 → 全部失败 |

注意一个反直觉但重要的事实：**端口是"看到设备"的**——`AppleUSBHostPort::createDevice` 只有在
端口检测到连接时才会被调用，所以不是"设备没插上/没供电"，而是
**SET_ADDRESS 阶段 XHCI 命令环拿不到事件（hardware became unavailable），连累整个控制器**。

由此得到两条互斥的假设，用**冷启动**一次就能分开：

- H-FW：是"固件驻留"造成的——掉电后芯片回到 ROM 状态，枚举应恢复（那就要在 kext 的关机路径里
  给芯片做一次复位/回 ROM，否则以后每次都得冷启动）；
- H-HW：是这块 M.2 模块的 USB 侧（或主板 XHCI 端口供电）本身在热重启下不可靠，
  与固件无关（那就属于 USB/供电这条线，得和做端口补丁的那个会话一起看）。

下一次要做的实验（按顺序）：

1. **完全断电冷启动**（关机 → 断电/长按电源 10 秒 → 开机）：
   - 若 `enumerated 0x0e8d/0616` 回来 ⇒ H-FW 方向，继续看蓝牙三行；
   - 若仍失败 ⇒ H-HW 方向，退一步：先把 BT kext 在 config 里 `Enabled=false` 做一次对照，
     确认在"没有我们任何代码"的状态下端口是否也起不来。
2. 冷启动成功后**立刻再做一次热重启**：如果又失败，就坐实"H-FW：固件驻留 → 热重启枚举失败"，
   届时方向是在 kext 里补"关机时把芯片复位/回 ROM"。

---

## 5. 在坏状态下做了什么（不用重启也能推进的部分）

### 5.1 失败点已被精确到命令级

每次失败都是同一条 XHCI 命令超时：

```
AppleUSBXHCICommandRing::executeCommand: command[2] … 01002c01 timeout after 1023ms
AppleUSBXHCICommandRing::abortCommand: command[2] … did not generate an event
AppleUSBXHCICommandRing::executeCommand: hardware became unavailable while waiting for command
AppleUSBXHCICommandRing::setAddress: completed with result code 0
HS11@01000000: AppleUSBHostPort::createDevice: failed to create device (0xe00002bc)
```

`01002c01` 解出来是 **Address Device**（TRB type 11 = SET_ADDRESS）。也就是：
**端口看见设备 → 发 SET_ADDRESS → 设备的 USB 核不回 → 控制器命令环报废 → 整个 XHCI 进入
recovery 循环**（本 boot 5 分钟内仍在失败 130 次）。这不是"设备没插上/没电"，是**设备侧的 USB 核挂住**。

社区对同一类现象的标准处置就是"彻底断电"（r/hackintosh 上的原话："unplug power cable, hold
pc's power button for 30 seconds, plug power cable back in and boom Bluetooth started working"）。

### 5.2 给蓝牙 kext 加了开关（已部署，默认行为不变）

`src-newfw/SimpleMtkBtFw.cpp` 新增 `mtkBtSkipRequested()`，抄的是本仓 Wi-Fi 驱动
`airportMtkAttachGate()` 的成熟写法（`IORegistryEntry::fromPath("/options", gIODTPlane)` +
`PE_parse_boot_argn`）：

| 开关 | 用法 | 语义 |
| --- | --- | --- |
| NVRAM `MtkBtSkip` | `sudo nvram MtkBtSkip=1` / `sudo nvram -d MtkBtSkip` | 存在即跳过 |
| boot-arg `mtkbt-skip` | 改 config.plist 的 boot-args | 同上 |

默认（键不存在）＝照常工作，所以对现有行为零影响；config.plist 的 `NVRAM>Add` 不含这个键、
`WriteFlash=True`，所以在 macOS 里写的值能活过重启。跳过时 `start()` 在 open 设备**之前**
就返回 false（和上游自己的 "not usb device" 分支同形），设备一个字节都不被碰 —— 这才是
"到底是不是下过固件把它弄脏"这个 A/B 需要的干净对照。

重建：`SRC_DIR=./src-newfw bash build-bt2.sh` → 6/6 编译 + 链接成功，`_kmod_info` 在，
段数 12（与上一版一致）、nm 符号 457→461。
部署：ESP 上 `SimpleMtkBtFw` 二进制 sha `529ea628eea0caa9`（含 §5.2 开关 + §1.4 的
`USB Product Name` 修补）；备份 `tools/backup-mtk-efi-20261012-210647-pre-bt-skipgate/`。

### 5.3 下一层修复的把手已经找到（暂不部署）

`/Library/Developer/CommandLineTools/SDKs/MacOSX15.2.sdk/…/Kernel.framework/…/IOKit/usb/IOUSBHostDevice.h`
第 765–769 行：

```
@brief Terminate the device and attempt to reenumerate it
@discussion This function will reset and attempt to reenumerate the USB device. The current
IOUSBHostDevice object and all of its children will be terminated. A new IOUSBHostDevice object
will be created and registered if the reset is successful…
virtual IOReturn reset();
```

同一头文件第 194–195 行把它标成旧 `IOUSBDevice::ReEnumerateDevice()` 的官方替代。

它的意义是：上游在下载完固件后只写了芯片侧的 `MTK_EP_RST_OPT`（重置 USB 端点，**期待**主机
重新枚举），而本机证据表明**重枚举并没有发生**（18:24 那次 boot 里，bluetoothd 看到的仍是
我们 kext 那个"已配置、属性带 FirmwareLoaded"的旧设备实例，节点名也还是我们改名后的那个）。
设备停在"端点已复位、等主机重新配置"的状态里 —— 这正好同时解释两件事：

1. 为什么系统蓝牙栈一直没真正接管；
2. 为什么下一次开机设备答不出 SET_ADDRESS（USB 核没被干净地重新枚举过，热重启又不断电）。

所以候选修复是：**在固件下载 + 协议使能之后，由主机侧调用一次 `m_pDevice->reset()`**，
让芯片重新枚举（新设备实例 → 干净的寻址状态），再把设备交给系统蓝牙栈。注意头文件明确
"may not be called from the port workloop thread"，实现要放到独立线程里，不能在 `start()`
的调用栈上直接调 —— 这一步等 A/B 结果确认方向后再做。

---

## 6. 决定性证据：Linux 官方驱动里逐字写着这个故障（并且上游漏了一个 shutdown 步骤）

按"去看官方实现"这条路翻 `drivers/bluetooth/btusb.c`，命中两段关键代码。

### 6.1 Linux 对这颗芯片 + Ryzen 根集线器的专门 quirk

`btusb_mtk_setup()` 里，针对 `dev_id` **0x7922 / 0x7925**，条件是
`platform_is_ryzen() && is_direct_child_of_root_hub(udev)`（本机完全符合：Ryzen 9700X，
内置蓝牙就在根集线器 port 16 上），设置 `BTUSB_WAKEUP_BROKEN`，并在 `btusb` 的 auto-suspend
判断里用它挡住挂起。它上面那段注释，是对本机现象最精确的描述：

> It is reported that **remote wakeup events could sometimes cause some adapters
> completely unresponsive. Resetting the xHCI root hub doesn't help at all, and
> **recovering from such a state needs a power cycle**.

对照我们的实测：热重启三次都不行、root hub 层面的反复重试（63 次 Address Device）全部失败、
只有掉电才有希望恢复 —— 症状逐条对上。

### 6.2 Linux 在关机时会把这个功能关掉，而上游 kext 从来不关

`btusb` 的 MTK 分支挂了 `hdev->shutdown = btusb_mtk_shutdown`，后者调用
`btmtk_usb_shutdown()`：发一条 **WMT FUNC_CTRL（flag=0，参数 0 = 关闭 BT 功能）**。

再看本仓的上游源码 —— `BtMtk.cpp:51`：

```cpp
bool BtMtk::shutdown()
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    return enableBluetoothProtocol(false);   // 就是 Linux 那条 WMT FUNC_CTRL(disable)
}
```

**这个函数存在，但全仓没有任何地方调用它**（`grep -rn 'shutdown()' *.cpp *.hpp` 只有定义那一行）。
也就是说：上游的 kext 下完固件、**使能**了 BT 功能，然后就再也没关过 —— 系统关机时芯片是
"活的、使能着、可远程唤醒"的状态进下一次开机的。这正好解释为什么故障偏偏从 18:24 那次
boot（第一次真正使能 BT 功能）之后开始，而之前每次开机都好好的。

### 6.3 已实现的修复（已部署，等下次开机验证）

在 `SimpleMtkBtFw` 里重写 `systemWillShutdown(IOOptionBits)`（IOService 的官方关机钩子，
按叶到根顺序通知，我们的 kext 通过 `joinPMtree` 已在 PM 平面里）：

1. 只在"本轮确实使能过"（`m_bSetupDone`）时动手；
2. 重新 open 设备 → `new BtMtk` + `initWithDevice`（只做配置/接口/管道定位，**不会重下固件**）；
3. 调 `bt->shutdown()` = WMT FUNC_CTRL 关闭功能，然后 close；
4. A/B 开关：NVRAM `MtkBtNoShutdown` / boot-arg `mtkbt-noshutdown`（存在即不做这步）。

| 项 | 值 |
| --- | --- |
| 编译 | 6/6 + 链接成功；`systemWillShutdown` 符号在；`_kmod_info` 在 |
| 新二进制 sha | `03cd2cd853d89ff8` |
| 已部署到 ESP | `/Volumes/OPENCORE/EFI/OC/Kexts/SimpleMtkBtFw.kext` |
| 备份 | `tools/backup-mtk-efi-20261012-215004-pre-bt-shutdown-disable/` |
| 回滚 | 把备份里的 `SimpleMtkBtFw.kext` 覆盖回去 |

### 6.4 修复之外的另一半：恢复需要掉电，而且可能不止"关机"

Linux 那句 "recovering from such a state needs a power cycle" 说明：**当前这台机器上已经挂住的
芯片，只能掉电恢复**（这也解释了为什么 21:43 那次重启仍然 88 次失败）。

注意 6.1 里 Linux 的做法是"不让它进那个状态"，而不是"从那里救回来"。所以顺序是：

1. 先掉电把芯片救回来（若普通关机/断电无效，说明主板在 S5 仍给 M.2 供电 —— 需要在 BIOS 开
   ErP/Deep Sleep 让 S5 断掉待机电源，或物理拔一次模块）；
2. 之后靠 §6.3 的关机禁用，避免下一次再把它弄进这个状态。

---

## 7. 用户的两条新情报（Windows 认、其它 USB 设备也被牵连）与机制确认

### 7.1 用户观察到的"牵连"是真的，但机制不是"kext 冲突"

22:02 那次 boot 的内核日志，1.5 分钟内的计数：

| 现象 | 次数 | 含义 |
| --- | --- | --- |
| `HS10/HS12 … terminateDevice: … port is powering off` | 49 | 麦克风、耳机的端口被**反复拆掉** |
| `HS10/HS12 … enumerateDeviceComplete` | 48 | 随即又**反复重新枚举** |
| `HS11@01000000 … forcePowerGated: kPowerStateSuspended/On: completed with 0xe00002d6 after 1–2 s` | 16 | **port 16（内置蓝牙）的电源状态切换全部超时** |
| `AppleUSBXHCIPCI@00000000 … hardwareExceptionThreadCallGated: 0x40 (command failure)` → `attempting recovery` | 持续 | 控制器异常 → 自我恢复（复位控制器）→ 所有端口重枚举 → 再撞 port 16 → 循环 |

错误码查证（`Kernel.framework/…/IOKit/IOReturn.h`）：
`0xe00002bc` = **kIOReturnError**（createDevice 失败）、`0xe00002d6` = **kIOReturnTimeout**
（端口电源切换超时）、`0xe00002e9` = **kIOReturnDeviceError**（控制器 setPowerState 失败）。

所以因果链是：

```
port 16 的设备答不出 Address Device（SET_ADDRESS 超时）
   → 控制器报 command failure，进入 recovery（复位自己）
   → 所有端口重枚举：G2000 / 麦克风 被拆了又装（用户看到的"受牵连"）
   → 恢复完再试 port 16，又超时 → 无限循环
```

**这不是"蓝牙 kext 与 USB 补丁冲突"**：这个 boot 里我们的蓝牙 kext 根本没加载
（设备创建失败 → 没有匹配对象 → `kmutil showloaded` 里没有它）。用户看到的连带影响是真的，
但它是"一个不可寻址的设备把整个 XHCI 拖进恢复循环"的后果。

### 7.2 新事实：Windows 里这颗芯片正常

用户实测：Windows 认这块蓝牙。含义有两条：

1. **芯片没坏**（不是"固件写坏了持久状态"）——至少它的 USB 侧在另一个 XHCI 驱动下是可寻址的；
2. 因此差异在"主机侧怎么对待这个设备"：Apple 的 XHCI 驱动**只能给端口做复位**，
   做不到真正的掉电重上电；而 Windows 的 XHCI 驱动（以及冷启动）会把端口的电真正断一次。

这与 §6.1 的 Linux 注释完全一致：那类状态下 *resetting the xHCI root hub doesn't help at all*，
**只有掉电才行**。而这块 M.2 模块的 BT 端口是"内部端口"（`UsbConnector=255`，没有电源开关），
macOS 永远不会给它断电 —— 所以一旦芯片落进那个状态，macOS 自己永远救不回来。

待确认的两个问题（决定下一步）：

- 22:02 这次开机，前面是不是刚从 Windows 过来？（若不是，Windows 那次可能已经把设备弄回可寻址，
  只是随后的 macOS 启动又把它弄回去了；若是，说明 Windows 的复位不足以让 macOS 认出来。）
- Windows 里蓝牙是"能正常用（能扫到/配对）"，还是只是"设备管理器里出现"？
