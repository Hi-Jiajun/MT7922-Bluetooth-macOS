# MT7922 蓝牙（USB `0e8d:0616`）注入方案 —— 可直接执行版

日期：2026-10-12
方法：只读（ioreg / system_profiler / plistlib / nm / otool 反汇编 / 上游源码与上游 README）。
前置阅读：`outputs/20261010-mt7922-wifi/bt-recon-20261011.md`（41 条 personality 逐条比对）。
本文档不重复 recon 的结论，只补「照着做就行」的执行细节 + 本轮新查到的实测事实。

---

## 0. 摘要（先看这段）

**要做的事**：把 `tools/bt-payload/` 里的 `SimpleMtkBtFw.kext`（+ 可选 `SimpleMtkBtPatcher.kext`）复制到 ESP 的 `Kexts/`，在 `/Volumes/OPENCORE/EFI/OC/config.plist` 的 `Kernel:Add` 里加条目，重启。

**关键前提已满足（实测）**：

- 设备已枚举：`Wireless_Device`，`idVendor=3725(0x0e8d)`、`idProduct=1558(0x0616)`、`bcdDevice=256`，三个接口 `BT_ACL_If@0/BT_SCO_If@1/BT_ISO_If@2` 都在。
- 无签名注入通道已通：`boot-args` 里已有 `amfi_get_out_of_my_way=0x1 amfi=0x80`，`AMFIPass.kext` 已启用，`SecureBootModel=Disabled`，且现有 `AirportMtk.kext / RTL812xLucy.kext / USBToolBox.kext` 都已经成功注入（`kmutil showloaded` 可见）。
- **两个 kext 都是 x86_64 单架构**（`file` 实测），本机就是 x86_64，架构没问题。

**三处必须知道的例外/更正**：

1. **`SimpleMtkBtFw.kext/Contents/Info.plist` 里那条 `0e8d:0616` personality 已经加好了**（见 §1.1），**不要再加第二条**，否则是重复工作、且可能被 `plistlib`/OC 当成两个同名条目。
2. 上游 README（实测抓取）**明确写了加载顺序**：`Lilu.kext → SimpleMtkBtFw.kext → SimpleMtkBtPatcher.kext`，并且写明「不要同时注入另一个会绑定到同一 USB 蓝牙设备的蓝牙固件加载器」。
3. `SimpleMtkBtPatcher` 的作用被我按二进制反汇编收窄了：在本机这样的现代 Lilu 下它**只**钩 `IOUSBHostDevice::deviceRequest` 一条（另一条 `IOBluetoothHostController::FindQueuedRequest` 是老 Lilu 的分支，本机不会走）；而且它**不修改**调用者的请求，只在看到一个特定 HCI opcode 时**额外**补发一个请求。详见 §3。

**建议的执行节奏**：先只上 `SimpleMtkBtFw`（§2 的最小风险组合）→ 重启 → 按 §6 的 A/B 步看日志确认 `dev_id` 与固件下载 → 再决定要不要上 patcher。

---

## 1. 精确的 Info.plist 修改内容

### 1.1 现状（先核对，别重复劳动）

本仓 `tools/bt-payload/SimpleMtkBtFw.kext/Contents/Info.plist`（mtime 2026-10-11 03:06）**已经是 42 条 personality**：

```
$ python3 比对上游源码 Info.plist（laobamac/SimpleMtkBtFw master）与本仓 payload：
upstream source personalities: 41
local (deployed-to-be)      : 42
only in local   : ['SimpleMtkBtFw_MT7922_0e8d_0616_added_by_AirportMtk_session']
only in upstream: []
```

即：**上游 41 条原封不动 + 本仓多加了唯一一条 `0e8d:0616`**（键名里写了 `added_by_AirportMtk_session`，是那个 WiFi 会话加的）。
上游源码里 `0e8d` 名下只有两条：`SimpleMtkBtFw_MT7921_0e8d_0608`（idProduct 1544）与 `SimpleMtkBtFw_MediaTek_Generic`（**设备级** `bDeviceClass=224/1/1`）——两条都不匹配我们，所以这条新 personality 是必需的。

### 1.2 那条 personality 的完整 XML（可粘贴；键名可保留现状）

风格与既有 41 条完全一致（tab 缩进，十进制 id）：

```xml
		<key>SimpleMtkBtFw_MT7922_0e8d_0616</key>
		<dict>
			<key>CFBundleIdentifier</key>
			<string>com.laobamac.SimpleMtkBtFw</string>
			<key>IOClass</key>
			<string>SimpleMtkBtFw</string>
			<key>IOMatchCategory</key>
			<string>SimpleMtkBtFw</string>
			<key>IOProbeScore</key>
			<integer>4000</integer>
			<key>IOProviderClass</key>
			<string>IOUSBHostDevice</string>
			<key>idProduct</key>
			<integer>1558</integer>
			<key>idVendor</key>
			<integer>3725</integer>
		</dict>
```

放在 `<key>IOKitPersonalities</key><dict>` 内的**任意位置**即可（字典无序）。**本仓 payload 已存在等价条目**（键名 `SimpleMtkBtFw_MT7922_0e8d_0616_added_by_AirportMtk_session`，内容完全相同）——要么什么都不做，要么把键名改成上面这个更规范的名字，**不要两条并存**。

### 1.3 每个键为什么取这个值

| 键 | 值 | 依据 |
| --- | --- | --- |
| `IOProviderClass` | `IOUSBHostDevice` | 上游 41 条全部用它；我们的设备在 ioreg 里就是 `IOUSBHostDevice`（`Wireless_Device@01000000`）。**不能用 `IOUSBHostInterface`**（那是接口层对象，Apple 的复合设备驱动已经占着）。 |
| `idVendor` | `3725` = `0x0e8d` | ioreg 实测 `"idVendor" = 3725`；十进制是上游与本 kext 既有条目的写法（IOKit 比较的是数值，十进制/十六进制等价）。 |
| `idProduct` | `1558` = `0x0616` | ioreg 实测 `"idProduct" = 1558`。 |
| `IOClass` | `SimpleMtkBtFw` | 上游一致（C++ 驱动类名）。 |
| `IOMatchCategory` | `SimpleMtkBtFw` | **关键键**。IOKit 里同一 provider 上不同 `IOMatchCategory` 的客户端可以并存；我们的驱动要跟 `AppleUSBHostLegacyClient`、`AppleUSBHostCompositeDevice` 共同挂在同一个设备上，用自有 category 才能共存（上游 41 条也都是这么写的）。 |
| `IOProbeScore` | `4000` | 上游统一值；足够压过默认分（0），保证 probe 胜出。 |
| `CFBundleIdentifier` | `com.laobamac.SimpleMtkBtFw` | 必须与 kext 的顶层 `CFBundleIdentifier` 完全一致，否则 kernel 找不到实现。 |

**故意不加的键**：`bDeviceClass/bDeviceSubClass/bDeviceProtocol`（我们设备级是 IAD `0xEF/2/1`，加了反而不匹配——这正是 recon §2.1 那个坑）、`IOProviderMergeProperties`、`IOResourceMatch`（那是另一类 provider 用的）。

### 1.4 改完怎么校验

```bash
plutil -lint /Volumes/OPENCORE/EFI/OC/Kexts/SimpleMtkBtFw.kext/Contents/Info.plist   # 期望: OK
python3 - <<'EOF'
import plistlib,glob
p=plistlib.load(open('/Volumes/OPENCORE/EFI/OC/Kexts/SimpleMtkBtFw.kext/Contents/Info.plist','rb'))
pers=p['IOKitPersonalities']
hits=[k for k,v in pers.items() if v.get('idVendor')==3725 and v.get('idProduct')==1558]
print("personalities:",len(pers))      # 期望 42
print("0e8d:0616 entries:",hits)       # 期望恰好 1 条
print("CFBundleIdentifier:",p['CFBundleIdentifier'])
EOF
```

另：把 payload 与 ESP 里的副本做字节比对，确认复制没走样（`shasum -a 256` 两边一致）。

> 本机**没有** `ocvalidate`（ESP `Tools/` 里只有 `NullMothSafe.efi`，工作区里也没有），所以配置校验只能靠 `plutil -lint` + 人工 diff。另外注意一条历史教训（`docs/handoff-mt7922-wifi.md` §5 记的）：`kmutil print-diagnostics` 只查 kext 级依赖、**不查单个符号**，别把它当符号校验。

---

## 2. 加载顺序

### 2.1 上游官方顺序（实测抓取 README）

> Installation — Load order: 1. `Lilu.kext` 2. `SimpleMtkBtFw.kext` 3. `SimpleMtkBtPatcher.kext`
> 「Do not inject another Bluetooth firmware loader that binds to the same USB Bluetooth device.」

### 2.2 最小风险组合（建议第一次就用这个）

| 顺序 | 条目 | 本次是否加 |
| --- | --- | --- |
| 现有 idx 0 | `Lilu.kext`（已启用） | 不动 |
| **新增（追加到列表末尾）** | `SimpleMtkBtFw.kext` | **加** |
| 第二步再加 | `SimpleMtkBtPatcher.kext` | 先不加（见 §3） |

理由：

- OC 按 `Kernel:Add` **列表顺序**注入，依赖必须在被依赖者之后；`SimpleMtkBtFw` 只依赖 `com.apple.iokit.IOUSBHostFamily` + `com.apple.kpi.*`（我按 plist 实测过它的 `OSBundleLibraries`：**不含 Lilu**），所以 Lilu 在不在它前面都不影响它；但 patcher 依赖 `as.vit9696.Lilu 1.2.0`，必须在 Lilu 之后。
- 追加到**列表末尾**（现在共 15 条，新条目就是 index 15/16）做的 diff 最小，也不与任何现有条目产生依赖关系。
- 若 OC 启动时报依赖相关错误（本机从来没遇到过），再把这两条挪到 `Lilu.kext` 正后面。

### 2.3 要加的两条 `Kernel:Add` 条目（键集合与现有条目逐字一致）

现有一条的样子（实测 `Kernel:Add[13]`）：
`{'Arch':'Any','BundlePath':'AirportMtk.kext','Comment':'AirportMtk (MT7922 IO80211 bring-up)','Enabled':True,'ExecutablePath':'Contents/MacOS/AirportMtk','MaxKernel':'','MinKernel':'','PlistPath':'Contents/Info.plist'}`

照抄这个形状，两条新的分别是：

```
Arch            = Any
BundlePath      = SimpleMtkBtFw.kext
Comment         = SimpleMtkBtFw 1.0.0 (MediaTek USB BT fw loader)
Enabled         = True
ExecutablePath  = Contents/MacOS/SimpleMtkBtFw
MaxKernel       = (空)
MinKernel       = (空)
PlistPath       = Contents/Info.plist
```

```
Arch            = Any
BundlePath      = SimpleMtkBtPatcher.kext
Comment         = SimpleMtkBtPatcher 1.0.0 (Lilu plugin, 第二步再加)
Enabled         = True
ExecutablePath  = Contents/MacOS/SimpleMtkBtPatcher
MaxKernel       = (空)
MinKernel       = (空)
PlistPath       = Contents/Info.plist
```

`MinKernel/MaxKernel` 留空 == 全版本生效，与现有 13 条启用项一致（本机是 macOS 15.8.1 / 24H32）。

---

## 3. `SimpleMtkBtPatcher.kext` 要不要一起上？

### 3.1 我按二进制反汇编看到的行为（不是照抄 recon）

| 事实 | 证据（地址 = `tools/bt-payload/SimpleMtkBtPatcher.kext/Contents/MacOS/SimpleMtkBtPatcher` 内偏移） |
| --- | --- |
| `init()` 读 Lilu 版本号，**只注册一个** onKextLoad：版本 `<=0x14` 时盯 `IOBluetoothFamily`，`>0x14`（本机现代 Lilu 属于这档）时盯 `IOUSBHostFamily` | `0x9de`–`0xa27` |
| `processKext()` 里同样再判一次：现代分支只对 `IOUSBHostFamily` 路由 `IOUSBHostDevice::deviceRequest`；`IOBluetoothHostController::FindQueuedRequest` 那条只在旧 Lilu 分支才会执行 | `0xa54`–`0xbc7`（字符串常量就在 `0xa8d`、`0xb1e`） |
| 钩子**不修改**调用者参数：只在 `descriptor != NULL && data == NULL` 时做一次 `prepare → readBytes(min(len,0xFE)) → complete` 快照，然后看快照前 2 字节 | `0xc50`–`0xf1d` |
| 看到 HCI opcode `0x200B`（LE Set Scan Parameters）且内部标记未置位：用原函数**额外**同步下发一次 `bmRequestType=0x21 / bRequest=0xE0 / wLength=9`（9 字节静态数据 `randomAddressHci`）的请求，然后照常放行原请求；看到 `0x0C03`（HCI Reset）清标记；其它一律透传 | 同上（`0xd72` 比 `0x200B`、`0xebd` 比 `0x0C03`） |
| 路由失败不算致命：会 `lilu_os_log("… failed to resolve %s, error = %d")` 然后 `clearError()` 继续 | `0xb74`–`0xba6` |
| 本机 KC 里**有** `IOUSBHostDevice::deviceRequest` 的符号串（`BootKernelExtensions.kc` 命中 1 次），**没有** `IOBluetoothHostController` / `FindQueuedRequest` 任何符号串（两个 KC 都 0 命中） | `grep -c --text` 实测 |

### 3.2 风险与收益

风险（都属"低但我不能证明是零"，逐条给依据）：

1. **全局钩子**：`IOUSBHostDevice::deviceRequest` 是本机**所有** USB 设备的类请求入口。对每个「带 descriptor、不带 data」的调用，它都会多做一次 ≤0xFE 字节的缓冲拷贝（有开销，非致命）。
2. **快照语义怪**：IN（设备→主机）请求在入口处读到的其实是调用者缓冲区的**旧内容**；理论上存在"旧内容头两字节恰好是 0x200B"从而误触发一次随机地址注入的极小概率（且触发后标记置位，只在下次 HCI Reset 前不再重复）。
3. **同步补发请求**：注入的那个请求是在钩子里**同步**调原函数下发的（completion=NULL，超时 0x2710=10000），若目标设备不响应会阻塞到超时，表现为短时卡顿。
4. 它钩的是 USB 核心 + 依赖 Lilu；一旦出问题，排查面比"只上 Fw"大得多。

收益（也是它唯一的作用）：给"主机开始 LE 扫描"这一刻打补丁（补发随机地址 / 相关重发），即**影响的是扫描/连接阶段**，而不是"设备能否被识别、固件能否下载"阶段。

### 3.3 建议（明确）

**先只上 `SimpleMtkBtFw.kext`。** 只有当 §6 显示"固件下载成功、`FirmwareLoaded=Yes`，但蓝牙扫描/配对异常"时，再把 `SimpleMtkBtPatcher.kext` 加上（它必须排在 Lilu 之后）。

另一个支持"分开上"的事实：Fw 的 `OSBundleLibraries` 实测**不含 Lilu**，两者互相独立；上游 README 也是把 patcher 列为独立第二步。

---

## 4. 风险清单（按"最可能先咬人"排序）

1. **固件名不落在内嵌表里** → 日志 `Firmware <name> not found` + `Failed to load firmware file: <name>`。
   依据：我把 kext 的 `fwList` 表逐字节 dump 出来，**只有 3 条**（名字 + 大小实测）：
   `BT_RAM_CODE_MT7961_1a_2_hdr.bin`(386,516B)、`BT_RAM_CODE_MT7922_1_1_hdr.bin`(515,514B)、`BT_RAM_CODE_MT7961_1_2_hdr.bin`(400,062B)。
   名字由 `dev_id` + `(fw_version & 0xff) + 1` + `fw_flavor` 拼成 → 我们必须正好落到这 3 个名字之一（正常 ROM 状态下 MT7922 → `..._MT7922_1_1_hdr.bin`）。
2. **`dev_id` 不在 `{0x7922, 0x7961}`** → `Unsupported MediaTek hardware variant: 0x%08x`。见 §7 的完整判定链与"判不出来"的部分。
3. **抢不到 / 打不开设备** → `start fail, can not open device`。
   依据：`SimpleMtkBtFw::start()` 会对设备做独占 `open(this)`（地址 `0x2d72`–`0x2d8b`，失败即该日志）。同一设备上同时有 Apple 的 `AppleUSBHostLegacyClient` / `AppleUSBHostCompositeDevice`。用不同 `IOMatchCategory` 可以并存（见 §1.3），但**独占 open 会不会被 Apple 侧先占**本轮没实测。
4. **`setConfiguration` 会真的被调用** → `initConfiguration()` 无条件执行 `setConfiguration(bConfigurationValue, …)`（日志 `set configuration to %d`，地址 `0xb88`–`0xc40`）。这会在启动瞬间对设备做一次配置设置，可能让其它 interface 客户端短暂重枚举。成功率应该很高（配置值本来就是 1），但日志里看到这两行不要误会成故障。
5. **端口/端口图**（这条其实是"已解决、别碰坏"）：设备挂在 **AMD XHCI `XH00`**（UTBMap 用 `IOPCIPrimaryMatch=0x43fc1022` 匹配它），已枚举；UTBMap 给 XH00 映射了 17 个端口，其中**端口 16（`HS11`，`UsbConnector=255`）就是蓝牙那个**；`Kernel:Quirks:XhciPortLimit=True` 也开着。→ **本方案不需要改端口图**；若主代理因别的原因重做 UTBMap，必须确认这一项还在，否则会退回"枚举不到"。
6. **`amfi` / 未签名 kext**：已放宽（`boot-args` 里 `amfi_get_out_of_my_way=0x1 amfi=0x80`、`csr-active-config=0x0A0C0000`、`SecureBootModel=Disabled`、`AMFIPass.kext` 启用），并且 `AirportMtk`/`RTL812xLucy`/`USBToolBox` 已经是同路径的成功先例 → **判定为不是风险**。
7. **别的蓝牙固件加载器**：上游 README 明说不要共存；本机当前没有（`kmutil showloaded` 里只有 `com.apple.iokit.IOBluetoothFamily`）。→ 保持现状即可。
8. **固件下载成功后 macOS 不接手**（最不确定的一环）：上游 README 断言"固件初始化完成后蓝牙功能由 macOS 接管"，recon 也从代码看到它 close 设备 + 改名。但我在本机两个 KernelCollection 里**找不到任何 USB 蓝牙传输类名**（`IOBluetoothHostController*`、`*USBTransport`、`BluetoothHostController` 全 0 命中；只有 `IOBluetoothHCIController` 那个挂在 `IOResources` 上的 personality）。所以"苹果会派哪个驱动接走"这件事我**没法静态证明**。观测判据见 §6 步骤 C；若卡在这里，下一步方向是补丁 macOS 蓝牙栈（社区常见做法是 BlueToolFixup 之类）——**这属于推断，且超出上游文档范围**。
9. **开机风险 / 回滚通道**：注入的 kext 在启动早期运行，最坏情况（panic）可能导致进不了正常系统。改前必须留 `config.plist` 备份 + 有能改 ESP 的第二条路径（Windows 双系统或 macOS 安装盘）。见 §5。
10. **`Kernel:Quirks:ExtendBTFeatureFlags` 现在 = False**：我查了 OpenCore 官方文档，这个键写的是「Set `FeatureFlags` to `0x0F` for full functionality of Bluetooth, including Continuity」，**Requirement: 10.8-11** —— 在 15.8.1 上大概率无效。**不建议**为了蓝牙去动它（若真要看效果，属于低优先实验项）。

---

## 5. 回滚方法（精确到命令）

### 5.1 先备份（照本仓 ESP 的既有命名惯例）

ESP 上已有 `config.plist.hl-bak-before-<描述>-<时间戳>` 这一系列（例：`config.plist.hl-bak-before-connectx3hide-20261010-183931`），新的一次照抄：

```bash
ts=$(date +%Y%m%d-%H%M%S)
cp -p /Volumes/OPENCORE/EFI/OC/config.plist \
      "/Volumes/OPENCORE/EFI/OC/config.plist.hl-bak-before-mt7922bt-${ts}"
ls -l /Volumes/OPENCORE/EFI/OC/config.plist*
```

（`config.plist` 属主是 `hiliang`，**不需要 sudo**。kext 目录同理。）

### 5.2 回滚（三档，按代价从小到大）

**A. 只关掉注入（最小、最快）** —— 把新加的两条 `Enabled` 改成 `false`：

```bash
/usr/libexec/PlistBuddy -c "Print :Kernel:Add" /Volumes/OPENCORE/EFI/OC/config.plist   # 先看顺序/下标
/usr/libexec/PlistBuddy -c "Set :Kernel:Add:15:Enabled false" /Volumes/OPENCORE/EFI/OC/config.plist
# 若 patcher 是第 16 条: -c "Set :Kernel:Add:16:Enabled false"
plutil -lint /Volumes/OPENCORE/EFI/OC/config.plist
```

**B. 整文件回滚（最稳）**：

```bash
cp -p "/Volumes/OPENCORE/EFI/OC/config.plist.hl-bak-before-mt7922bt-<时间戳>" \
      /Volumes/OPENCORE/EFI/OC/config.plist
plutil -lint /Volumes/OPENCORE/EFI/OC/config.plist
diff <(plutil -p /Volumes/OPENCORE/EFI/OC/config.plist) <(plutil -p /Volumes/OPENCORE/EFI/OC/_backup-20261008-224929/config.plist.esp-old)   # 辅助确认（非必须）
```

**C. 顺带把 kext 移出（非必须）**：OC 只认 `Kernel:Add` 里列出的 kext，文件留着不列条目是安全的。若一定要清：

```bash
mkdir -p /Volumes/OPENCORE/EFI/OC/_bt-rollback-$(date +%Y%m%d-%H%M%S)
mv /Volumes/OPENCORE/EFI/OC/Kexts/SimpleMtkBtFw.kext \
   /Volumes/OPENCORE/EFI/OC/Kexts/SimpleMtkBtPatcher.kext \
   /Volumes/OPENCORE/EFI/OC/_bt-rollback-*/
```

（用 `mv` 到备份目录，**不要 `rm -rf`**。）

**不用做的**：NVRAM 无需清（本方案不动 NVRAM、不改 `boot-args`）；不需要重建 kernel cache（OC 注入发生在启动期）。

**进不去系统时**：从 Windows（`/Volumes/Untitled` 是只读挂载，得在 Windows 里操作）或 macOS 安装盘挂载 ESP，把 5.1 的备份覆盖回去；ESP 上还有一串更早的 `config.plist.*` 备份可按时间挑。

**回滚后的验证**：`kmutil showloaded | grep -i laobamac` 应为空；`log show --last 5m --predicate 'eventMessage CONTAINS "SimpleMtkBtFw"'` 无输出。

---

## 6. 验证判据（重启后按 A→B→C 跑）

### 步骤 A：kext 有没有被加载

```bash
kmutil showloaded | grep -iE 'laobamac|SimpleMtk'
```

期望：出现 `com.laobamac.SimpleMtkBtFw (1.0.0)`（装了 patcher 就再多一行 `com.laobamac.SimpleMtkBtPatcher (1.0.0)`）。
没有这行 → 注入没生效（检查 `Kernel:Add` 条目、`Kexts/` 目录、`Enabled`）。

### 步骤 B：驱动有没有 attach、固件有没有下载

```bash
sudo dmesg | grep -iE 'SimpleMtkBtFw|BtMtk' | tail -40
# 或（dmesg 被 SIP 限制时）：
log show --last 10m --predicate 'eventMessage CONTAINS "SimpleMtkBtFw"' --info --debug | tail -40

ioreg -c SimpleMtkBtFw -r -l -w0 | grep -E '"fw_name"|"IOClass"|"IOProbeScore"|"IOMatchCategory"'
ioreg -p IOUSB -l -w0 | grep -iE 'FirmwareLoaded|Bluetooth USB Host Controller'
```

期望的日志顺序（**字符串全部取自 kext 二进制，实测**）：

```
SimpleMtkBtFw: Driver Start()
SimpleMtkBtFw: BT init succeed
SimpleMtkBtFw: reg=0x70010200 value=0x........      <- ★ 这就是 §7 的关键读数
SimpleMtkBtFw: MediaTek dev_id=0x7922 fw_version=0x........ fw_flavor=…
SimpleMtkBtFw: MediaTek firmware file: BT_RAM_CODE_MT7922_1_1_hdr.bin
SimpleMtkBtFw: Found device firmware BT_RAM_CODE_MT7922_1_1_hdr.bin
SimpleMtkBtFw: HW/SW Version: 0x........, Build Time: …, sections=…
SimpleMtkBtFw: MediaTek device setup complete
SimpleMtkBtFw: Clean up...
```

IORegistry 判据（上游 README 也是这么写的）：**USB 设备节点上出现 `"FirmwareLoaded" = Yes`**，驱动实例上有 `"fw_name" = "BT_RAM_CODE_MT7922_1_1_hdr.bin"`。

⚠️ **不能只靠设备改名判断成功**：反汇编实测，`start()` 的**失败分支同样**会把设备改名成 `Bluetooth USB Host Controller` 并把 `FirmwareLoaded` 置成 `false`（地址 `0x2e63`–`0x2e9b`）。所以判据是 `FirmwareLoaded` 的**布尔值** + 日志，而不是节点名字。

### 步骤 C：macOS 有没有接手

```bash
system_profiler SPBluetoothDataType
ioreg -c IOBluetoothHCIController -r -l -w0 | grep -E 'CFBundleIdentifier|IOProviderClass|BluetoothTransportConnected|Address'
```

期望：

- `Bluetooth Controller` 的 `Address` **不再是 NULL**（变成真实 BD_ADDR），且不再显示 `Chipset: BCM_4350C2 / Firmware v0 c0 / Transport: UART` 这套兜底占位。
- `ioreg` 里除了原来那个 `IOProviderClass = IOResources` 的占位控制器，出现一个**与 USB 设备相关的蓝牙控制器实例**（判据：它的祖先链里能找到 `Wireless_Device` / `Bluetooth USB Host Controller`，或 provider 不再是 `IOResources`）。

⚠️ `Transport:` / `Chipset:` 这两行是 user-space 的 `SPBluetoothReporter` 拼的（实测该二进制里有 `UART`、`hciTransportType`、`controller_transport` 串），**具体会显示成什么字符串本轮没实测**，所以不要把"显示成 MediaTek"当唯一判据；用 `Address` 非 NULL + ioreg 的 provider 链来判。

### 步骤 D：失败时逐条对号入座

| 现象 | 含义 | 下一步 |
| --- | --- | --- |
| `ioreg -c SimpleMtkBtFw` 无输出，且日志里一行 `SimpleMtkBtFw` 都没有 | personality 没匹配上 / kext 没加载 | 核 ESP 那份 Info.plist 是否有 `0e8d:0616` 条目；`kmutil showloaded` 再看一次 |
| `Driver Start fail, not usb device` | provider 不是 `IOUSBHostDevice` | 检查 `IOProviderClass` |
| `start fail, can not open device` | 独占 open 失败 | 看是否有别的驱动占着设备 |
| `Failed to get initial device id` / `Failed to get device id` | `0x80000008` 或 `0x70010200` 的控制传输失败 | 设备没响应（掉电/复位状态），多半要再看 §7 |
| `Unsupported MediaTek hardware variant: 0x%08x` | `dev_id` 既不是 0x7922 也不是 0x7961 | §7 的"判不出来"部分变成必须解决的前置 |
| `Firmware <name> not found` / `Failed to load firmware file: <name>` | 拼出来的固件名不在内嵌 3 条里 | 记下日志里的名字，等价于 §4-1 |
| `Failed to set up firmware: <…>` / `Failed to send WMT …` / `Failed to read WMT event …` | 固件下载阶段通信失败 | 看是否有 patcher 干扰（如已装） |
| `can not find Bluetooth HCI interface` / `can not open interface` | 3 个 `0xE0` 接口里没挑到 HCI 那个 / 打不开 | 用 `ioreg` 复核接口与端点（要 interrupt-in + bulk-in + bulk-out） |
| 装了 patcher 时 | 看 patcher 自己的日志 | `log show --last 10m --predicate 'eventMessage CONTAINS "smbtp"' \| tail -20`，看 `… routed <symbol>` 或 `… failed to resolve <symbol>` |

---

## 7. 任务书 §7 的确认：`devId` 读回值从哪来、我们会不会被拒

### 7.1 能确定的（反汇编实测 + 上游 Linux 逐字对照）

- **读回值的来源 = 芯片寄存器本身**，不是 kext 里的表、也不是 VID/PID 推断：
  `BtMtk::readDeviceId(addr,&v)` 其实只是 `jmp regRead`（地址 `0x1c64`），
  `BtMtk::regRead()`（地址 `0x2702`）最终发的是
  `USBDeviceController::controlRequest(bmRequestType=0xC0, bRequest=0x63, wValue=addr>>16, wIndex=addr&0xFFFF, buf, 4, timeout=10000)` —— 一次 **vendor 控制传输读 4 字节**。
  对照 Linux（本轮实测抓取 `btmtk.c`）：`btmtk_usb_reg_read()` 用的是 `usb_control_msg_recv(udev, 0, 0x63, USB_TYPE_VENDOR|USB_DIR_IN, reg>>16, reg&0xffff, buf, 4, …)` —— **协议完全一致**，是同一个寄存器接口的移植。
- **判定顺序**（地址 `0x190c` 起，与 `btmtk.c:1317-1367` 一一对应）：
  先读 `0x80000008`；若 `== 0x7663` 就当 MT7663（我们不是）；否则依次读 `0x70010200`=`dev_id`、`0x80021004`=`fw_version`、`0x70010020`=`fw_flavor`（`fw_flavor = (x & 0x80) >> 7`）。
- **接受集**（地址 `0x19ab`/`0x19b3`）：`dev_id == 0x7961 || dev_id == 0x7922`，其它值走 `Unsupported MediaTek hardware variant: 0x%08x`。
  上游 README 也明写：「At runtime, the firmware path only accepts MediaTek internal device IDs `0x7961` and `0x7922`.」
- **固件名**由 `dev_id` + `(fw_version & 0xff) + 1` + `fw_flavor` 拼（地址 `0x1c6e`），并且必须命中内嵌的 3 条之一（§4-1 已 dump 名字与字节数）。正常 ROM 状态（未加载过固件）下 `fw_version` 低字节为 0 → MT7922 得到 `BT_RAM_CODE_MT7922_1_1_hdr.bin`，正好在表里。

### 7.2 判不出来的（**明说：只读手段判不出来，本轮没实测**）

**本机这颗 RZ616 的 Bluetooth ROM 在 `0x70010200` 上到底返回什么值**，无法只读判定。
原因（不是"懒得做"，是通道本身不存在）：要读这个寄存器，必须对 USB 设备下发 vendor 控制传输；用户态拿不到这种访问（`IOUSBHostDevice` 的独占 open / user client 需要内核侧驱动与授权），而本轮任务明确禁止加载任何 kext、禁止改动任何设备状态。**所以只能等装上去用 §6 步骤 B 的 `reg=0x70010200 value=0x…` 那一行实测。**

### 7.3 现有间接证据（都指向 `0x7922`，但都不是本机实测）

1. **MediaTek 自己的 Windows 驱动**把硬件 id `USB\VID_0E8D&PID_0616&MI_00` 命名为 **「RZ616 Bluetooth R Adapter」**（驱动站页面，2023 版驱动）。
2. **多份 Linux 报告把这两个 ID 配成同一张卡**：Arch BBS 明确写「onboard MediaTek MT7922/RZ616 (PCIe `14c3:0616`, Bluetooth USB `0e8d:0616`)」；EndeavourOS 帖里 `chip-ID: 0e8d:0616` 与本机同款的 MT7922 卡并列，且那段日志里 `hci0: Device setup in 1284219 usecs` 说明 **Linux 的 btmtk 走完了 dev_id 判定并成功下载固件**（btmtk 遇到不认识的 dev_id 会直接报 `Unsupported MediaTek hardware variant` 并失败）。
3. **本机 WiFi 半边实测**：`ioreg` 里是 `pci14c3,616`（`device-id = 0x0616`、`subsystem-id = 0x0616`），与 MT7922/RZ616 一致（`docs/handoff-mt7922-wifi.md` 也记了 Windows setupapi 里的 `PCI\VEN_14C3&DEV_0616&SUBSYS_061614C3`）。

**判断**：`"devId 读回 0x7922"是有依据的推断，不是实测**。好消息是 kext 的接受集包含 `0x7961`，内嵌表里也有 MT7961 的两个固件 —— 也就是说 `dev_id` 落在 `{0x7922, 0x7961}` 的任一个都能继续走；真正会翻车的是第三种值。**不要在没有实测前把它当成结论写进汇报。**

---

## 8. 三分清单

**实测（本轮在本机跑出来的）**

- 设备描述符：`idVendor=3725 / idProduct=1558 / bcdDevice=256`，`USB Product Name="Wireless_Device"`，`USB Vendor Name="MediaTek Inc."`。
- WiFi 半边：`ioreg` 为 `pci14c3,616`（device 0x0616 / subsystem 0x0616）。
- ESP 现状：`Kexts/` 里**没有** `SimpleMtkBtFw/Patcher`；`Kernel:Add` 15 条，`Lilu.kext` 在 idx 0；patches/quirk 现状（`XhciPortLimit=True`、`ExtendBTFeatureFlags=False`、`SecureBootModel=Disabled`、`csr-active-config=0x0A0C0000`、`boot-args` 含 `amfi_get_out_of_my_way=0x1 amfi=0x80`）。
- 本地 payload 的 Info.plist = 上游 41 条 + 1 条 `0e8d:0616`（`only in local` 恰好这一条）。
- 两个 kext 都是 x86_64、**完全未签名**（`codesign: code object is not signed at all`），`SimpleMtkBtFw` 未 strip（符号齐全，可反汇编）。
- `BootKernelExtensions.kc` 里存在 `IOUSBHostDevice::deviceRequest` 符号串（1 次命中）；`IOBluetoothHostController` / `FindQueuedRequest` 两个 KC 都 0 命中。
- UTBMap 给 XH00 的端口 16 / `HS11` 标了 `UsbConnector=255`，XH00 由 `IOPCIPrimaryMatch=0x43fc1022` 匹配。
- 上游 README / README_CN 的加载顺序、支持范围、排错项（原文抓取）。

**反汇编 / 上游源码推断（对二进制和上游代码的判读，不是本机运行实测）**

- Fw 的完整流程：open 设备 → `setConfiguration` → 找 HCI 接口（`0xE0/1/1` + 三个端点）→ 读 `dev_id`/版本 → 固件名 → 从内嵌表取 blob → WMT 下发 → `uhwRegWrite(0x74011890, 0x10001)`（= Linux 的 `MTK_EP_RST_OPT`）→ WMT `0x10006` 使能 → 置 `FirmwareLoaded`/`fw_name` → close 设备 + 改名。
- Fw 的 `OSBundleLibraries` 不含 Lilu；Patcher 依赖 `as.vit9696.Lilu 1.2.0`。
- Patcher 的钩子范围与行为（§3.1 表）。
- `ExtendBTFeatureFlags` 在 OC 文档里只对 10.8–11 有意义。

**不确定 / 需要实测才能定的**

- 本机蓝牙 ROM 的 `dev_id` 实际值（§7.2）。
- 固件下载完成后，macOS 到底由哪个驱动接手（本机 KC 里找不到任何 USB 蓝牙传输类名）。
- 独占 `open` 会不会被 Apple 侧驱动挡住；`setConfiguration` 的实际扰动。
- 加了 patcher 之后的实际收益（是否需要它才能扫描/配对）。

---

## 9. 给主代理的执行清单（照顺序做）

```bash
# 0) 备份 config.plist（§5.1）
ts=$(date +%Y%m%d-%H%M%S)
cp -p /Volumes/OPENCORE/EFI/OC/config.plist "/Volumes/OPENCORE/EFI/OC/config.plist.hl-bak-before-mt7922bt-${ts}"

# 1) 复制 Fw（第一次先只要它）——用 cp -R 保留 bundle 结构
cp -R <payload-dir>/SimpleMtkBtFw.kext \
      /Volumes/OPENCORE/EFI/OC/Kexts/
shasum -a 256 <payload-dir>/SimpleMtkBtFw.kext/Contents/Info.plist \
                /Volumes/OPENCORE/EFI/OC/Kexts/SimpleMtkBtFw.kext/Contents/Info.plist   # 两边必须一致

# 2) 确认 ESP 那份里的 0e8d:0616 personality 在（§1.4 的 python3 片段）

# 3) 追加 Kernel:Add 条目（PlistBuddy，逐一设键；下标以 PlistBuddy 打印的当前条目数为准）
P=/Volumes/OPENCORE/EFI/OC/config.plist
/usr/libexec/PlistBuddy -c "Add :Kernel:Add:15 dict" $P
/usr/libexec/PlistBuddy -c "Add :Kernel:Add:15:Arch string Any" $P
/usr/libexec/PlistBuddy -c "Add :Kernel:Add:15:BundlePath string SimpleMtkBtFw.kext" $P
/usr/libexec/PlistBuddy -c "Add :Kernel:Add:15:Comment string SimpleMtkBtFw 1.0.0 (MediaTek USB BT fw loader)" $P
/usr/libexec/PlistBuddy -c "Add :Kernel:Add:15:Enabled bool true" $P
/usr/libexec/PlistBuddy -c "Add :Kernel:Add:15:ExecutablePath string Contents/MacOS/SimpleMtkBtFw" $P
/usr/libexec/PlistBuddy -c "Add :Kernel:Add:15:MaxKernel string " $P
/usr/libexec/PlistBuddy -c "Add :Kernel:Add:15:MinKernel string " $P
/usr/libexec/PlistBuddy -c "Add :Kernel:Add:15:PlistPath string Contents/Info.plist" $P
plutil -lint $P
/usr/libexec/PlistBuddy -c "Print :Kernel:Add:15" $P

# 4) 重启（只有重启才生效），然后按 §6 步骤 A/B/C 收证据
```

第二步（**只有在 §6 步骤 B 成功、且扫描/配对异常时**）再加 patcher：`cp -R` 到 `Kexts/` → 在 `Kernel:Add` 末尾追加同形状的 `SimpleMtkBtPatcher.kext` 条目（`ExecutablePath = Contents/MacOS/SimpleMtkBtPatcher`，必须在 `Lilu.kext` 之后）→ 重启 → `log show … | grep smbtp` 看 `routed`。

**不要做**：不要改 ESP 里 `AirportMtk`/`IO80211FamilyLegacy`/`UTBMap` 相关条目；不要注入第二个蓝牙固件加载器；手改 XML 后**务必** `plutil -lint`。

---

## 10. 本轮没做的事

没有写 ESP、没有装 kext、没有改任何系统配置、没有重启、没有改 NVRAM、没有动 USB 端口图；除本文档外未在工作区落任何文件（反汇编/plist 读都走管道，未留中间产物）。
