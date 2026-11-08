# round47 — 重启后判读：蓝牙"谁接走"的判据找到了，IE 普查有个早退 bug

采集：`captures/post-reboot-20261012-184941/`（`tools/post-reboot-capture.sh`，只读）。
boot 时间 18:24:00，uptime 26 分钟，无新 panic（`/Library/Logs/DiagnosticReports` 最新仍是 06:16 那条）。

---

## 1. 蓝牙：kext 干了它该干的事，卡在 bluetoothd 的识别判据上

### 1.1 实测到的（本机，24H32）

| 观测 | 值 | 来源 |
| --- | --- | --- |
| kext 加载 | `com.laobamac.SimpleMtkBtFw (1.0.0)` | `kmutil showloaded` |
| 匹配到设备 | `SimpleMtkBtFw` 节点，idVendor 3725 / idProduct 1558，`IOMatchedAtBoot=Yes` | `ioreg -c SimpleMtkBtFw -r -l` |
| 固件下载成功 | `FirmwareLoaded = Yes`，`fw_name = BT_RAM_CODE_MT7922_1_1_hdr.bin` | 同上（设备节点属性） |
| 改名成功 | 注册表出现 `Bluetooth USB Host Controller@01000000 <class IOUSBHostDevice>` | `ioreg -l` |
| 新固件本身没问题 | 下载路径走通，没有超时/失败日志 | 上表两条 |

⇒ **新固件（20260724143815）和重建链是好的**；之前判据表里"固件下载失败就回滚旧固件"这条暂时用不上。

### 1.2 关键新观测：bluetoothd 的传输层被切成 USB 了

重启前那一轮（无 kext）日志里是：

```
BT_HCI_TRANSPORT not set - Attempting to read from plist.
FindTransportType -- UART Transport
HCI Transport is set to H4BC
```

本次（有 kext、设备已改名）：

```
BT_HCI_TRANSPORT not set - Attempting to read from plist.
HCI Transport is set to USB
```

即：**USB 这条传输路径已经被选中**，而且 `bm3_usb` 真的开始找设备了：

```
[bm3_usb][GetProductAndVendorID] -- Found USB Device :
    USB Product Name = Wireless_Device
     USB Vendor Name = MediaTek Inc.
            idVendor = 0x0E8D
           idProduct = 0x0616
         deviceClass = 0xEF (239)
          locationID = 0x01000000
[bm3_usb][GetProductAndVendorID] -- Use internal Bluetooth USB Host Controller
[bm3_usb][IOThreadFunc] -- USB -- Can't obtain vendorID and productID -- try again
```

每 1–5 秒循环一次，直到 `Still cannot find USB Bluetooth Controller after 15 seconds`。

（"为什么传输层从 UART 变 USB"目前是**推断**：最可能是设备改名/出现后 `FindTransportType` 才返回 USB；
没有找到打印 transport 来源的那一行日志，未证实。）

### 1.3 根因：它比的不是节点名，是 `USB Product Name` 这个属性

`/usr/sbin/bluetoothd` 未 strip 到只剩未定义符号，用 `llvm-objdump -d` 反汇编
`IOThreadFunc`/`GetProductAndVendorID`（VMA `0x1000d1bf0`–`0x1000d2367`），逐条读出来：

1. `IOServiceMatching("IOUSBHostDevice")` 遍历**所有** USB 设备；
2. 过滤 `bDeviceClass`：`(class - 0xE0) > 0x1F` 丢弃，再按位掩码 `0x80008001` 检查
   ⇒ **只保留 0xE0 / 0xEF / 0xFF**（本机 0xEF，所以它把我们的设备打印了出来）；
3. 取该设备的 **`USB Product Name` 属性**（`CFDictionaryGetValue`，不是注册表节点名），
   `CFStringCompare(它, "Bluetooth USB Host Controller", 0)`：
   - 相等 ⇒ `foundInternal = true`，后续用这对 VID/PID；
   - 不等 ⇒ 落到最后兜底（两个 uint16 全局量），两者都是 0 时打印
     `Can't obtain vendorID and productID -- try again`。

本机属性值一直是 `Wireless_Device`——上游 kext 只做了 `setName()`（改注册表**节点名**），
**没有改 `USB Product Name` 属性**，所以判据不成立。这就是 15 秒超时循环的全部原因。

### 1.4 已做的修补（已部署，等重启验证）

改 `outputs/20261010-mt7922-wifi/bt-rebuild/src-newfw/SimpleMtkBtFw.cpp`
的 `publishReg()`：固件成功后，除了原来的 `setName()`，再给设备加一条属性

```cpp
m_pDevice->setProperty("USB Product Name", OSString::withCString("Bluetooth USB Host Controller"));
```

只动注册表属性，**不动 USB 描述符、不动 VID/PID 匹配**；失败分支不改（避免没装好固件也冒充控制器）。

| 项 | 值 |
| --- | --- |
| 重建命令 | `SRC_DIR=./src-newfw bash build-bt2.sh`（6/6 编译 + 链接成功） |
| 新 kext 二进制 sha | `4292f66c3f198c3e` |
| ESP 路径 | `/Volumes/OPENCORE/EFI/OC/Kexts/SimpleMtkBtFw.kext`（43 条 personality 原样，含 `0e8d:0616`） |
| 编译验证 | `strings <binary> \| grep -c 'USB Product Name'` = 1（旧二进制 0） |
| ESP 备份 | `tools/backup-mtk-efi-20261012-190218-pre-bt-usrprodname/`（含 BT kext、WiFi kext、config.plist） |
| 回滚 | `cp -R tools/backup-mtk-efi-20261012-190218-pre-bt-usrprodname/SimpleMtkBtFw.kext /Volumes/OPENCORE/EFI/OC/Kexts/` |

`config.plist` 未变：`Kernel:Add` 16 条，index 15 `SimpleMtkBtFw.kext` Enabled=true。

### 1.5 下次重启要看的三行

```
[bm3_usb][GetProductAndVendorID] -- Found internal Bluetooth USB Host Controller -- vendorID = 0x0e8d, produceID = 0x0616
```

出现这行 ⇒ 判据过了，接下来应当是 `BTDeviceOpen` / `SetConfiguration` / `Configuring bluetooth device`。
然后再看 `system_profiler SPBluetoothDataType` 的 `Address` 是否从 NULL 变成真实 BD_ADDR。

判据（不许只看设备改名）：`FirmwareLoaded` 布尔 + 上面那行 + `Address` 非 NULL。

---

## 2. WiFi：IE 普查有早退 bug（已修，已部署）

`MtkMwx.cpp` 的 `mwx_beacon_tim_note()` 里，TIM 分支结尾是 `return;`——
而 IE 普查的循环体就在同一个函数里。结果是**每个 beacon 只统计到 TIM 就退出**：

```
mwx[mwx0]: ies: beacons=18 elems=72 seen= 0:18 1:18 3:18 5:18  lastseq= 0,1,3,5
```

18 个 beacon × 4 个元素 = 72，`lastseq` 只有 SSID / rates / DS / TIM。
RSN(48)、HT(45/61)、VHT(191/192)、Ext(127)、vendor(221) **一个都没进普查**——
也就是说这一版"用来跨 AP 对比 IE"的观测，恰好对最关键的那些元素是瞎的。

修法：把 TIM 分支里的 `return` 去掉，让它继续沿 `frm += 2 + len` 走完整个 beacon。

| 项 | 值 |
| --- | --- |
| 编译 | `driver/AirportMtk/build.sh` → 56/56，链接成功，build id `src=f835d38b2193` |
| 新二进制 sha | `d9809af9038056fc` |
| 部署 | `tools/deploy.sh`（install-efi.sh 备份在 `tools/backup-mtk-efi-20261012-190223/`） |
| 台账 | 旧 build `6fb117282b96abcc` 已按"跑过"标记 verified，并在 note 里写明它的普查不完整 |

另外本轮 WiFi 侧的状态没有变化：en1 关联在 `QIN_5G`（ch44），拿到的是 `169.254.x.x`
（DHCP 未成），也就是 CASE-B 原样复现；`health` 里 `isr` 在涨（1119→1476）、
`ip4=0 ip6=11 dgrp=0`，与之前记录一致。

---

## 3. 本轮没动的

- AWDL：未改，`awdl0` 仍不存在（维持不做）。
- `SimpleMtkBtPatcher.kext`：仍在 ESP 但不注入（它 patch 的是 `IOBluetoothHostController::FindQueuedRequest`
  / `IOUSBHostDevice::deviceRequest`，属"控制器起来之后"的 quirk，不是识别判据）。
- 别的会话的 `CX3` 驱动、`UTBMap.kext` 等：未触碰。
