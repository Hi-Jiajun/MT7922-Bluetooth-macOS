# MT7922 蓝牙：bluetoothd SIGABRT 根因定位（只读分析）

日期：2026-10-13。范围：**只读**取证（无部署、无重启、未改任何系统文件/kext/ESP）。
机器：macOS 15.8.1 (24H32)，iMacPro1,1。分析对象：

- `/usr/sbin/bluetoothd`（x86_64 slice，UUID `9D4727A2-1FC2-3777-BE26-7BAD0ED65EF0`，与 `.ips` 的 `usedImages[0].uuid` 完全一致）
- `/Library/Logs/DiagnosticReports/bluetoothd-*.ips`（54 份）
- 统一日志（`log show`，90 分钟窗口）
- 两个 kext 的源码/二进制（`outputs/20261010-mt7922-wifi/bt-rebuild/src/`、`tools/bt-payload/`）

---

## 0. 结论先行

1. **崩在用户态 bluetoothd 的 Broadcom VSC 应答解析器里**：`_BCM_ReadVerboseConfigVersionInfoParser`（`MobileBluetooth/Stack/vsc/bcm/bcm_vsc.c:1125`）。断言 `ByteStream_NumReadBytesAvail(*pBs) >= 1` 的含义是 **"这条 VSC 的应答载荷一个字节都没有"**（`avail = __BufLen - __ReadPos == 0`）。【实测：反汇编 + 日志逐字吻合】
2. **进程为什么走 Broadcom 路径**：不是靠读控制器的 manufacturer 判型，而是"平台默认档 + NVRAM 持久化状态"决定。本机 NVRAM（设备树 `/options`）里没有 `bluetoothInternalControllerInfo` 等 Apple 私有控制器信息，定制回调直接兜底：`External Bluetooth dongle was not found -- setting chipset to default BCM_4350C2` ⇒ **chipset = BCM_4350C2（Broadcom）** ⇒ bcm_vsc.c 的 BCM 厂商命令序列（含 Read Verbose Config Version Info）被启用。【实测：统一日志 + 反汇编】
3. **触发链（最可能）**：MTK 控制器对这条 0xFCxx 厂商命令回了"成功但零参数"的 Command Complete（或长度不匹配的应答）⇒ VSC 层消耗掉状态字节后把**空流**交给解析器 ⇒ 解析器要读第 1 个字节 ⇒ `avail == 0` ⇒ assert ⇒ `abort()` ⇒ launchd 记 `exited due to SIGABRT`、`runs` 上涨并节流。【实测 + 一处推断，见 §7】
4. **kext 侧没有"改写命令/响应长度"的代码**：`SimpleMtkBtFw` 只在 `start()` 里用 WMT(0xFC6F) 下载固件，随后 `cleanUp()` 关设备、完全不参与 bluetoothd 的 HCI 收发包；`SimpleMtkBtPatcher` 是**内核态** Lilu 插件，目标是把硬编码的 `LE Set Random Address (0x2005)` 塞进内核请求队列，**不在**本次用户态 bm3_usb 传输路径上。【源码 + 二进制实测】
5. **`secd` 不是根因**：`Failed to talk to secd after 4 attempts` 每次 keychain 查询都打，但同一进程随后 `commited /Library/Keychains/System.keychain` 成功（19:41:36.228）⇒ 只是崩溃同秒的邻居。【实测】
6. **最小修法**：优先走"让栈不要选 BCM_4350C2 档 / 让它认出内部控制器信息"（纯 NVRAM → kext 一行级）；**不建议** patch bluetoothd（SIP/签名，且任务约束禁改 `/System`）。详见 §5。

---

## 1. 六个 offset 的符号化（含 `abort()` 那一帧）

### 1.1 方法论：为什么 `atos` 给不出符号，以及正确的坐标换算

```sh
$ atos -arch x86_64 -o /usr/sbin/bluetoothd -l 0x10C888000 0x10C888000+1096126
0x0000000100000000 (in bluetoothd)      # 无符号
$ nm -arch x86_64 /usr/sbin/bluetoothd | wc -l
    1307                                # 只剩外部/导出符号，没有本地函数名
```

（另注：`atos` 不做 `base+off` 算术，必须先把地址算成绝对值再传。）

**两条必须知道的换算（我踩过）**：

| 项 | 值 | 依据 |
| --- | --- | --- |
| 该文件是 **fat**（x86_64 + arm64e），x86_64 slice 从 **文件偏移 0x4000** 开始 | `fat magic 0xcafebabe, nfat=2, cputype=0x01000007 offset=0x4000 size=0xa89bb0` | 读前 8 字节 fat 头 |
| `imageOffset`（.ips 里的十进制数）**就是 slice 内偏移**；反汇编地址 = `0x100000000 + imageOffset` | `__TEXT vmaddr=0x100000000 fileoff=0 filesize=0x9db000` | `otool -l` |
| 坑：`strings -t x` 打印的偏移比 slice 内偏移**大 0x4000**（它按文件绝对偏移算） | 实测 `strings` 报 `88768d`，真实 slice 内是 `0x88768d`（文件 `0x88b68d`） | `data.find()` 交叉核对 |

于是符号化改成：**反汇编 + `LC_FUNCTION_STARTS` 建函数边界 + 用二进制内嵌的 `funcname` 字符串给函数命名**。
命名原理：断言宏会把**函数名**当作第三个参数喂给一个 helper（除 `AssertFail %s:%d : %s` 之外还有一条带 funcname 的路径），所以每个函数内部都有若干条 `lea <funcname 字符串>(%rip), %rdx/…`。全量扫 `__text` 的 RIP 相对引用，按目标字符串分组，再用函数起点表把引用点归属到函数 ⇒ 反推出 **747 个函数的真实名字**（本次定位足够）。

### 1.2 结果表

`loadAddress = 0x10C888000`（= `usedImages[0].base` = 4505239552）；`VA = 0x100000000 + imageOffset`。

| .ips imageOffset | 十六进制 | VA | 归属 |
| --- | --- | --- | --- |
| **1096126** | **0x0010B9BE** | 0x10010B9BE | **`abort()` 的返回地址 ⇒ 调用者就是断言报告例程本身**（函数 `0x10010B980`；`0x10010B9BE` 是它 `callq _abort` 的下一条指令，同时也是**下一个函数的起点**，所以按函数表直接查会显示"落在 0x10010b9be"） |
| 1461399 | 0x00164C97 | 0x100164C97 | **`void _BCM_ReadVerboseConfigVersionInfoParser(OI_STATUS, GENERIC_HL_CB, OI_BYTE_STREAM *)`**（函数起点 `0x100164B29`；bcm_vsc.c:1125 断言点，PC 就在 `callq 0x10010b980` 之后） |
| 1404429 | 0x00156E0D | 0x100156E0D | 函数 `0x100155BA4`：函数体内挂着 `_handle_CommandComplete` / `_handle_CommandStatus` / `_handle_ReturnLinkKeys` 三个 funcname 串 ⇒ **HCI 命令完成/状态事件处理器（合并代码体）**；该帧正好是 `callq *-0x70(%rbp)`（VSC 回调函数指针）的下一条指令 |
| 1399631 | 0x00155B4F | 0x100155B4F | 函数 `0x10015543E…0x100155BA4`（二进制内无名字串）：**HCI 事件分发层**，`callq 0x100155ba4` 的返回点 |
| 836937 | 0x000CC549 | 0x1000CC549 | 函数 `0x1000CC4F0…0x1000CC582`（无名）：栈线程的事件/任务投递小函数 |
| 861664 | 0x000D25E0 | 0x1000D25E0 | 函数 `0x1000D24E7…0x1000D261A`（无名）：**BT 栈线程入口**（`_pthread_start → thread_start` 之后） |

### 1.3 原始片段 A —— 崩溃函数（`0x10010B980`，它调了 `abort()`）

```asm
10010b980: 55                   pushq  %rbp                ; ← 断言报告例程入口
10010b981: 48 89 e5             movq   %rsp, %rbp
10010b984: 41 57                pushq  %r15
10010b986: 41 56                pushq  %r14
10010b988: 53                   pushq  %rbx
10010b98a: 48 89 d3             movq   %rdx, %rbx          ; rbx  = cond 字符串
10010b98d: 41 89 f6             movl   %esi, %r14d         ; r14d = line
10010b990: 49 89 ff             movq   %rdi, %r15          ; r15  = file 字符串
10010b993: bf 75 0e 00 00       movl   $0xe75, %edi
10010b998: e8 32 09 11 00       callq  0x10021c2cf
10010b99d: 48 8d 3d ce d2 76 00 leaq   0x76d2ce(%rip), %rdi ## literal pool for: "AssertFail %s:%d : %s\n"
10010b9a4: 4c 89 fe             movq   %r15, %rsi          ; 参数1 = file
10010b9a7: 44 89 f2             movl   %r14d, %edx         ; 参数2 = line
10010b9aa: 48 89 d9             movq   %rbx, %rcx          ; 参数3 = 条件文本
10010b9af: e8 4f 05 11 00       callq  0x10021bf03         ; printf 家族
10010b9b4: e8 af 9e fc ff       callq  0x1000d5868
10010b9b9: e8 36 95 6d 00       callq  0x1007e4ef4 ## symbol stub for: _abort
10010b9be: 55                   pushq  %rbp                ; ← 帧 #3 的 PC（abort 的返回地址）
```

**交叉验证（决定性）**：整份 `bluetoothd` 里 `"AssertFail %s:%d : %s\n"` 这个格式串**只被引用一次**，就在 `0x10010B99D`——即崩溃帧内部。所以帧 `0x10B9BE` 必然是"断言报告例程"，它的调用者才是真正的断言点（下一帧）。

```sh
# 复现：扫全 __text 的 RIP 相对引用，找 "AssertFail %s:%d : %s\n"（VA 0x100878c72）的引用
$ /usr/bin/python3 /tmp/findcaller.py 0x100878c72
target 0x100878c72 refs: ['0x10010b99d']      # 唯一一处
```

### 1.4 原始片段 B —— 断言点（`bcm_vsc.c:1125`）

```asm
100164b29: 55                 pushq  %rbp                ; ← _BCM_ReadVerboseConfigVersionInfoParser 入口
100164b34: 48 89 f3           movq   %rsi, %rbx          ; rbx  = cb
100164b37: 41 89 fe           movl   %edi, %r14d         ; r14d = status
100164b3a: 85 ff              testl  %edi, %edi
100164b3c: 74 70              je     0x100164bae         ; status == 0 → 去解析应答
        ; status != 0 分支：打印 "VSC Read Verbose Config Version Info failed %!" 后返回
        ; 本机日志里没有这句 ⇒ 走的确实是"成功"路径

100164bae: 80 7a 0f 00        cmpb   $0x0, 0xf(%rdx)     ; rdx = pBs（第 3 参数）
100164bb2: 0f 85 9f 00 00 00  jne    0x100164c57         ; 流状态异常 → 断言（本机未走这条）
100164bb8: 0f b7 72 0a        movzwl 0xa(%rdx), %esi      ; esi = pBs->__BufLen
100164bbc: 0f b7 42 0c        movzwl 0xc(%rdx), %eax      ; eax = pBs->__ReadPos
100164bc0: 66 39 c6           cmpw   %ax, %si
100164bc3: 0f 86 a6 00 00 00  jbe    0x100164c6f         ; ★ BufLen <= ReadPos（可用字节 0）→ 断言
100164bc9: 80 7a 0e 01        cmpb   $0x1, 0xe(%rdx)     ; __RWFlag != BYTESTREAM_READ
100164bcd: 0f 85 ac 00 00 00  jne    0x100164c7f         ; → 断言 "(*pBs).__RWFlag == BYTESTREAM_READ"
...
100164c6f: leaq   <...bcm_vsc.c 全路径...>, %rdi           ; ← 本机实际走的断言分支
100164c76: leaq   0x6fce49(%rip), %rdx ## "ByteStream_NumReadBytesAvail(*pBs) >= 1"
100164c7d: eb 0e              jmp    0x100164c8d
100164c7f: leaq   <...bcm_vsc.c 全路径...>, %rdi
100164c86: leaq   0x6fce61(%rip), %rdx ## "(*pBs).__RWFlag == BYTESTREAM_READ"
100164c8d: be 65 04 00 00     movl   $0x465, %esi        ; ★ 0x465 = 1125
100164c92: e8 e9 6c fa ff     callq  0x10010b980         ; → 断言报告例程（帧 0x10B9BE 的调用点）
100164c97: ...                                                     ; ← 帧 #4 的 PC
```

「崩溃当秒判词」与之逐字吻合（`0x465 = 1125`）：

```
2026-10-13 19:41:36.233 bluetoothd[17405:e6ab] [com.apple.bluetooth:Stack.UNKNOWN]
  AssertFail /AppleInternal/.../MobileBluetooth/Stack/vsc/bcm/bcm_vsc.c:1125 : ByteStream_NumReadBytesAvail(*pBs) >= 1
```

### 1.5 原始片段 C —— 帧 #5：VSC 回调是从哪被调进来的

```asm
100156dd1: ba 01 04 00 00        movl   $0x401, %edx
100156dd6: e8 ca 34 00 00        callq  0x10015a2a5           ; 事件类型/掩码判定（0x401）
100156ddb: 84 c0                 testb  %al, %al
100156ddd: 75 1d                 jne    0x100156dfc
...
100156e00: 48 8d 95 c0 fd ff ff  leaq   -0x240(%rbp), %rdx    ; 第 3 参数 = 应答 ByteStream
100156e07: 44 89 f7              movl   %r14d, %edi           ; 第 1 参数 = status
100156e0a: ff 55 90              callq  *-0x70(%rbp)            ; ★ 间接调用：VSC 回调函数指针
100156e0d: e9 cf 06 00 00        jmp    0x1001574e1           ; ← 帧 #5 的 PC
```

> 该函数体（`0x100155BA4…0x100157864`）同时挂着 `_handle_CommandComplete` / `_handle_CommandStatus` / `_handle_ReturnLinkKeys` 三个名字串（同一代码体被合并）。这也是"按最近的名字串猜函数名会翻车"的原因——必须用 `LC_FUNCTION_STARTS` 定边界。

**线程归属旁证**：断言线程与 bm3_usb 的 IO 线程是**相邻的一对新线程**——同一进程内 `[bm3_usb]` 活动在 tid `5937`，断言落在 tid `5938`；另一实例 `[bm3_usb]` 在 `e6aa`、断言在 `e6ab`。
（对应的 `_pthread_start` 帧 `24069/6231` 与 `10272/6211` 分别是两种形状的线程启动链。）

---

## 2. `bcm_vsc.c:1125` 是"哪条厂商命令的响应、期望 ≥1 字节的是哪个字段"

### 2.1 命令身份【实测】

```sh
$ strings -a -t x /usr/sbin/bluetoothd | grep -i bcm_vsc
 88768d /AppleInternal/.../MobileBluetooth/Stack/vsc/bcm/bcm_vsc.c      # ← 注意：要减 0x4000
```

用 slice 内偏移直接读（确认是完整路径串，且同文件另有第二条副本在 `0x1012f67c3`——`__cstring` 的第二份拷贝）：

```
0x10088768d /AppleInternal/Library/BuildRoots/4~CW9qugA76LRUelR5Nc8l98CC8ipCrj0xAZoxlr0/Library/Caches/com.apple.xbs/Sources/MobileBluetooth/Stack/vsc/bcm/bcm_vsc.c
```

同文件里还有该函数的**函数名字符串**（断言宏的 funcname 参数）：

```
0x10088840f  void _BCM_ReadVerboseConfigVersionInfoParser(OI_STATUS, GENERIC_HL_CB, OI_BYTE_STREAM *)
0x10088266e  initVSCmdReadVerboseConfigVersionInfoCB
0x1008825da  OI_HCI_VendorCmd for VSC ReadVerboseConfigVersionInfo failed: %!
0x10088261b  VSC Read Verbose Config Version Info failed %!
```

⇒ **这是 Broadcom VSC「Read Verbose Config Version Info」（读控制器详细配置/版本信息）的应答解析器**。
（旁证：同一符号族还有 `_BCM_Load_MSFFile_EventParser`（bcm_vsc.c:0x44f）等，说明该文件整体是 Broadcom 家族 VSC 的实现。）

### 2.2 期望 ≥1 字节的是哪一段【实测（反汇编解读字段布局）】

1125 行这一对断言是宏 `ByteStream_NumReadBytesAvail(*pBs) >= 1` 的**第一次读**，位置在解析器最开头；按 `avail == 0` 判定 ⇒ **这条应答的载荷长度为 0**。紧接着代码把这条应答按 **u8, u8, u16, u16（共 6 字节）** 拆开，一次性回调：

```asm
100164bd3: 4c 8b 02             movq   (%rdx), %r8           ; r8 = pBs->__Buf
100164bd6: 48 8d 78 01          leaq   0x1(%rax), %rdi
100164bda: 66 89 7a 0c          movw   %di, 0xc(%rdx)        ; __ReadPos++
100164bde: 66 39 fe             cmpw   %di, %si ; jbe 0x100164c97   ; ← 1126 行同一断言（第二次读）
100164be7: 41 8a 0c 00          movb   (%r8,%rax), %cl      ; ★ 字段1 = 第 1 字节（1125 行要的就是它）
100164c04: 41 8a 3c 38          movb   (%r8,%rdi), %dil     ; 字段2 = 第 2 字节
100164c08: 47 0f b7 24 08       movzwl (%r8,%r9), %r12d   ; 字段3 = u16
100164c29: 47 0f b7 04 08       movzwl (%r8,%r9), %r8d    ; 字段4 = u16
100164c49: 44 89 f7             movl   %r14d, %edi
100164c4c: ff d3                callq  *%rbx               ; 回调 (status, f1, f2, f3, f4)
```

即：**1125 行的 `>= 1` 对应"应答载荷的第一个字节"**。载荷为空就是本次失败的直接原因。

回调 `initVSCmdReadVerboseConfigVersionInfoCB`（`0x10014556B…0x100145774`）把**第 2 个字节**拿去查表：

```asm
1001455dd: 41 0f b6 c6    movzbl %r14b, %eax
1001455e1: 3d 91 00 00 00 cmpl   $0x91, %eax ; jg ...
1001455e8: 83 f8 65       cmpl   $0x65, %eax
1001455ed: 83 f8 4f       cmpl   $0x4f, %eax
1001455fb: 83 f8 3f       cmpl   $0x3f, %eax ; je ...
100145604: 83 f8 49       cmpl   $0x49, %eax ; jne ...
                          ; …0x50/0x5c/0x66/0x7b/0x7d/0x7e/0x85/0x92/0x96/0x98/0xa6/0xa7…0xb8…
100145629: 48 8d 05 20 01 00 00  leaq 0x120(%rip), %rax    ; 跳转表
100145637: ff e1          jmpq   *%rcx
```

映射出来的是一组小整数 CHIPSET/PLATFORM 档位，并与 `"Unknown platform/chipset/manufacturer combination, restarting"`（`0x1008cfdc5`，位于 `0x10047D94C…0x10047DD54`）这条路径相连 ⇒ **这条 VSC 的应答正是 Apple 用来判"平台/芯片档"的输入**。所以它拿不到应答（空载荷）不只是崩一次，还直接掐断了判型。

### 2.3 0xFCxx 的**具体数值**：本机二进制查不到，标【看不出】

按提示做的几类扫描结果：

| 扫描 | 结果 |
| --- | --- |
| `_BCM_*` 代码区（0x100144000–0x100195500）内所有 `$0xfcXX` 立即数 | 只有 3 处，且都是比较：`0x100146a28 cmpl $0xfc00`（厂商命令区间判断）、`0x100152bb3 cmpl $0xfcf7`、`0x100162bc5 cmpl $0xfce9` |
| 整个 `__text` 的 `$0xfcXX` 立即数去重 | 9 个值：`fc00 fc03 fc06 fc94 fca0 fce0 fce9 fcf7 fcfe`——**没有"命令号"形态的 fc4x/fc7x** |
| `__DATA_CONST` 中"连续 ≥3 个 u16 落在 0xfc00–0xfcff"的表 | **0 组**（VSC 描述符不是静态表） |
| 全文件搜 `_BCM_ReadVerboseConfigVersionInfoParser` 指针（原样 8 字节 / 链式 fixup 低 36 位 / 低 32 位） | **0 命中** ⇒ 描述符运行时构造 |

命令下发路径（可复现）：

```asm
1001440c0: 48 8d 3d a4 14 00 00  leaq   0x14a4(%rip), %rdi  ; rdi = 0x10014556b = initVSCmdReadVerboseConfigVersionInfoCB
1001440c7: e8 a5 8d 07 00        callq  0x1001bce71
1001bce71: 48 8b 05 79 3e 89 00  movq   0x893e79(%rip), %rax ; rax = *(0x100a50cf8) —— 运行时注册的全局
1001bce86: ff d0                 callq  *%rax                ; opcode/帧格式由此全局决定
```

`0x100A50CF8` 位于 `__DATA` 的 **BSS（零填充）**区（`__DATA` filesize 仅 0x20000，该地址超出），即由 VSC 框架启动时写入 ⇒ **"命令号是多少"必须靠运行时抓包，静态查不出。**

**可直接把答案拿到的验证手段（需要你执行；本次只读任务未做）**：本机 HCI 抓包通道存在（日志里有 816 行 `[com.apple.bluetooth:PacketLogger.HCI]`，并打印过 `Init sniff[Full Logging] … gLogPath:<private>`，但 `gLogPath` 实为 `(null)`，磁盘上也没找到 `.pklg`）。用 **PacketLogger.app / BTPacketLogger** 开一次抓包并触发一次崩溃，即可看到那条 `0xFCxx` 请求与它的 Command Complete 长度——这能把 §3/§7 里"零载荷"那一条从【推断】升级为【实测】。

---

## 3. 为什么 MediaTek 芯片会走 Broadcom VSC 路径

### 3.1 判型依据：**不是** controller 上报的 manufacturer，而是"平台默认值 + NVRAM 持久化状态"

反汇编与日志同时指向同一个函数（`0x1001A1F2E…0x1001A27F8`，日志里自称 "Invoking customization callback"）。它读 **IORegistry 设备树 `/options`（即 NVRAM 侧）** 里的 Apple 私有变量，取不到就**兜底写编译期平台默认档**：

```asm
1001a2393: leaq 0x86807e(%rip), %rsi ## Objc cfstring ref: @"bluetoothInternalControllerInfo"
1001a23a2: callq _IORegistryEntryCreateCFProperty       ; 取 14 字节 CFData
1001a23aa: leaq 0x868047(%rip), %rsi ## Objc cfstring ref: @"bluetoothExternalDongleFailed"
1001a23b9: callq _IORegistryEntryCreateCFProperty
1001a23e6: callq _CFDataGetLength
1001a23eb: cmpq  $0xe, %rax                              ; 必须恰好 14 字节
1001a23fd: leaq ... "[HasInternalControllerAddr] Error -- CFDataGetLength(internalControllerInfoRef) != 14 -- it returned %d\n"
1001a2413: leaq ... "[HasInternalControllerAddr] Internal controller info missing\n"     ; ← 本机走这条
1001a2439: leaq ... "[GetExternalDongleSwitchBehavior] externalDongleSwitchBehavior = 0x%02x (never)\n"
1001a2587: leaq ... "External Bluetooth dongle %s -- setting chipset to default BCM_4350C2 \n"  ; %s ∈ {"was not found","failed setup"}
1001a25c8: leaq ... "NVRAM setting for switching to external Bluetooth dongle is set to 'never' -- setting chipset to default BCM_4350C2 \n"
1001a271c: movl $0x1770, %edi ; callq 0x100134705        ; 写档位
```

同一函数里另挂：`"Found external Bluetooth dongle -- setting chipset to THIRD_PARTY_DONGLE"`、`"External Bluetooth dongle not found -- setting chipset to default APPLE_VIRTUAL"`、`"Current chipset remains same : %d"`、变量名 `"bluetoothHostControllerSwitchBehavior"`、`"HCI Transport is set to USB / H5 / H4 / H4DS / H4BC / USB SRS / PCIE Skywalk…"`。

**本机实测日志（19:41:36 实例，顺序稳定、每个实例一致）**：

```
19:41:36.207 [Server.Core]       Starting bluetooth core stack (BUILD Aug 23 2026 20:54:48)
19:41:36.207 E [Server.Core]     LMP Routing is disabled
19:41:36.211 [Stack.UNKNOWN]     Invoking customization callback
19:41:36.211 [Stack.UNKNOWN]     [GetExternalDongleSwitchBehavior] Unable to get bluetoothHostControllerSwitchBehavior from nvram
19:41:36.211 [Stack.UNKNOWN]     [HasInternalControllerAddr] Internal controller info missing
19:41:36.211 [Stack.UNKNOWN]     External Bluetooth dongle was not found -- setting chipset to default BCM_4350C2
19:41:36.211 [bm3_usb]           [Initialize] -- Before calling pthread_cond_wait()
19:41:36.213 [bm3_usb]           [IOThreadFunc] -- USB -- calling GetProductAndVendorID()
19:41:36.213 [bm3_usb]           [GetProductAndVendorID] -- Found USB Device :
19:41:36.213                        USB Product Name = Bluetooth USB Host Controller
19:41:36.213                         USB Vendor Name = MediaTek Inc.
19:41:36.213                               idVendor = 0x0E8D
19:41:36.213                              idProduct = 0x0616
19:41:36.214 [bm3_usb]           [GetProductAndVendorID] -- Use internal Bluetooth USB Host Controller
19:41:36.214 [bm3_usb]           [GetProductAndVendorID] -- Found internal Bluetooth USB Host Controller -- vendorID = ...
19:41:36.214 [bm3_usb]           [IOThreadFunc] calling CFRunLoopRun()
...（同期：Baseband 事件、LE 缓存加载、keychain 读）
19:41:36.229 E [Server.LE.Security] Generating new local static random
19:41:36.233 [Stack.UNKNOWN]     AssertFail .../bcm/bcm_vsc.c:1125 : ByteStream_NumReadBytesAvail(*pBs) >= 1
19:41:36.233 [Stack.UNKNOWN]     Wake Up Display            （断言所在线程 tid 与之一致）
19:41:36.233 [PacketLogger.HCI]  Terminate sniff / Sniff terminated
```

**关键点**：

- 档位判定发生在 `.211`，**早于** USB 层读出 `idVendor = 0x0E8D`（`.213`）⇒ **判型不依赖 controller 上报的 manufacturer**；`Read Local Version Information` 的 manufacturer 这条线不是触发源。
- 栈里确实存在 bm3_usb 层的 USB 厂商标识检查（字符串 `"[bm3_usb][Found_USB_Dongle] -- deviceClass (%d), idVendor = 0x%04x, idProduct = 0x%04x -- Not Broadcom (0x0A5C) nor CSR (0x0A12)"`），但它只是**提示**，不参与档位选择；而且本机走的是 internal 分支。
- 控制器上报内容在栈里是空的：`check Vendor build (null) Vendor 0 0`。这不是"驱动判型的依据"，而是同一批 BCM VSC 没拿到应答的**同源后果**【推断，但与本断言同批命令，见 §3.2 的 opcode 缺口】。
- 与"地址"直接同源的线索：`could not find BT_DEVICE_ADDRESS env variable`（BlueTool 打的）、`[HasInternalControllerAddr] Internal controller info missing`、`_BTKCGetDataCopy returned -25300` → `Generating new local static random`，以及所有 keychain 条目里的 `localAddress 0x0` ⇒ 一个共同根：**NVRAM 里缺 Apple 私有持久化控制器信息**。
- **`Read Local Version Information` / `Read Local Supported Commands` 的判定痕迹在日志里看不到**（没有 manufacturer 值、没有 supported-commands 解析行），与 §2.3 的"档位不是从 controller 读出来的"一致。

### 3.2 `SimpleMtkBtFw` / `SimpleMtkBtPatcher` 有没有改写 HCI 命令/响应长度

**`SimpleMtkBtFw`（源码已读，`bt-rebuild/src/`）——没有改写命令/响应及其长度的代码**【源码】：

- 它的工作集中在 `start()` → `BtMtk::setup()`：`m_pDevice->open(this)`，然后走**厂商控制传输 + 中断端点**下载固件（`BTMTK_HCI_WMT_OPCODE 0xfc6f`、`HCI_EV_WMT 0xe4`、`sendHCIRequest()` / `readWMTEvent()` / `interruptPipeRead()`），下发 Patch 与 WMT 寄存器读写。
- 之后 `SimpleMtkBtFw::cleanUp()` 执行 `OSSafeReleaseNULL(m_pBTMtk); if (m_pDevice->isOpen(this)) m_pDevice->close(this);` ⇒ **不长期占用设备，也不参与 bluetoothd 之后的 HCI 收发**（bluetoothd 是自己经用户态 `bm3_usb` 打开 USB 端点收发 HCI）。
- 源码里没有"按 opcode 改长度 / 截断事件 / 伪造响应"的分支；`USBDeviceController::interruptPipeRead()`（`USBDeviceController.cpp:279`）只做"超时 / 长度 0 → 返回错误"的**判错**，不构造数据。
- 唯一对协议栈有语义影响的动作在 `publishReg()`：`if (version_major >= 21) { m_pDevice->setName("Bluetooth USB Host Controller"); }` —— **这一行直接决定 bluetoothd 把设备判成 internal**（见 §5 候选 2）。

**`SimpleMtkBtPatcher`（二进制已反汇编；仓库里只有 `tools/bt-payload/SimpleMtkBtPatcher.kext`，无源码）——它确实会"改写 HCI 命令"，但改的是内核态另一条路**【二进制实测】：

```
# kextstat（都已加载）
  57    8 0xffffff8004012000 ... as.vit9696.Lilu (1.7.3)
  63    0 0xffffff8004b61000 ... com.laobamac.SimpleMtkBtPatcher (1.0.0) <57 38 9 7 6 3 2 1>
 108    0 0xffffff8004a11000 ... com.laobamac.SimpleMtkBtFw (1.0.0)

# Info.plist：IOClass=SimpleMtkBtPatcher, IOProviderClass=IOResources（Lilu 插件形态）

# 它的 hook 目标与注入载荷
__ZN25IOBluetoothHostController17FindQueuedRequestEtP22BluetoothDeviceAddresstbPP21IOBluetoothHCIRequest
        # = IOBluetoothHostController::FindQueuedRequest(uint16_t, BluetoothDeviceAddress*, uint16_t, bool, IOBluetoothHCIRequest**)
__ZL16randomAddressHci @0x13b3 =
        05 20 06 94 50 64 d0 78 6b   → opcode 0x2005 (LE Set Random Address), plen 6, addr(LSB-first) 6B:78:D0:64:50:94
"%s%10s: @ [PATCH] Sending Random Address HCI %d %s"
"%s%10s: @ [PATCH] Resend LE SCAN PARAM HCI %d"
目标镜像 /System/Library/Extensions/IOBluetoothFamily.kext/Contents/MacOS/IOBluetoothFamily
```

它的定位是：**在内核的 HCI 请求队列里"找一条排队中的请求"，把载荷换成硬编码的 LE Set Random Address (0x2005)，并重发 LE SCAN PARAM**。而本机 HCI 收发发生在 **bluetoothd 用户态（bm3_usb）**，`IOBluetoothHostController` 这条内核路径不在本次断言的调用链上。

**两个必须如实说明的边界**：

1. `ls /System/Library/Extensions/IOBluetoothFamily.kext/Contents/` ⇒ 只有 `Info.plist / Resources / _CodeSignature / version.plist`，**没有 `MacOS/` 目录**（代码只在 KernelCollection 里；Resources 下 40+ 个目录全是本地化，无 VSC 表）。Lilu 有能力直接按名字从 KC 解析符号、绕过磁盘路径；我**没能**证实这次 hook 是否真的生效——KC 的 fileset 是压缩的，`strings`/`grep` 在 KC 内 `FindQueuedRequest` **0 命中**，**不能作为判据**。⇒ 标【看不出】。
2. 即便它生效，也**不会**拦到用户态 bm3_usb 的 HCI 流量 ⇒ 与本次断言**无直接因果**（【推断】；理由：断言栈帧全在 bluetoothd 用户态 + 传输由 bluetoothd 自己持有 USB 端点）。但"注入 0x2005 会不会打乱请求/应答配对"这一可能性我没有排除——要排除它，最省事的实验是**临时把 `SimpleMtkBtPatcher` 从 `Kernel:Add` 移除后对比**（属于被禁止的部署动作，本次没做）。

---

## 4. 崩溃集合与第二种形状（顺手核到的事实）

54 份 `bluetoothd-*.ips` 里有 **2 种栈顶形状**：

| 形状 | 份数 | 特征 | 归类 |
| --- | --- | --- | --- |
| A（你给的那 6 帧） | 38 | `abort ← 0x10B9BE ← 0x164C97 ← 0x156E0D ← 0x155B4F ← 0xCC549 ← 0xD25E0 ← _pthread_start` | **bcm_vsc.c:1125 断言**（本文主题） |
| B | 16 | `abort ← bluetoothd+7526560(0x72D8A0) ← bluetoothd+836833(0xCC4E1) ← libdispatch…` | **BT 栈看门狗两连击** |

形状 B 的原始串（反汇编 `0x10072D810` 附近）：

```asm
10072d810: leaq ... "BT Stack Stuck"        ; + movl $0xbd15dead, %esi（signpost）
10072d864: leaq ... "Stack watchdog timeout on thread: %s"
10072d881: movq %rbx, 0x318578(%rip)        ; 记录看门狗文本
10072d888: leaq ... "Watchdog_Timer - Stack Mutex try lock failed and watchdog has struck twice -- abort\n"
10072d891: callq 0x10021bf03
10072d89b: callq _abort                      ; ← 形状 B 的 abort
```

含义：**栈线程"拿不到栈互斥锁且看门狗已触发两次"→ 主动 abort**。这与形状 A 指向同一件事：**BT 栈线程在带锁状态下卡住 / 死掉**。两种崩溃交替出现，正好解释 `launchctl print system/com.apple.bluetoothd` 的 `runs=16 / job state=exited / spawn scheduled`（launchd 节流），以及报告里的 `consecutiveCrashCount 13`、`throttleTimeout 80`。

（另有 1 份 `ExcUserFault_bluetoothd-2026-10-13-183326.ips`，形状与 A/B 都不同；本次未展开。）

---

## 5. 最小修法候选（按代价从低到高；**本次均未执行**）

> 共同前提：断言发生在 **bluetoothd 用户态**，因此**不可能**靠内核 kext 拦下这次 `abort()`。真正能起作用的是"换掉栈走的档位 / 让栈拿到它要的信息"。

### 候选 1（最低代价｜纯 NVRAM，零代码）——把 Apple 私有控制器信息补进 NVRAM

- **改哪里**：NVRAM（设备树 `/options`，即 `nvram -p` 可见区）：
  - `bluetoothInternalControllerInfo` —— **必须恰好 14 字节** CFData（代码判 `CFDataGetLength == 0xe` 才认；它取 byte[8..11] 当 4 字节、byte[12..13] 当 2 字节）。真实 Mac 上这个变量形如"8 字节前缀 + 6 字节 BD_ADDR"再补 2 字节。
  - `bluetoothExternalDongleFailed` —— 同一函数里按 CFData 读；值 0/1 决定"内置控制器是否被判失败"。
  - 可选：`bluetoothHostControllerSwitchBehavior` 置 `never`（不置也有兜底分支，本机日志显示"取不到"时照样继续）。
- **为什么能改结果**：`HasInternalControllerAddr` 变真后，定制回调不再走"内部控制器信息缺失 → 默认 BCM_4350C2"这条兜底；同时 `localAddress 0x0` / `Generating new local static random` 这条链也会变。**注意：这只改变档位决策，能否彻底避开 BCM VSC 序列我无法保证（可能仍是 BCM_4350C2 档）** ⇒ 期望值是"流程确实改变"，不是"必然不崩"。
- **风险**：低。填错格式（长度 ≠ 14）只会让栈打印 Error 并按缺失处理，无害；可 `sudo nvram -d <name>` 撤销。
- **怎么验证**：
  `nvram -p | grep -i bluetooth` 看变量在不在；重启后
  `sudo log show --last 5m --style compact --predicate 'process=="bluetoothd"' --info | grep -E "Internal controller info|External Bluetooth dongle|setting chipset|localAddress|AssertFail"`
  期望 `Internal controller info missing` 消失、`setting chipset to …` 变化、`localAddress` 不再是 `0x0`。

### 候选 2（低代价｜kext 改 1 行）——别把 USB 节点改名成 Apple 内部控制器

- **改哪里**：`bt-rebuild/src/SimpleMtkBtFw.cpp::publishReg()` 里的
  `if (version_major >= 21) { m_pDevice->setName("Bluetooth USB Host Controller"); }`
- **为什么**：这句是 bm3_usb 把本机判成 **internal** 的唯一来源（日志：`USB Product Name = Bluetooth USB Host Controller` → `Use internal Bluetooth USB Host Controller`）。若改回 `Wireless_Device`，`GetProductAndVendorID` 会落到 **external dongle** 分支（同文件的 `"[bm3_usb][Found_USB_Dongle] … Not Broadcom (0x0A5C) nor CSR (0x0A12)"`），而定制回调里存在 `"Found external Bluetooth dongle -- setting chipset to THIRD_PARTY_DONGLE"` 分支 ⇒ **有机会整条跳过 BCM 专属初始化**。
- **风险**：**中**。internal / external 两条路径差异大，可能完全不启动 BT 栈；且这是"为了绕过而把设备伪装成另一类"，会改变栈行为。
- **怎么验证**：`sudo log show … | grep -E "Use internal|Found external Bluetooth dongle|setting chipset to|AssertFail"`；成功判据是出现 `setting chipset to THIRD_PARTY_DONGLE` 且不再出现 `bcm_vsc.c:1125`。

### 候选 3（中代价｜驱动侧把候选 1 自动化）——kext 里发布 `bluetoothInternalControllerInfo`

- **改哪里**：`SimpleMtkBtFw::start()`（或 `BtMtk::setup()` 成功后）把 14 字节 `bluetoothInternalControllerInfo`（含 6 字节 BD_ADDR）作为 IORegistry 属性 / NVRAM 语义发布；或沿用本轮已实装的 `0xfc1a` 写地址路径（`MtkBtBdAddr*`），把地址同时喂给上述语义。
- **为什么**：等价于候选 1，但不靠手敲 `nvram`，且地址来自控制器真实回读（不是固定值）。
- **风险**：中（要写对 14 字节布局，写错会被判"缺失"）。**注意它解决的是"地址缺口"，不等于消掉 VSC 断言。**
- **怎么验证**：`ioreg -p IOUSB -r -n Wireless_Device -l -w0 | egrep 'MtkBt|FirmwareLoaded'` 有值 + `log show` 里 `Internal controller info missing` 消失 + `system_profiler SPBluetoothDataType` 的 `Address` 非 `NULL`。

### 候选 4（中高代价｜唯一能"直接修在崩溃点"的方案）——接管 HCI 传输层，让那条 VSC 应答非空

- **改哪里**：让 kext **不再 close 设备**，改为独占 USB 端点并向上暴露"虚拟控制器"（或在 VSC 层做请求/应答配对与长度补齐），使那条 `0xFCxx` 的应答至少带 1 字节合法载荷。
- **为什么**：1125 行只要求"载荷非空"。要在用户态断言前把它变成非空，唯一落点是**用户态 HCI 与控制器之间**——而这一段现在被 bluetoothd 的 `bm3_usb` 独占。
- **风险**：**高**，工程量约等于重写传输层（端点多路复用 / 事件再造 / 生命周期同步），并可能引出形状 B 的栈看门狗。**不建议作为首选。**
- **怎么验证**：先用 PacketLogger 确认那条 VSC 的 opcode 与应答长度，再决定是否值得做。

### 候选 5（不建议｜记录用）——patch bluetoothd 二进制 / 注入 dylib

断言在 `/usr/sbin/bluetoothd` 内，理论最小改法是把 `0x100164C92` 的 `callq AssertFail` 改成 `nop`（或让解析器提前返回）。但 SIP + 代码签名 + 只读系统卷 ⇒ 需要关 SIP/AMFI 或 root patch；dylib 注入对系统守护进程同样需要关保护。**与本次任务约束（禁改 `/System`）冲突，列为不可选**。

---

## 5.6 已开启 `-btlfxnvramcheck`（2026-10-13 20:39，主代理补记）

`BlueToolFixup 2.7.2` **本来就已加载**（`kextstat`：`as.acidanthera.BlueToolFixup`），
它的二进制自带一组开关（`-btlfxoff` / `-btlfxdbg` / `-btlfxboardid` / `-btlfxallowanyaddr` /
**`-btlfxnvramcheck`** / `-btlfxnvramcheck` 之外的还有 `-btlfxbeta`），并 patch
`_cs_validate_page`、`/usr/sbin/BlueTool` 与 `bluetoothd`。

**极性从反汇编读出**（`0x16f3`）：

```
leaq "-btlfxnvramcheck", %rdi ; callq _PE_parse_boot_argn
testl %eax,%eax ; setne shouldPatchNvramCheck
...
cmpb $0x1, shouldPatchNvramCheck ; jne <skip>
leaq kSkipInternalControllerNVRAMCheckPatched13_3 / kSkipInternalControllerNVRAMCheck13_3
     / kSkipInternalControllerNVRAMCheckMask13_3   → KernelPatcher::findAndReplaceWithMask
```

⇒ 这个开关**开启**一个名为 **`SkipInternalControllerNVRAMCheck`** 的补丁——正对着我们崩溃链的起点
（`Internal controller info missing` ⇒ 兜底 `BCM_4350C2` ⇒ 发 BCM VSC ⇒ MTK 空应答 ⇒ 解析器断言）。

**已加入两份 config 的 boot-args**（`boot-args.sh test` / `set-efi-boot-args.py`，均有备份与 lint）。

**判据**（`-liludbg` 已在 boot-args 里，所以补丁是否命中可读）：

1. `External Bluetooth dongle … setting chipset to default BCM_4350C2` 是否消失/改变；
2. `bluetoothd` 是否还在 `bcm_vsc.c:1125` 崩（`launchctl print` 的 `runs` + 新 `.ips`）；
3. Lilu 日志里该 patch 是否命中（它的 patch 数据是按版本命名的 `…13_3`，在本机
   IOBluetoothFamily 9.0.0 上**可能找不到模式**——那样就没有任何效果，需要下一步换手段）；
4. `localAddress` 是否还恒为 `0x0`（这条更可能属于 `-btlfxallowanyaddr`，暂未开启）。

### 5.7 撤回：这个补丁在本机**不会命中**，而且**即使命中也不会救**（同日 20:41，预判定，未烧重启）

**（a）不会命中**——把补丁字节抠出来，对本机内核集合做掩码匹配：

```
BlueToolFixup 里的数据（0x1fb2 / 0x1fbc / 0x1fc6）：
  orig    = 41 80 00 01 75 00 84 db 75 00
  mask    = ff ff 00 ff ff 00 ff ff ff 00
  patched = 90 90 90 90 90 90 90 90 90 90        (全 NOP ⇒ 跳过该检查)
```

在 `/System/Library/KernelCollections/BootKernelExtensions.kc`（66 MB）里掩码匹配 **0 次命中**。
对照检查证明搜索本身没问题：同一份 kc 里 `IOBluetoothFamily` 出现 **83 次**。
⇒ 补丁数据是按更早版本命名的（`…13_3`），本机 IOBluetoothFamily 9.0.0 的代码已经不同。

**（b）即使命中也不会救**——重读 bluetoothd 的判定分支（见 §2 的反汇编）：NVRAM 信息只是**第一道**检查。
本机已经把 14 字节 `bluetoothInternalControllerInfo` 注入进 NVRAM，日志里
`Internal controller info missing` 确实**消失了**（第一道已过），但栈依旧走
`External Bluetooth dongle was not found -- setting chipset to default BCM_4350C2`
⇒ 真正卡住的是**后面那道"外部 dongle 是否存在"**的判定，与 NVRAM 检查无关。

**（c）顺带确认的架构事实**（同一份 kc 上做的字符串对照）：
`HasInternalControllerAddr` / `setting chipset to default` 在 **kc 里 0 次**（它们属于用户态 bluetoothd），
而 `bluetoothInternalControllerInfo` 这个变量名在 **kc 里出现 1 次**（内核侧另有一处使用）
⇒ 内核侧与用户态的判定是两套东西，别混。

**处置**：`-btlfxnvramcheck` 已从两份 config 撤掉（预判定无效，留着只是噪声）。
**BT 剩下的候选**：① `-btlfxboardid`（BlueToolFixup 二进制里带 `boardIdsWithUSBBluetooth` 列表，
可能改变"系统是否被认为有 USB 蓝牙"这一判定）；② `-btlfxallowanyaddr`（地址检查）；
③ 自己 fork BlueToolFixup，为 15.8 的 IOBluetoothFamily 找到等价检查并补丁数据；
④ 接管 HCI 传输层（工程量大）。①和②是零代码、可立刻试的，③需要先逆向内核那一处检查的位置。

### 5.8 BlueToolFixup 各补丁在本机（macOS 15.8）**能否命中**的完整判定（同日 20:47）

关键纠正：这些补丁**不是打内核**，而是通过 Lilu 的 **user patcher 打 userspace 的 `bluetoothd`**
——所以在内核集合里搜当然是 0 命中（上一节那个"0 次命中"的对照检查因此也解释清了）。
改在 `/usr/sbin/bluetoothd` 里逐个掩码匹配，结果是一张可用的开关表：

| 补丁数据（BlueToolFixup 内符号） | 字节模式 | 在 `bluetoothd` 里命中 | 结论 |
| --- | --- | --- | --- |
| `kVendorCheckOriginal` | `81 fa 5c 0a 00 00 74`（`cmp edx,0x0A5C; je` = Broadcom VID） | **1** | 生效中：第三方 VID 的检查已被解开 |
| `kBadChipsetCheckOriginal` | `81 f9 cf 07 00 00 72` | **2** | 生效中 |
| `kBadChipsetCheckOriginal15_4` | `81 f9 6f 17 00 00 77 30`（`cmp ecx,0x176f; ja`） | **1** | 生效中（15.4 备的 pattern 正好匹配 15.8） |
| `kSkipInternalControllerNVRAMCheck13_3` | `41 80 00 01 75 00 84 db 75 00` | 0 | **本机不可用**（`-btlfxnvramcheck` 无效，已撤） |
| `kSkipAddressCheckOriginal` | `48 89 f3 e8 00 00 00 00 85 c0 74 1d` | 0 | **本机不可用** ⇒ `-btlfxallowanyaddr` 同样无效，别试 |
| `kSkipUpdateFilePathOriginal` | `/etc/bluetool/SkipBluetooth…`（64 字节路径串） | 0 | 那是给 `/usr/sbin/BlueTool` 的，与 bluetoothd 无关 |

⇒ **BlueToolFixup 该做的（厂商检查 + 坏 chipset 检查）在本机是生效的**，所以"它没装/没生效"不是原因；
BT 卡的是**更下游**：chipset 仍被判成 `BCM_4350C2`（默认档），于是 bluetoothd 发 Broadcom 厂商命令，
而 MTK 芯片回空载荷 ⇒ `bcm_vsc.c:1125` 断言。

**因此剩下真正可试的只剩两个方向**：

1. `-btlfxboardid`：它不是字节补丁，而是让 `BaseDeviceInfo` 报告一个"带 USB 蓝牙的主板 id"
   （二进制里带 `boardIdsWithUSBBluetooth` 表，其中一个字符串是 `Mac-F60DEB81FF30ACF6`）。
   它改变的是"这台机器是否被认为应有 USB 蓝牙"——如果栈因此换一条初始化路径，就可能避开 BCM 序列。
2. 让 chipset 落到 `THIRD_PARTY_DONGLE` 而不是 `BCM_4350C2`（就是上文 §2 里那道
   `callq *0x50(%rax)` 的判定），或接管 HCI 传输层（工程量大）。

### 5.9 `-btlfxboardid` 也是空操作（同日 20:52，同样零重启预判定）

它的机制不是通用字节补丁，而是**先取本机主板 id、再去内置列表里查**：

```
callq BaseDeviceInfo::get()
leaq  boardIdsWithUSBBluetooth, %rdx     ← 14 个条目，每条 21 字节
→ 命中才走 findAndReplaceWithMask
```

列表内容（从二进制里 dump）：

```
Mac-F60DEB81FF30ACF6  Mac-9F18E312C5C2BF0B  Mac-937CB26E2E02BB01  Mac-E43C1C25D4880AD6
Mac-06F11FD93F0323C5  Mac-06F11F11946D27C5  Mac-A369DDC4E67F1C45  Mac-FFE5EF870D7BA81A
Mac-DB15BD556843C820  Mac-B809C3757DA9BB8D  Mac-65CE76090165799A  Mac-4B682C642B45593E
Mac-77F17D7DA9285301  Mac-BE088AF8C5EB4FA2
```

本机 `board-id = <"Mac-7BA5B2D9E42DDD94">`（SMBIOS 是 iMacPro1,1）——**不在列表里**
⇒ 开关加上也不会走补丁分支。**已从两份 config 撤掉**。

### 5.10 BT 的廉价手段已全部排除，剩下的是结构性工作

| 手段 | 判定 | 依据 |
| --- | --- | --- |
| `-btlfxnvramcheck` | 空操作 | 补丁 pattern 在 bluetoothd 里 0 命中（§5.7/5.8） |
| `-btlfxallowanyaddr` | 空操作 | 同上（地址检查 pattern 0 命中） |
| `-btlfxboardid` | 空操作 | 本机主板 id 不在它的 14 项列表里（§5.9） |
| 注入 `bluetoothInternalControllerInfo` | 不够 | 第一道检查已过，卡在"外部 dongle 是否存在"（§5.5/5.7） |

而 BlueToolFixup 本身**是生效的**（厂商检查、坏 chipset 检查在本机都命中，§5.8）。

⇒ 剩下两条真正可行的路：

1. **给 bluetoothd 加一条自己的补丁，把 chipset 从 `BCM_4350C2` 引到 `THIRD_PARTY_DONGLE`**。
   RE 起点已经定位：判定分支在 `0x1001a2502–0x1001a2530`（`callq *0x50(%rax)` 那道"外部 dongle
   是否有"的门），两条分支分别调用 `0x100134705`，参数 `0` = 第三方、`1` = BCM/虚拟；
   "坏 chipset" 比较用的是常数 `0x176f`。做法：fork BlueToolFixup（acidanthera 的源码），
   加一组 15.8 的 pattern + mask，像它现有的补丁那样经 Lilu user patcher 打上去——**可先做静态
   pattern 匹配预验证**（这套方法本轮已用过三次）。
2. **接管 HCI 传输层**（让那条 Broadcom VSC 的应答非空），工程量大，只在①失败后考虑。

### 5.11 已实现并部署：`mtkbtcsr` —— 让设备**自称 CSR 外部 dongle**（2026-10-13 20:44）

比"fork BlueToolFixup 打补丁"更省的一步：**改的是我们自己的 BT kext**，用的机制它本来就在用
（在 USB 设备节点上覆盖注册表属性，bm3_usb 读的正是这些属性）。根据 §2/§5.8 的判定链，
栈接受的外部 dongle 只有两种：Broadcom `0x0A5C` 或 **CSR `0x0A12`**；而 CSR 那条**不需要**
厂商固件加载——也就是纯 HCI，正是这块芯片真正会说的话。

改动（`bt-rebuild/src-newfw/SimpleMtkBtFw.cpp::publishReg`，开关 `mtkbtcsr`，默认关）：

- 打开时：`idVendor=0x0A12`、`idProduct=0x0001`、`USB Product Name="CSR Bluetooth Dongle"`，
  并**跳过**内部控制器的命名/属性；同时写属性 `MtkBtShape = "csr-external"`（关闭时写 `"internal-apple"`）
  便于直接读。
- USB 描述符、硬件、kext 匹配路径**都不动**；撤销只需去掉一个 boot-arg。

**部署**：新 BT kext 二进制 `3919a1bf78b4438f`（两份介质同哈希，旧件备份在
`tools/backup-mtk-bt-20261013-204404/`）；两份 config 的 boot-args 末尾加 `mtkbtcsr`。

**判据**（下次开机）：

1. `ioreg -l | grep MtkBtShape` → `csr-external`（改动本身生效）；
2. 日志里是否出现 `Found external Bluetooth dongle -- setting chipset to THIRD_PARTY_DONGLE`
   —— 出现即栈接受了我们的伪装（这也是唯一能把我们带离 BCM VSC 序列的东西）；
3. `bluetoothd` 是否还在 `bcm_vsc.c:1125` 崩（`runs` 与新 `.ips`）；
4. `localAddress` 是否不再 `0x0`。

**失败时的读法**：若 `MtkBtShape=csr-external` 但日志仍是 `BCM_4350C2`，说明 bm3_usb 读的不是
这几个注册表属性（或还有别的门），那就回到 §5.10 的①（fork BlueToolFixup 打补丁）。

## 6. 复现命令清单

---

## 7. 结案（2026-10-14）：这段分析的最后一块是 `bluetoothExternalDongleFailed`

本文档此前把根因定到"缺 `bluetoothInternalControllerInfo` ⇒ 兜底 `BCM_4350C2` ⇒ Broadcom VSC
应答零字节 ⇒ `bcm_vsc.c:1125` 断言"。**这条链是对的，但它只是第一层**；2026-10-14 的实测把
后面的两层补齐了，而且第二层才是"崩个不停"的原因：

1. **第一层（本文档已定）**：`mtkbtcsr` 让设备自称 CSR 外置 dongle 之后，
   栈确实改走 `THIRD_PARTY_DONGLE`，BCM 那条路不再走（08:07:28 实测）。
2. **第二层（新）**：设备地址撞车 —— SimpleMtkBtFw 用 HCI `0xfc1a` 把芯片写成
   `02:1a:7d:da:71:13`，而我们又把这个**同一个**地址当作"内置模块地址"注入
   `bluetoothInternalControllerInfo`，于是 macOS 报
   `ERROR -- Third Party Dongle has the same address as the internal module`
   并放弃 dongle，改回内置路径（= `BCM_4350C2` = `H4BC` = 等 `/dev/cu.BLTH`）。
3. **第三层（新，也是最要命的）**：上面这次失败让 bluetoothd 把
   `bluetoothExternalDongleFailed` 写成 `%01`（字符串在 `/usr/sbin/bluetoothd` 里：
   `IORegistryEntrySetCFProperty() failed for bluetoothExternalDongleFailed`）。
   **从此每一次** bluetoothd 启动都直接跳过 dongle，走
   `External Bluetooth dongle was not found -- setting chipset to default BCM_4350C2` →
   `HCI Transport is set to H4BC` → `UART open(/dev/cu.BLTH) port failed to appear after 15 seconds`
   → `CBCrash` → 每 16 秒一轮。08:07–08:11 之间 `/Library/Logs/DiagnosticReports/` 里
   积了 **16 个** bluetoothd 崩溃报告。

所以"蓝牙崩"在后半段根本不是 MediaTek 的问题，是 macOS 自己的兜底路径 + 一个自我强化的标志位。
修法（三条 NVRAM，工具 `tools/bt-nvram-shape.py`，写进两份 EFI）：
`bluetoothExternalDongleFailed=%00`、`bluetoothHostControllerSwitchBehavior=always`、
`bluetoothInternalControllerInfo` 末字节改成与他人不同的 `…%14`。

实测结果：控制器 `State: On` / `Chipset: THIRD_PARTY_DONGLE` / 地址非 0，bluetoothd
`runs = 1` 且不再退出，12 秒收到 46 条 LE 广播（`tools/btle-scan.swift`）。

**本文档 §1–§6 的结论仍然有效且必要**（它解释了"为什么必须让栈别走 BCM 那条路"，
以及 `mtkbtcsr` 为什么是前提），只是单靠 14 字节注入不足以让 dongle 活下来。
完整证据见 `round67-bt-dongle-shape-and-awdl-publish.md`。

```sh
# 0) 崩溃集合与两种形状（逐份解析 .ips 的 faultingThread 帧；下面命令里的 python 脚本见 /tmp，可重写）
#    期望：38 份含 bluetoothd 帧 0x10b9be/0x164c97/0x156e0d/0x155b4f/0xcc549/0xd25e0（bcm_vsc 断言）
#          16 份含 bluetoothd 帧 0x72d8a0/0xcc4e1 + libdispatch（栈看门狗 abort）
/usr/bin/python3 - <<'PY'
import json,glob
for p in sorted(glob.glob("/Library/Logs/DiagnosticReports/bluetoothd-*.ips")):
    raw=open(p,errors='replace').read(); i=raw.index('\n'); b=json.loads(raw[i+1:])
    th=b['threads'][b['faultingThread']]; imgs=b['usedImages']
    print(p.split('/')[-1], [(imgs[f['imageIndex']]['name'], hex(f['imageOffset'])) for f in th['frames'][:8]])
PY

# 1) 反汇编（llvm-objdump 的 start/stop 对 Mach-O 不生效，全量 dump 后按地址切片最稳）
TMPD=$(mktemp -d)
/usr/bin/objdump -d --arch=x86_64 /usr/sbin/bluetoothd > "$TMPD/bt.dis"     # ~98 MB / 208 万行

# 2) 函数边界 + 名字
#    a. LC_FUNCTION_STARTS（cmd 0x26）→ ULEB128 增量 → 本机 33362 个函数起点
#    b. 收集所有 "void/OI_STATUS/…" 形态的 funcname 字符串 → 扫 __text 的 RIP 相对 lea → 按目标归组
#    c. 用 b 给 a 的边界贴名（本机 747 个函数拿到名字）

# 3) 反查字符串引用（示例：断言报告例程的唯一引用）
/usr/bin/python3 /tmp/findcaller.py 0x100878c72        # → ['0x10010b99d']

# 4) 运行时证据（只读）
printf '%s\n' "$PASS" | sudo -S log show --last 90m --style compact \
  --predicate 'process == "bluetoothd" OR process == "BlueTool"' > /tmp/btlog.txt
grep -n -E "AssertFail|Internal controller info missing|setting chipset|Use internal|Found USB Device" /tmp/btlog.txt
kmutil showloaded --list-only | grep -i -E "mtk|Bluetooth|lilu"     # 两个 kext + Lilu 都已加载
```

本次写在 `/tmp` 的辅助脚本/中间件（可重跑）：`fstarts.pkl`（函数起点）、`names.pkl`（函数名）、`fnames.py`/`name.py`（建名）、`win.py`（按地址切反汇编）、`findcaller.py`（字符串 xref）、`btlog.txt`（90 分钟日志）。

---

## 7. 结论可靠性分级

| 结论 | 等级 |
| --- | --- |
| 6 个 offset 的归属；`abort()` 的调用者 = `0x10010B980` 断言例程 | **实测**（唯一 xref 交叉验证） |
| 崩溃点 = `_BCM_ReadVerboseConfigVersionInfoParser` 的 bcm_vsc.c:1125，条件 `>= 1`，判定依据是 `__BufLen - __ReadPos == 0` | **实测**（反汇编 + 日志逐字一致，`0x465 = 1125`） |
| 该断言等价于"这条 VSC 应答载荷为空" | **实测**（代码语义；旁证：日志无 "…failed %!" ⇒ status == 0） |
| 本机 chipset 档 = BCM_4350C2，来源是 NVRAM 缺 Apple 私有控制器信息后的兜底 | **实测**（日志 + 反汇编同一函数） |
| 判型不依赖 controller 上报的 manufacturer / `Read Local Version Information` | **实测**（时间序：判档 `.211` 早于 USB 读 VID `.213`；且判定读的是 IORegistry） |
| "MTK 回了成功但零参数的 Command Complete" | **推断**（由"空载荷 + status==0"倒推；升格为实测需 HCI 抓包看那条 `0xFCxx` 请求/应答） |
| 那条命令的具体 `0xFCxx` 数值 | **看不出**（静态查不到；描述符运行时构造） |
| Patcher 的 hook 是否真的生效 | **看不出**（磁盘无目标镜像；KC fileset 压缩无法查证） |
| Patcher 与本次用户态断言无直接因果 | **推断** |
| `secd` 不是根因 | **实测**（同进程随后成功 commit keychain） |

---

## 8. 建议的下一步（都不属于本次只读范围）

1. **先抓一次 HCI 包**（PacketLogger）：确认那条 `0xFCxx` 的 opcode 与 Command Complete 长度 ⇒ 把 §3/§7 的"零载荷"升级为实测，并同时回答 §2.3 的 opcode 缺口。
2. **对比实验**：临时移除 `SimpleMtkBtPatcher`，看崩溃形状/频率是否变化（排除内核注入的干扰）。
3. **试候选 1**（纯 NVRAM，可回退）——风险最低、最可能改变整条决策链。
4. 若候选 1/2 都不改变 `setting chipset`，再考虑候选 4（传输层接管），避免过早投入重写。
## 5.5 已执行：候选 1 的精确版本（2026-10-13 19:54，主代理补记）

上面 §5 候选 1 说"14 字节布局没吃准、不保证"。**布局已经挖到确定**——从 bluetoothd 自己的
**写入端**（`0x1001a1ec6`）读出来的：

```
movq $0x0, (%rsi)          ; bytes[0..7]   = 0
movl (%r14), %eax
movl %eax, 0x8(%rsi)       ; bytes[8..11]  = *(uint32_t *)src
movzwl 0x4(%r14), %eax
movw %ax, 0xc(%rsi)        ; bytes[12..13] = *(uint16_t *)(src + 4)
movl $0xe, %edx            ; CFDataCreate(..., 14)
```

⇒ **14 字节 = 8 个零字节 + 控制器的 6 字节 BD 地址**。读取端（`0x1001a2473`）把后面 6 字节交给
一个"设置控制器地址"的函数 —— 也就是说这个变量同时治 `localAddress 0x0`。

**已注入**（工具：`outputs/20261010-mt7922-wifi/tools/add-bt-controller-info.py`，带备份 + 回读）：

| 介质 | 变量 | 值 | 语义 diff |
| --- | --- | --- | --- |
| ESP `/Volumes/OPENCORE` | `bluetoothInternalControllerInfo` | `0000000000000000` + `021a7dda7113`（14 字节） | 只多这一行 |
| U 盘 `/Volumes/1401` | 同上 | 同上 | 只多这一行 |

备份：两份 `config.plist.bak-btinfo-20261013-195400`；`plutil -lint` 通过。
回退：`python3 tools/add-bt-controller-info.py <config> --remove` 或 `sudo nvram -d bluetoothInternalControllerInfo`。

**注意**：控制流并不保证换到 THIRD_PARTY_DONGLE —— `0x1001a2518` 之后还要过 `bluetoothExternalDongleFailed`
与一次"external dongle found"虚调用；所以期望值是"档位决策与地址都变"，不是"必然不崩"。

## 6. 复现命令清单
