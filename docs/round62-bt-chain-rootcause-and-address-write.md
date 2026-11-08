# Round 62 — 蓝牙链路：逐层定位到"地址缺口"，并在 kext 里补上写地址

日期：2026-10-13。产物：`bt-rebuild/` 链新构建 **`4d256b220aae2bca`**（已部署到 ESP）。

## 1. 逐层排除（全部有本机证据）

| 层 | 结论 | 证据 |
| --- | --- | --- |
| **USB 拓扑 / 复合结构** | **正常** | 递归读子树可见 `Wireless_Device@01000000` 下三个接口 `BT_ACL_If@0` / `BT_SCO_If@1` / `BT_ISO_If@2`（`AppleUSBInterface`，均已 matched）。命名就是 macOS 对蓝牙控制器的标准命名 |
| **kext 附着** | **正常** | `class SimpleMtkBtFw` 实例在设备子树内（`IOProbeScore = 94000`） |
| **Apple 传输层** | **正常** | `bm3_usb`：`[GetProductAndVendorID] Found internal … 0x0E8D` → `RegisterBTDevice` → `BTDeviceCreate` ✓ → `BTDeviceOpen` ✓ → `Configuring bluetooth device` ✓ |
| **BlueTool 脚本** | **正常** | `BlueTool finished running boot script` ✓ / `BlueTool finished running init script` ✓（**不是**失败点） |
| **控制器上电 / 本地地址** | **卡在这里** | `localAddress 0x0`、`State: Off`、`Vendor 0 / build (null)` |

**作废的两条旧结论（我自己先说错、后用对照验掉）**：
1. "设备没有 interface 节点 ⇒ 传输层接不上"——错。我先前用 awk 范围读 ioreg 读漏了；而且**已知工作正常的复合设备同样只显示 `AppleUSBDevice`**，说明那种呈现是正常的。
2. **"没有 IOKit 的 `IOBluetoothHCIController` 对象 ⇒ 传输层没接上"**——错。这个 macOS 版本把 BT 的 USB 传输放在 **`bluetoothd` 用户态（`bm3_usb`）**，IORegistry 里本来就没有那个对象；用它当判据是无效的。

## 2. 地址这件事：谁在要、要什么

- `bluetoothd` 启动时会打 `could not find BT_DEVICE_ADDRESS env variable`；**这句话是 `/usr/sbin/BlueTool` 打的**（二进制内含该字符串与 6 字节 MAC 的格式串）。
- BlueTool 是 **按需 Mach 系统守护进程**（`/System/Library/LaunchDaemons/com.apple.BlueTool.plist`）。
- **实验（部分成功）**：`sudo launchctl setenv BT_DEVICE_ADDRESS 02:1a:7d:da:71:13` **并同时重启 BlueTool**（先前只重启 bluetoothd 时它沿用旧环境 ⇒ 无效）后，那句报错**消失** ⇒ **BlueTool 确实消费了这个变量**。
- **但 `localAddress` 仍是 0x0** ⇒ bluetoothd 侧那个本地地址**不是**来自这个环境变量，更可能来自**控制器自身对 `Read_BD_ADDR` 的应答**。
- 本机 **NVRAM 里没有任何 bluetooth 变量**（`nvram -p | grep -i bluetooth` 为空），所以也没有走"持久化控制器信息"那条路。

## 3. 落地：在 kext 的 bring-up 里给控制器写地址

依据 Linux `drivers/bluetooth/btmtk.c:369` 的 `btmtk_set_bdaddr()`：
**厂商 HCI 命令 `0xfc1a` + 6 字节地址**（`btusb.c` 把 `hdev->set_bdaddr` 指向它）。

- 时机：**固件下载之后、bluetoothd 打开设备之前**——那一刻命令/事件端点由我们独占（这也解释了为什么"运行时读 0x81"要避免，而这里可以）。
- 字节序：按 `bdaddr_t` 的 **LSB-first** 写（Linux 用 `str2ba()` 构造，打印顺序相反），代码里显式反转并注明。

### 部署前自查抓到的两个真错误（若不改，下一次重启必白费）

| 错误 | 后果 | 更正 |
| --- | --- | --- |
| 用 `readWMTEvent()` 读应答 | 那是**厂商控制传输**（`bRequest=1, wValue=48`）的 WMT 通道；HCI 事件走**中断端点** ⇒ 成功的命令会被读成**超时** | 改用 `interruptPipeRead()`（控制器自带该原语） |
| 把 `evtBuf[2]` 当状态码 | `04 \| evt \| plen \| num_pkts \| opcode(2) \| status` 中第 2 字节是**长度** ⇒ **成功会被判成失败** | 改为校验事件类型 `0x0e`、长度 ≥7，读**第 6 字节** |

## 4. 诊断属性（不依赖 IOLog，从 ioreg 直接读）

本机实测：kext 的 `IOLog`（`XYLog`）**既不进统一日志、也不进 dmesg**（`log show` 里 25 条 `SimpleMtkBtFw` 全是 `kernelmanagerd` 噪音，dmesg 计数 0）。故所有关键结论都改为发布属性：

`MtkBtChipId`、`MtkBtSetupOk`、`MtkBtLastError`（`gate-skip` / `not-usb-device` / `super-start-failed` / `open-failed` / `init-failed` / `setup-failed` / `load-failed` / `none`）、`MtkBtLocationID`（固件上传后芯片会**重枚举**，只有它能区分实例）、`MtkBtBdAddrSet`、`MtkBtBdAddr`。

## 5. 重启后的判据

```sh
ioreg -p IOUSB -r -n Wireless_Device -l -w0 | \
  egrep 'MtkBtBdAddr|MtkBtBdAddrSet|MtkBtChipId|MtkBtSetupOk|MtkBtLastError|FirmwareLoaded'
system_profiler SPBluetoothDataType | egrep 'Address|State'
```

- `MtkBtBdAddrSet=true` 且 `Address` 非 `NULL`、`State` 不再恒 `Off` ⇒ **地址缺口闭合**；
- `=false` ⇒ 我们能拿到**控制器自己的状态码**（不再靠猜）；
- 属性完全读不到 ⇒ `start()` 未被执行，由 `MtkBtLastError` 指认。

## 6. 待办（下一步）

1. 用 `mtkrecover` 之外的**单变量**依次验证：写地址是否被接受、写后是否上电；
2. 若要**真实地址**（而非固定本地管理地址）：读 EFUSE/OTP 并用同一个调用点替换那 6 个字节；
3. 重新评估那两个第三方组件（`SimpleMtkBtPatcher` / `BlueToolFixup`）：前者目标符号所在的可执行体在 24H32 上**不在磁盘**
   （`IOBluetoothFamily.kext/Contents/MacOS` 不存在，只在 KernelCollection 里），其"加载但从不触发"与之吻合——**能用我们自己的 kext 做的，就不再依赖它们**。
