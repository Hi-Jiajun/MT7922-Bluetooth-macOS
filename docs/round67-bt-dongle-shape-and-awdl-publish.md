# round67 —— 蓝牙走通了（USB dongle 路线）+ AWDL 的真实卡点（接口没发布）

日期：2026-10-14（本次开机 08:07:01，部署件 `d9ad9d9115fdb60b`）
结论先行：**蓝牙从"16 秒崩一次的死循环"变成可用**（控制器 On、地址非 0、
12 秒收到 46 条 LE 广播）；**Wi-Fi 的 AWDL 卡点被定位到"虚拟接口 attach 了但从未
registerService"**，已修 + 已编译 + 已部署，等一次重启验证。

---

## 1. 蓝牙：三层原因，最后一层是 macOS 自己的标志位

### 1.1 本次开机的实测链（08:07:28–08:07:42，pid 159）

```
08:07:28.716 [bm3_usb][Found_USB_Dongle] -- Found USB Device :
08:07:28.716     USB Product Name = CSR Bluetooth Dongle
08:07:28.716             idVendor = 0x0A12   idProduct = 0x0001   deviceClass = 0xEF
08:07:28.805 ...returned true / setting gUseInternalController to FALSE
08:07:28.805 Found external Bluetooth dongle -- setting chipset to THIRD_PARTY_DONGLE
08:07:29.068 E  OI_HCICMD_ReadLocalVersionInformation failed: STATUS 408, but we move forward
08:07:29.080 E  ERROR -- Third Party Dongle has the same address as the internal module
08:07:29.080 E  initCmdReadBdAddrCB failed:  STATUS 158
08:07:29.080    ERROR -- Third Party USB Bluetooth dongle failed initialization.
                Switching to internal Bluetooth module
```

即：`mtkbtcsr` 那条"冒充 CSR 外置 dongle"**成功了**（BCM_4350C2 那条路被绕开，
`bcm_vsc.c:1125` 的断言不再触发）。真正拦住它的是**地址撞车**：
SimpleMtkBtFw 用 HCI `0xfc1a` 把芯片地址写成 `02:1a:7d:da:71:13`，而我们又把
**同一个地址**塞进了 `bluetoothInternalControllerInfo` 当"内置模块地址"，
于是 macOS 判定"外置 dongle 冒用了内置模块的地址"并拒绝初始化。

### 1.2 失败之后才是真正的死循环

初始化失败让 bluetoothd 把这件事写回固件树（字符串在 `/usr/sbin/bluetoothd`：
`IORegistryEntrySetCFProperty() failed for bluetoothExternalDongleFailed`），
`nvram -p` 里读到 `bluetoothExternalDongleFailed %01`。此后**每一次**
bluetoothd 启动都直接跳过 dongle：

```
08:09:22.261 [GetExternalDongleSwitchBehavior] Unable to get bluetoothHostControllerSwitchBehavior from nvram
08:09:22.261 External Bluetooth dongle was not found -- setting chipset to default BCM_4350C2
08:09:22.261 HCI Transport is set to H4BC
08:09:37.834 E  UART open(/dev/cu.BLTH) port failed to appear after 15 seconds
08:09:37.834 E  Transport layer initialization failed   -> CBCrash -> restart
```

`/Library/Logs/DiagnosticReports/` 里 08:07→08:11 有 **16 个** bluetoothd 崩溃报告，
周期约 16 秒（15 秒等 `/dev/cu.BLTH` + 启动开销）。也就是说，**所谓"蓝牙崩"在后半段
根本不是 MediaTek 的问题，是 macOS 在走它自己的 Broadcom-UART 兜底路径**，
而这条路径的串口设备在这台机器上永远不存在。

### 1.3 修法（三条，都是 NVRAM，不需要改 kext）

| key | 值 | 为什么 |
| --- | --- | --- |
| `bluetoothExternalDongleFailed` | `%00` | 清掉"外置 dongle 已失败，用内置"的标志 |
| `bluetoothHostControllerSwitchBehavior` | `always` | 每次开机都优先外置 dongle（macOS 上 USB 蓝牙 dongle 的通行做法） |
| `bluetoothInternalControllerInfo` | 8 个零字节 + `02:1a:7d:da:71:14` | 内置地址必须**不等于** dongle 自己的地址，否则撞车判定再次拒绝 |

写入工具：`tools/bt-nvram-shape.py`（保留 plist 容器格式、写前备份、写后回读 + `plutil -lint`）。
已写入**两份** EFI 的 `NVRAM/Add/7C436110-…`：ESP 与 U 盘。

### 1.4 实测结果（无需重启：bluetoothd 启动时读这三个值）

`launchctl bootout/bootstrap` 之后：

```
08:14:07.313 [GetExternalDongleSwitchBehavior] bluetoothHostControllerSwitchBehavior is set to always
08:14:07.313 [GetExternalDongleSwitchBehavior] externalDongleSwitchBehavior = 0x02 (always)
08:14:07.314 [bm3_usb][Found_USB_Dongle] -- Found USB Device :
08:14:07.314     USB Product Name = CSR Bluetooth Dongle
08:14:07.750 THIRD_PARTY_DONGLE supports the following VSCs
08:14:07.763 [Server.NVRAM] Using external Bluetooth dongle, do not read info from NVRAM
```

`system_profiler SPBluetoothDataType`：

```
Bluetooth Controller:
    Address: 02:1A:7D:DA:71:13
    State: On
    Chipset: THIRD_PARTY_DONGLE
    Firmware Version: v9473 c1557
    Supported services: 0x392039 < HFP AVRCP A2DP HID Braille LEA AACP GATT SerialPort >
```

`launchctl print system/com.apple.bluetoothd`：`runs = 1`、`last exit code = (never exited)`
（此前是 `runs = 14` + 每 16 秒一个新 `.ips`），08:14 起持续运行。
`nvram -p` 里 `bluetoothExternalDongleFailed` 保持 `%00` —— 说明这次初始化真的过了，
没有再把标志位写回 `%01`。

### 1.5 功能验证：无线电真的在收

`tools/btle-scan.swift`（CoreBluetooth 扫描，12 秒）：

```
state: 5 (poweredOn)
found[42] rssi=-58 name=书房的REDMI G Pro 27U   (服务数据 FDAA = 小米 MIoT)
found[45] rssi=-59 name=lemesh.light.wy0c14     (服务数据 FE95 = 米家灯)
...
total: 46
```

46 条广播、带 RSSI、带名字、带厂商/服务数据 ⇒ **射频接收链路（天线→芯片→HCI→主机）
是通的**。这是"State: On"证明不了的东西：它只说明栈接受了控制器。

### 1.6 还没验证的

- 重启后三条 NVRAM 由 OpenCore 注入是否同样生效（这次是运行时写的，开机注入是另一回事）。
- 经典蓝牙（BR/EDR）配对、A2DP/HID 实际连接：需要一台设备进配对模式，属于用户侧动作。
- `mtkbtcsr` 是否可以去掉（让设备自称 `0e8d:0616`）：现在没有理由改，留着，
  但它给"是否需要冒充"留了个可测的开关。

---

## 2. Wi-Fi：AWDL 的真实卡点 = 虚拟接口没有发布

### 2.1 本次开机的读数

```
AirportMtkVifIoctlCalls    = "r1=0 r4=0 r5=0 r6=2 r7=0 other=0"
AirportMtkVifOverrideCalls = "r1=0 r4=0 r5=0 r6=1 r7=0 other=0"
AirportMtkVirtIfResult     = "awdl-already-exists"
AirportMtkVirtIfName       = ""
AirportMtkVirtIfRole       = 6
AirportMtkVifReq           = "calls=18 fail=0 last10=207/0 134/0 118/0 136/0 129/0 213/0 135/0 132/0 126/0 122/0"
AirportMtkAwdlSeq          = "len=16 enc=0 steps=3 dup=0 fill=255 s0=0x0006 …"
AirportMtkIoctlReqs        = "calls=3428 r1=901/0 r9=296/0 r103=0/0 r207=0/0"
```

ioreg 里控制器下的接口节点：

```
+-o en1                  <class AirportMtkInterface, registered, matched, active, ...>
+-o IO80211P2PInterface  <class IO80211P2PInterface, !registered, !matched, active, busy 0, retain 9>
```

wifip2pd：

```
08:08:09.418 E Timed out while waiting for interface AWDL [CoreP2P/AppleDevice.swift:556]
08:08:09.419 E Failed to accept XPCSession[ContinuityCaptureAgent] because there is no AWDL interface
```

**读法**：第一次 `VIRTUAL_IF_CREATE`（r6）确实走了 `attachVirtualInterface()`，
对象也 attach 到控制器下了（树里就在 en1 旁边，BSD 层 `awdl0` 也在），
**但它从来没有 `registerService()`** ⇒ 永远不 matched、不 start，谁都打不开。
第二次请求走了"已存在"捷径并返回成功，于是产出属性看起来像进展，实际是空转。

旁证：我们自己发布的 en1 是**两步**——`attachInterface()` 之后
`fNetIf->registerService()`（`AirportMtk.cpp`）。虚拟接口这条路只做了前半步。

### 2.2 对照：`r103=0/0` 与 `r207=0/0`

`APPLE80211_IOC_CURRENT_NETWORK`(103) 与 `CHANNELS_INFO`(207) **一次都没进驱动**
（`AirportMtkIoctlReqs` 的 `r103=0/0`），而 airportd 仍报
`Apple80211GetWithIOCTL … IOCTL type 207 return -3903`；CoreWLAN 探针
（`tools/cwprobe`）仍然是 `ssid=nil bssid=nil rate=7.0 ch=44`。
⇒ 这两条走的是**接口侧协议**，不是在老 ioctl 分派上，`case 103` 是死代码
（与 `round65-interface-side-scoping.md` 的结论一致）。

### 2.3 修法与部署

在 AWDL 分支的 attach 成功之后（以及"已存在"那条捷径里）补上发布：

```cpp
if (!fAWDLRegistered) {
    vif->registerService();
    fAWDLRegistered = true;
    setProperty("AirportMtkVirtIfPublished", kOSBooleanTrue);
}
```

`fAWDLRegistered` 是新增成员（`registerService()` 返回 void，且编译头里没有
`isRegistered()`），删除接口时随 `fAWDLInterface` 一起清零。
同时给 AWDL 请求做了**时刻属性**（见 §3）。

构建与部署：

```
bash build.sh      -> 56/56 compiled, 链接成功, build id src=85eb82478fdf
bash verify.sh     -> VERIFY: PASS（773 个未定义符号，未解析 2 个 = 预链接元数据槽，IO80211/Skywalk 105 个全解析）
tools/deploy.sh    -> ESP  475e7b88ee66cdb7（d9ad9d9115fdb60b 已标 verified）
tools/sync-usb-efi.sh -> U 盘 475e7b88ee66cdb7
```

---

## 3. 新的"开口即留痕"：三个时刻属性

AWDL 的判断一直卡在"关联落在 wifip2pd 的 30 秒窗口里没有"，而这个数字此前是**从
dmesg 快照 grep 出来的**——已经漏过一次（t≈38s 的那次在快照里，t≈7s 的第一次不在）。
内核环形缓冲 128 KB，会把这台机器最需要的那个数转走。

所以三个里程碑改成**开机毫秒属性**（`airport_up_time()`，与 dmesg 同一时钟，只写一次）：

| 属性 | 位置 |
| --- | --- |
| `AirportMtkTFirstScan` | `mt7921_mcu_hw_scan()` 入口（第一次扫描） |
| `AirportMtkTJoin` | `AirportSTAIOCTL.cpp: setASSOCIATE()` 入口（join 请求到达） |
| `AirportMtkTAssoc` | `mwx_newstate_task()` 的 `case IEEE80211_S_RUN:`（链路真的起来） |

`assoc - FirstScan` 与 `assoc` 相对开机时刻这两个口径都能读，不需要日志留存。

---

## 4. 下一次开机的验收清单（按顺序）

1. `sudo bash tools/round65-verdict.sh` —— 一次性读全部（哈希、AWDL、BT、MCS、扫描形状、时刻）。
2. **BT**：`system_profiler SPBluetoothDataType` 应为 `State: On` + `THIRD_PARTY_DONGLE`；
   `nvram -p | grep bluetooth` 三条值应在；bluetoothd `runs` 小且不涨。
   （这一项同时验证"三条 NVRAM 由 OpenCore 注入也生效"。）
3. **AWDL 发布**：ioreg 里那个 `IO80211P2PInterface` 节点应变成 `registered, matched`，
   属性 `AirportMtkVirtIfPublished = Yes`，且 wifip2pd 不再出现
   `Timed out while waiting for interface AWDL`。
4. **AWDL 时序**：`AirportMtkTAssoc` 是否 < wifip2pd 的窗口。若是，下一步才是
   "按 `AirportMtkAwdlSeq` 的时间分片切信道"（**必须分片**，不能停在 AWDL 信道上把 infra 扯断）。
5. 若 AWDL 仍超时：下一个嫌疑是 `Apple80211BindToInterfaceWithService … IOServiceOpen
   0xe00002c2`（本机连 en1 都失败、只是它有 IOCTL 兜底）。那时再决定是否给接口侧补
   `newUserClient`。

## 5. 回滚

- Wi-Fi：`sudo bash driver/AirportMtk/uninstall-efi.sh`（或把 ESP/U 盘的 AirportMtk.kext
  换回 `d9ad9d9115fdb60b`）。
- 蓝牙三条 NVRAM：`python3 tools/bt-nvram-shape.py <config.plist> --remove`（两份都要）。
  备份在各自 config 旁的 `*.bak-btnvram-20261014-081557`。

---

## 6. "固件是不是最新的"——已核实（不需要重启）

这个问题以前只能靠 README 里的对照表猜，因为 kext 里的固件是**zlib 压缩**的
（数组以 `0x78 0x9C` 开头），`strings` 抓到的常是固件体内另一个 build-time 串而不是头部版本。
现在有工具了：`tools/bt-fw-version.py`（解压后读前 14 字节）。

```
bt-rebuild/src/FwBinary.cpp       MT7922 fw 20251118163447   515,478 B   （上游 master）
bt-rebuild/src-newfw/FwBinary.cpp MT7922 fw 20260724143815   517,398 B   （linux-firmware 最新）
```

再把**部署件二进制**与这两份压缩块逐字节比对（ESP 与 U 盘都做）：

```
src       fw 20251118163447 in ESP=False USB=False
src-newfw fw 20260724143815 in ESP=True  USB=True
```

⇒ **这台机器现在跑的蓝牙固件就是 linux-firmware 的最新版（2026-07-24 那一版）**，
不是 2025-11 的旧件；驱动侧是本仓库重建的 laobamac/SimpleMtkBtFw（3919a1bf78b4438f），
即上游 master + 我们的 `mtkbtcsr` 形状补丁。所以"同步最新固件/驱动"这件事**已经是完成态**，
只是以前没有证据，现在有了。

## 7. 同一类 bug 还没修的地方（下次的范围，已读出来的确切形状）

`setVIRTUAL_IF_CREATE` 的另外两个分支（`P2P_DEVICE` / `P2P_GO`，AirDrop 新式
"所有人"模式用的那对接口）仍然是老写法：`new IO80211P2PInterface` + `init()`，
**既不 attach 也不 registerService** —— 和 AWDL 修复前一模一样的病。
本次开机没有触发它们（`AirportMtkVifIoctlCalls` 只有 `r6`），所以这一轮不动，保持单因变量。

**但这不能简单照抄 AWDL 那五行。** 角色常量（本 SDK，`include/Airport/apple80211_var.h`）：

```
APPLE80211_VIF_P2P_DEVICE = 3     P2P_CLIENT = 4     P2P_GO = 5
APPLE80211_VIF_AWDL       = 6     SOFT_AP    = 7
```

而我们自己的 `createVirtualInterface()` override 对 **role 1–4** 一律
`new IO80211VirtualInterface`（名字 "p2p"），role ≥5 才交给 `super`。
也就是说 P2P_DEVICE(3)/P2P_CLIENT(4) 现在拿到的是**通用类**，不是 `IO80211P2PInterface`；
而 `attachVirtualInterface()` 会回调这个 override，所以如果只是把 ioctl 分支换成
`attachVirtualInterface()`，`OSDynamicCast<IO80211P2PInterface>` 会得到 NULL，
`fP2PDISCInterface` 变成空 —— 那就是把"没发布"换成"类型错"，一样用不了。

正确顺序是**两步一起改**（留给 AWDL 验证通过之后的那一批）：

1. override 里把 role 3/4/5 都建成 `IO80211P2PInterface`（与框架自己的工厂一致，
   见同函数上方那段反汇编注释：`role 3,4,5 -> IO80211P2PInterface, init(..., "p2p")`）；
2. ioctl 的 P2P 分支改走 `attachVirtualInterface()`，再补 `registerService()`，
   并把结果写成独立属性（`AirportMtkP2PResult` / `…Published`），
   这样一次开机里 AWDL 与 P2P 两条读数能分开归因。

---

## 8. 08:58 boot 的结果（AWDL 修复成立；蓝牙出现"冷启动回退"）

### 8.1 AWDL 发布修复：成立

```
AirportMtkVirtIfPublished = Yes
AirportMtkVirtIfResult    = "awdl-attached"
ioreg: +-o IO80211P2PInterface  <class IO80211P2PInterface, registered, matched, active, busy 0 (0 ms), retain 11>
```

wifip2pd 侧（这是真正的判据，8 分钟窗口内已无任何超时）：

```
08:58:53.024 Df wifip2pd  Found new interface awdl0 with role AWDL [CoreP2P/AppleDevice.swift:340]
08:58:53.113 Df wifip2pd  Created AWDLInterface<P2PController<IO80211Driver>>[awdl0]<c2:f6:e9:12:83:f5>
08:58:53.118 Df wifip2pd  WiFi powered on, setting link up on AWDLInterface[awdl0]
08:58:53.128 Df wifip2pd  XPCSession[airportd] is monitoring AWDL state information
```

`ifconfig awdl0`：`UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST` + `status: active`。
⇒ 2026-10-13 那个"接口在 BSD 层有、但不是 IOService"的死结解开了。

**同一批读数给出的下一层缺口**（都不是 AWDL 能否起来的问题，是更远的特性）：

| 现象 | 读数 | 判断 |
| --- | --- | --- |
| 服务绑定回退 | `Apple80211BindToInterfaceWithService … IOServiceOpen failed 0xe00002c2` → `Falling back to IOCTL path` | 连 en1 也这样（一直如此），有 IOCTL 兜底，不阻塞 |
| awdl0 的国家码 | `APPLE80211_IOC_COUNTRY_CODE`(51) 读/写都 `-1/0xffffffff` | 需要给虚拟接口也接上 51 |
| 低延迟接口 | `IOCTL type 293/'unknown Apple80211_IOC_'` → `Failed to create low latency interface for awdl0` | Apple 的低延迟（Sidecar 那类）通道，属**新功能**，不是 AWDL 前提 |

### 8.2 蓝牙：冷启动会回退（已确认机制 + 已装自动兜底）

这次开机的原始读数：

```
08:59:33.608 Found external Bluetooth dongle -- setting chipset to THIRD_PARTY_DONGLE   ← 伪装本身没问题
08:59:33.880 E  ERROR -- Third Party Dongle has the same address as the internal module
08:59:33.902    ERROR -- Third Party USB Bluetooth dongle failed initialization. Switching to internal module
→ BCM_4350C2 → H4BC → 等 /dev/cu.BLTH 15s → 每 16 秒崩一次（launchctl runs 一路涨到 21）
system_profiler: Address NULL / State Off / Chipset BCM_4350C2；nvram: bluetoothExternalDongleFailed %01
```

**机制（运行时 A/B 钉死，不再靠猜）**：把这把钥匙（`bluetoothInternalControllerInfo`）设成**芯片自己的地址**
（`…02:1a:7d:da:71:13`）再重启 bluetoothd，能 100% 复现"same address"这条；设成任何**别的** 14 字节
（`…:14`）就通过。所以判定读的就是这个 NVRAM 值，而冷启动时它是芯片地址。

**是谁写的：还不是 macOS 以外的东西。** 三个 kext 逐字节 grep `bluetoothInternalControllerInfo`
—— SimpleMtkBtFw / SimpleMtkBtPatcher / BlueToolFixup **都是 0 命中** ⇒ 写它的是 macOS 自己
（bluetoothd 里就有 `IORegistryEntrySetCFProperty() failed for bluetoothInternalControllerInfo` 这个 setter）。
而运行时（成功的那几次）它**从不被改写**，说明这次写入只发生在冷启动早期一次。

**09:20 补一条把范围缩到内核之外**：`IOBluetoothFamily.kext` 在本机是 **KC-only**
（`Contents/MacOS/` 下没有 Mach-O），而
`LC_ALL=C grep -c bluetoothInternalControllerInfo /System/Library/KernelCollections/SystemKernelExtensions.kc`
= **0** ⇒ 内核侧不含这个变量名。**写它的只能是用户态的 bluetoothd**（该名字在 `/usr/sbin/bluetoothd`
里同时以读取和 setter 两种形态存在）。剩下唯一没定的就是**它在启动序列里的哪一步写**，
以及为什么运行时重启不会写——`bt-nvram-watch.log` 的下一次开机读数就是为这两点准备的。

### 8.3 落地的两个 LaunchDaemon（一个取证、一个兜底）

| 组件 | 位置 | 作用 |
| --- | --- | --- |
| `com.hiliang.bt-nvram-watch` | `/Library/LaunchDaemons/…plist` + `/usr/local/libexec/bt-nvram-watch.sh` | 开机后每 2 s 采一次那三个键（**只在变化时记一行**，含 uptime 与 bluetoothd pid），持续 5 分钟 → `/var/log/bt-nvram-watch.log`。**下一次重启就能回答"谁在什么时刻把它写成芯片地址"** |
| `com.hiliang.bt-nvram-nudge` | 同上（`-nudge`） | 开机后 4 分钟内若看到 `bluetoothExternalDongleFailed = %01`（bluetoothd 只在 dongle 路径失败后写它）→ 写入实测可用的三个值 + 重启 bluetoothd 一次，并回读验证 → `/var/log/bt-nvram-nudge.log` |

兜底是**桥**不是修复：它把"每次冷启动要手动拨一次"变成自动，但根因（谁写的、能否不让它写）仍待取证。
已实测：人为把状态摆回失败态（`…13` + `%01`）后运行 nudge，

```
09:10:53 dongle-failed flag seen (t=+0s); applying the three measured values
09:11:20 OK: controller is on THIRD_PARTY_DONGLE
```

控制器回到 `State: On / THIRD_PARTY_DONGLE / Address 02:1A:7D:DA:71:13`。
卸载（三条，可随时回滚）：`launchctl bootout system/com.hiliang.bt-nvram-{watch,nudge}`、
删 `/Library/LaunchDaemons/com.hiliang.bt-nvram-*.plist`、删 `/usr/local/libexec/bt-nvram-*.sh`。
脚本副本留在 `tools/launchd/`。

### 8.4 一个构建纪律补充（避免下次误判"部署件不对"）

重建的二进制与部署件**哈希不同是正常的**：kext 里嵌了构建时间戳
（`strings` 可见 `20261014T001906Z` = 部署件 / `20261014T011135Z` = 重建件），
而 `src=85eb82478fdf` 这个源码标识两边一致。要确认"部署件确实含某段代码"，
用内容检查（例如 `strings … | grep AirportMtkVirtIfPublished`），不要只用哈希。

---

## 9. AWDL 接口被真的用起来之后（09:15，已部署 `bdfc8ac487829c5b`）

接口一旦发布成功，栈就开始**真的对它下发请求**了——这是这一轮最大的读数变化：

```
AirportMtkVifReq  = "calls=73 fail=6 last10(newest first)=27/0 27/0 12/0 207/0 129/0 207/0 12/0 27/0 27/0 27/0"
AirportMtkIoctlReqs = "calls=9636 r1=1747/0 r9=569/0 r103=0/0 r207=0/0"
```

请求号对应（`include/Airport/apple80211_ioctl.h`）：**12 = CARD_CAPABILITIES**、
**27 = SUPPORTED_CHANNELS**、207 = CHANNELS_INFO、129 = AWDL 时序表。
也就是说 wifip2pd 现在在问这张虚拟接口"你支持哪些信道/能力"——这是 AWDL 选社会信道的基础。

### 9.1 用户态日志只看到 3 条失败，驱动自己数到 6 条

```
Apple80211IOCTLGetWrapper:7023 ifname['awdl0'] IOCTL type 51/'APPLE80211_IOC_COUNTRY_CODE', len[3] return -1/0xffffffff
Apple80211IOCTLSetWrapper:5108 ifname['awdl0'] IOCTL type 51/'APPLE80211_IOC_COUNTRY_CODE', len[8] return -1/0xffffffff
Apple80211IOCTLSetWrapper:5108 ifname['awdl0'] IOCTL type 293/'unknown Apple80211_IOC_', len[24] return -1/0xffffffff
```

3 ≠ 6，所以这一轮加了 `AirportMtkVifFail`（失败请求号的最新 10 条，含总数）——
下次开机直接读，不再靠推断。

### 9.2 本轮的两处改动

1. **虚拟接口补 `APPLE80211_IOC_COUNTRY_CODE`(51)**（顺带 `LOCALE`(28)，只读）。
   基础设施接口一直有这两个 case，虚拟接口没有——这就是上面那两条 `-1` 的来源。
   国家码决定 AWDL 社会信道里哪些合法，缺它就会回到早期的
   `social channel 2G: 6 on 5G: 0`。实现上只是加两个 dispatch 项：
   处理函数就是 airportd 一直在基础设施上驱动的那两个，没有新代码路径。
   （编译期抓到 `setLOCALE` 不存在，所以 28 用 `IOCTL_GET` 只读。）
2. **`AirportMtkVifFail`** 仪器（见上）。

构建：56/56、`VERIFY: PASS`、`src=9f2f2aee6061`；已部署 ESP + U 盘
`bdfc8ac487829c5b`（上一版 `475e7b88ee66cdb7` 已按 08:58 boot 的实测标 verified）。

**09:25 同一批再加一条只读仪器，构建换成 `9a2de83978565100`（src=630f79c6e543，ESP+U 盘）**：
129 的时序表以前只 dump 前 8 条（`len=16 … s0=0x0006 s1..s7=0x0000`），现在按 `length` 打满
（每条打 `u16/pad` 两个字段）。理由是那 8 条读不出**表的结构**：
`struct apple80211_channel_sequence { uint16_t flags; uint8_t pad; }`，0x0006 最可能是
"信道 6、无标志"，但"1 个信道 + 15 个无效项"和"16 项计划里 1–7 恰好为 0"需要完全不同的代码——
而这两个解释在前 8 条里长得一样。这是**仪器**不是行为改动，所以与 51 那条同批部署
（`deploy.sh --force` 已在台账里注明覆盖了未验证的 `bdfc8ac4`）。

### 9.3 下一次开机要看的三条（判据已写进 `round65-verdict.sh`）

| 看什么 | 期望 |
| --- | --- |
| `awdl0` 上的 `IOCTL type 51` | 从 `-1/0xffffffff` 变成 `0` |
| wifip2pd 的 `social channel` 行 | 5GHz 项不再恒 0 |
| `AirportMtkVifFail` | 给出那 6 条失败的**具体请求号**；若全是 51/293/其它已支持项，就知道还差什么 |
| `AirportMtkAwdlSeq` | 打满 16 条的 `u16/pad` ⇒ 才能决定"按表切信道"要写什么 |

再往后才是把 129 下发的时序表落到**信道时间分片**（`AwdlSeq` 已经能读到：
`len=16 steps=3 s0=0x0006` ⇒ 第一步信道 6）——那一步必须在 infra 关联不被扯断的前提下做。
