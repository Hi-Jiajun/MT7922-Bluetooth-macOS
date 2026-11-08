# Round 46 — 蓝牙固件已换最新并部署（附一次险些踩到的坑）

日期：2026-10-12

用户确认"可以"，于是把蓝牙换到最新固件一起部署。

---

## 1. 部署前查出一个会白烧一次重启的问题

我构建时用的 `Info.plist` 是从 **上游 master** 抓的，它**没有**那条为
`0e8d:0616` 加的 personality：

| | personality 数 | 含 `0e8d:0616` |
| --- | --- | --- |
| 我的构建产物（未修） | **41** | **否** |
| ESP 上原那份（上游 1.0.0 发布包） | 42 | 是 |

**若直接部署，kext 根本匹配不到我们的设备**，蓝牙会毫无动静 ——
而现象会与"驱动有问题"无法区分，白烧一次重启。

修法：把带 42 条的 `Info.plist`（取自 `tools/bt-payload/`）放进两个产物，
并核对：

- personality = 42，含 `idVendor=3725 / idProduct=1558` ✓
- `CFBundleIdentifier` = `com.laobamac.SimpleMtkBtFw`，与二进制里
  `kmod_info.name` 一致 ✓
- `_kmod_info` 在 ✓

## 2. 部署结果（实测核对）

```
部署前  ESP MT7922 固件 = 20251118163447
部署后  ESP MT7922 固件 = 20260724143815   ← 最新
        sha256 = 45407b6eec4c9813（与 out-new 一致）
        另两个固件未动：20251020151255 / 20250625154126
        personality = 42，含 0616 ✓    _kmod_info ✓
```

本次**只替换了 kext 内的二进制与 Info.plist**，`config.plist` 未动
（与备份逐字节一致，已核对）。

## 3. 三条线现在的待启动状态

| 线 | ESP 上的内容 | 状态 |
| --- | --- | --- |
| WiFi | `6fb117282b96abcc`（IE 普查 + assoc req/resp IE 列表） | 未启动 |
| 蓝牙 | `45407b6eec4c9813`（**新固件 20260724143815**，42 条匹配） | 未启动 |
| AWDL | 未改动（已决定不做，理由见 round44 §3） | — |

蓝牙 `SimpleMtkBtPatcher.kext` **在 ESP 上但不注入**，符合"先只上 Fw"的建议。

## 4. 部署带来的取舍（如实说）

换新固件的代价是：**失去了"旧固件能不能用"这个基线**。

理由是取舍而非纯收益：

- 若新固件成功 ⇒ 我们要的结果拿到了，基线不再重要；
- 若失败 ⇒ 无法立刻区分"固件问题"还是"别的问题"，需要回滚到旧固件再跑一次。

**但 3/4 的基线问题与固件无关**（kext 是否注入、是否被签名拦、`devId` 能否通过
`{0x7961,0x7922}` 判定、固件名能否查到），这些用新固件同样能回答。
只有"固件下载阶段是否失败"这一项需要基线对照。

回滚已就位、一条命令（见 §5）。

## 5. 回滚

```bash
K=/Volumes/OPENCORE/EFI/OC/Kexts
B=<backup-dir>
cp -R "$B/SimpleMtkBtFw.kext" "$K/"          # 还原旧固件版（sha 8079337d3bd2ef9e）
# 或只关闭注入：
/usr/libexec/PlistBuddy -c 'Set :Kernel:Add:15:Enabled false' /Volumes/OPENCORE/EFI/OC/config.plist
```

另外两套已构建好、随时可换：

| 目录 | 固件 | sha256 |
| --- | --- | --- |
| `bt-rebuild/out-old/` | `20251118163447`（基线） | `6e8f05e8f4039aef` |
| `bt-rebuild/out-new/` | `20260724143815`（**已部署**） | `45407b6eec4c9813` |

## 6. 记账

- 部署：ESP `SimpleMtkBtFw.kext` → sha `45407b6eec4c9813`
- 备份：`tools/backup-mtk-efi-20261012-173613-pre-bt-newfw/`（含旧 kext + config.plist）
- 未改动：`config.plist`（核对与备份一致）、WiFi kext、AWDL 相关代码
- 未改动任何设备（光猫/路由器/G1600）配置
