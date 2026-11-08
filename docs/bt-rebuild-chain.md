# 蓝牙 kext 重建链（无 Xcode）

结论：已打通。本目录的脚本能用 `clang + mksdk` 重建 `SimpleMtkBtFw.kext`，
不再依赖 Xcode（本机只有 CommandLineTools）。这意味着换固件/修补驱动从此是一条命令的事。

---

## 1. 为什么需要它

上游 README 写死的构建方式是 `xcodebuild -project SimpleMtkBtFw.xcodeproj ...`，
而本机 `xcodebuild` 不可用。要更新内嵌固件（见 §6），就必须绕开 Xcode。

## 2. 打通时修掉的两个坑（都是实测踩出来的）

| 坑 | 现象 | 解法 |
| --- | --- | --- |
| `zutil.c` 要按 C++ 编译 | 按 C 编译报 `expected identifier or '('`，因为它体内写着 `extern "C" {`；而它后面的 `_start`/`_stop` 又在 `extern "C"` 之外，名字被 C++ 修饰成 `__Z5_stopP9kmod_infoPv` | 全部按 C++ 编译 |
| 缺 `_kmod_info` 符号 | 首次链接"成功"，段列表与上游发布版逐项相同，但 `nm` 只有两个 `_start/_stop`，没有 `_kmod_info`；而发布版在 `0x142ec0` 处有一个。内核会以 *kexts must have a _kmod_info symbol* 拒绝 | 手写 `kmod-info.cpp` 定义它（本仓 Wi-Fi 驱动 `driver/AirportMtk/kmod-info.c` 是同样做法） |

注意：用 `strings | grep kmod_info` 验不出来（段名不是独立字符串），必须用 `nm` 看符号。
我第一次就是被这个假通过骗了。

## 3. 产物核对（与上游发布二进制对比）

| | 段数 | nm 符号数 | 未定义符号 | `_kmod_info` |
| --- | --- | --- | --- | --- |
| 我们重建 | 9 | 457 | 326 | 有（`0x144f40`） |
| 上游发布 | 9 | 445 | 329 | 有（`0x142ec0`） |

差异来自编译器版本/参数，结构一致。

## 4. 固件在里面长什么样（决定怎么换）

`FwBinary.cpp` 里固件是 zlib 压缩后的字节数组：

```c
const unsigned char firmware_<40位哈希>[] = {0x78, 0x9C, ...};
const unsigned char *BT_RAM_CODE_MT7922_1_1_hdr_bin = firmware_<哈希>;
const long int BT_RAM_CODE_MT7922_1_1_hdr_bin_size = sizeof(firmware_<哈希>);
const struct FwDesc fwList[] = { {MTK_BT_FW("BT_RAM_CODE_MT7922_1_1_hdr.bin", ...)}, ... };
const int fwNumber = 3;
```

关键实测：固件是高熵数据，zlib 基本压不动 —— 515,478 字节的固件压缩后是 515,514 字节
（只多 36 字节包装开销）。所以"压缩"只是格式包装，尺寸几乎等于原始大小。

## 5. 怎么用

```bash
bash build-bt2.sh                        # 基线（= 上游 master 的固件）→ ./build/
SRC_DIR=./src-newfw bash build-bt2.sh    # 换新固件的变体 → ./build/
```

源码目录需含 `BtMtk.cpp/.h`、`USBDeviceController.cpp/.hpp`、`SimpleMtkBtFw.cpp/.hpp`、
`Hci.h`、`Log.h`、`linux.h`、`zutil.c/.h`、`FwData.h`、`FwBinary.cpp`、`Info.plist`，
外加本目录新增的 `kmod-info.cpp`。

## 6. 固件版本对照（本次实测）

| 来源 | MT7922 固件头部版本 | 大小 |
| --- | --- | --- |
| 本机正在注入的（上游 1.0.0 发布包） | `20251118163139` | 515,478 |
| 上游 master `FwBinary.cpp` | `20251118163447` | 515,478 |
| linux-firmware 最新 | `20260724143815` | 517,398 |

固件体内另有一个 build-time 串（例如 `20260724143303`），`strings` 抓到的常是它而不是
头部版本 —— 别混淆；要拿头部版本必须解压后读 offset 0。

## 7. 边界

- 本目录只做构建，不安装、不部署、不重启。
- `build-fw-old/` 与 `build/` 是两份 `.kext` 目录，可对比；部署仍走
  `config.plist` 的 `Kernel:Add`，由主代理统一处理。
- 重建产物未在任何机器上运行过，不当作已验证。
