# mt7922-bt-macos

让 MediaTek **MT7922 / RZ616**（USB `0e8d:0616`）在 **macOS** 上当蓝牙用：开机即用、能连耳机、能开关。

本机参考环境：AMD Ryzen 7 9700X + MAXSUN MS-Terminator B850BKB WIFI 板载网卡，
macOS 15.8.1 (24H32) x86_64。

驱动源码来自 **laobamac / SimpleMtkBtFw**（GPL-3.0）。本仓库的增量是**构建与部署**：

| 增量 | 内容 |
| --- | --- |
| 无 Xcode 重建链 | `build-bt2.sh` + `assemble-kext.sh`：本机只有 CommandLineTools，`xcodebuild` 不可用 |
| 固件更新 | 内嵌固件换成 linux-firmware 最新（见下表），逐字节比对确认 |
| NVRAM 干预 | `bluetoothInternalControllerInfo` 必须写成**这颗芯片自己的 BD 地址**，否则 macOS 会把它当 BCM_4350C2 走一套 CSR 流程并 abort |
| 验收记录 | `docs/` 里是从识别、枚举、根因到"真实对端（AirPods Pro 3）听歌成功"的逐轮记录 |

## 状态

| 能力 | 状态 | 依据 |
| --- | --- | --- |
| USB 层枚举 + macOS 建立 IOBluetoothHCIController | ✅ | `0e8d:0616` → `THIRD_PARTY_DONGLE`，`runs=1` 无崩溃 |
| 蓝牙开关、扫描、连接耳机 | ✅ | 实际连 AirPods Pro 3 正常播放（真实对端验收） |
| 免兜底冷启动 | ✅ | 不再需要任何启动后补救脚本 |
| 固件 | ✅ 最新 | `20260724143815`（linux-firmware） |
| macOS 26 Tahoe | ⚠️ 未验证 | 升级后该芯片所在的 XH00 硬件端口 16（HS11）被**主动隔离**，以压制 USB 控制器恢复风暴；蓝牙不出现是预期的，不是驱动故障 |

> 上面两行"通过"的含义是：在 macOS 15.8.1、本机这一颗芯片上，用真实耳机验收过。
> 没有测过其它芯片型号、其它 macOS 版本、其它主板的组合，不作外推。

产物 `kext/SimpleMtkBtFw.kext` 与当时两份启动介质里注入的那份逐字节相同：

```
shasum -a 256 kext/SimpleMtkBtFw.kext/Contents/MacOS/SimpleMtkBtFw
9e2c867b731ff303…      # 从 src/ 重建可复现同一个值
```

## 怎么重建

```bash
SRC_DIR=./src bash build-bt2.sh     # 编译 + 链接，输出 build/SimpleMtkBtFw
bash assemble-kext.sh               # 组装成 kext/SimpleMtkBtFw.kext，并打印 sha
```

`build-bt2.sh` 需要内核 SDK 头文件，默认路径写在脚本的 `SDK=` 里
（本项目是 `$HOME/Documents/Hackintosh-MacLocal/tools/research/mksdk`，
用 `SDK=/your/path bash build-bt2.sh` 覆盖）。怎么提取这套头文件见
`docs/bt-rebuild-chain.md`。

### 两个必踩的坑（都是实测）

1. **`zutil.c` 必须按 C++ 编译**。它体内写着 `extern "C" {`，按 C 编译直接语法错；
   而它后面的 `_start`/`_stop` 在 `extern "C"` 之外，按 C 编译会被 C++ 修饰成
   `__Z5_stopP9kmod_infoPv`。
2. **缺 `_kmod_info` 符号会伪装成链接成功**。段列表与上游发布版逐项相同，但内核以
   *kexts must have a _kmod_info symbol* 拒绝加载。本仓库用 `src/kmod-info.cpp` 补上；
   注意 `strings | grep kmod_info` 验不出来，必须用 `nm` 看符号。

## 固件

固件以 zlib 压缩数组的形式内嵌在 `src/FwBinary.cpp`，版本号是**解压后镜像的前 14 字节**。
`tools/bt-fw-version.py` 负责把它读出来（直接 `strings` 会读到镜像内部的另一个时间戳，是陷阱）：

```bash
python3 tools/bt-fw-version.py src/FwBinary.cpp
```

| 版本 | 原始大小 | 说明 |
| --- | --- | --- |
| `20260724143815` | 517398 B | **本仓库当前使用**（linux-firmware 最新） |
| `20251020151255` | 494321 B | 上游发布包里那份 |
| `20250625154126` | 532910 B | 上游更早的 loader 段 |

换固件：把 linux-firmware 的 `BT_RAM_CODE_MT7922_1_1_hdr.bin` 重新压成数组替换
`FwBinary.cpp` 中对应条目，再按上面的两步重建。

## 怎么部署

1. 把 `kext/SimpleMtkBtFw.kext` 放进 EFI 的 `EFI/OC/Kexts/`，并在 `config.plist` 的
   `Kernel > Add` 里加一条对应条目（`BundlePath` / `ExecutablePath` / `PlistPath`）。
2. **两份启动介质都要改**（内置 ESP + U 盘），否则从另一块启动就是另一套行为。
3. NVRAM 那条键（`bluetoothInternalControllerInfo`）用
   `tools/add-bt-controller-info.py` 生成/核对，它的内容是 8 个零字节 + 芯片的 6 字节 BD 地址。

本机这颗芯片的地址被固定为 `02:1a:7d:da:71:2a`（locally-administered 地址，非厂商 MAC）：
原先它与 macOS 写入 NVRAM 的缓存值（`…:13`）撞车，导致 bluetoothd 在更新钥匙串 ACL 时 abort。
换一个**两个值都不等**的地址即可，根因链见 `docs/bt-abort-root-cause.md`。

## 怎么回滚

```bash
K=/Volumes/OPENCORE/EFI/OC/Kexts
# 1) 移走 kext
mv "$K/SimpleMtkBtFw.kext" ~/Desktop/SimpleMtkBtFw.kext.disabled
# 2) 或只关掉注入（条目序号按你的 config.plist 实际值）
/usr/libexec/PlistBuddy -c 'Set :Kernel:Add:N:Enabled false' /Volumes/OPENCORE/EFI/OC/config.plist
# 3) 还原注入前的 config.plist 备份
cp /Volumes/OPENCORE/EFI/OC/config.plist.<你的备份后缀> /Volumes/OPENCORE/EFI/OC/config.plist
```

改 ESP 前先备份 `config.plist`，改完用 `plutil -lint` 校验。两份启动介质都要回滚。

## 尚未解决

- 需要更严格 SIP/AMFI 的环境下 `securityd` 仍会拒绝对 BluetoothGlobal 条目的 ACL 改写；
  本机的规避是保留 `amfi_get_out_of_my_way`（NVIDIA 驱动也需要）。属系统安全栈，不在驱动侧。
- macOS 26 上该芯片所在 USB 端口被隔离后，没有重新验证蓝牙；恢复端口需要另开一个
  USB / 冷启动窗口，并接受控制器恢复风暴的风险。
- 上游代码未做本仓库外的改动；若上游更新，需要把 `src/` 与新版重新对齐。

## 许可证与来源

本仓库以 **GPL-3.0** 发布（同上游 SimpleMtkBtFw），因为它是该项目的衍生作品。
源码出处、固件来源与本地修改的逐项说明见 [`CREDITS.md`](CREDITS.md)，
许可证全文见 [`LICENSE`](LICENSE)。
