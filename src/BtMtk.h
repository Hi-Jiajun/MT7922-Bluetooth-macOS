//
//  BtMtk.h
//  SimpleMtkBtFw
//
//  Created by laobamac on 2026/5/30.
//  Copyright © 2026 laobamac. All rights reserved.
//

#ifndef BtMtk_h
#define BtMtk_h

#include <libkern/OSByteOrder.h>
#include <libkern/c++/OSObject.h>
#include <libkern/libkern.h>

#include "USBDeviceController.hpp"
#include "Hci.h"
#include "linux.h"

#define BTMTK_CMD_BUF_SIZE 272
#define BTMTK_WMT_EVENT_SIZE 64

#define BTMTK_HCI_WMT_OPCODE 0xfc6f
#define HCI_EV_WMT 0xe4

#define FIRMWARE_MT7922 "BT_RAM_CODE_MT7922_1_1_hdr.bin"
#define FIRMWARE_MT7961 "BT_RAM_CODE_MT7961_1_2_hdr.bin"
#define FIRMWARE_MT7961_FLAVOR "BT_RAM_CODE_MT7961_1a_2_hdr.bin"

#define MTK_FW_ROM_PATCH_HEADER_SIZE 32
#define MTK_FW_ROM_PATCH_GD_SIZE 64
#define MTK_FW_ROM_PATCH_SEC_MAP_SIZE 64
#define MTK_SEC_MAP_COMMON_SIZE 12
#define MTK_SEC_MAP_NEED_SEND_SIZE 52

#define BTMTK_WMT_REG_WRITE 0x1
#define BTMTK_WMT_REG_READ 0x2

#define MT7921_DLSTATUS 0x7c053c10
#define BT_DL_STATE BIT(1)

#define MTK_BT_MISC 0x70002510
#define MTK_BT_SUBSYS_RST 0x70002610
#define MTK_UDMA_INT_STA_BT 0x74000024
#define MTK_UDMA_INT_STA_BT1 0x74000308
#define MTK_BT_WDT_STATUS 0x740003A0
#define MTK_EP_RST_OPT 0x74011890
#define MTK_EP_RST_IN_OUT_OPT 0x00010001
#define MTK_BT_RST_DONE 0x00000100
#define MTK_BT_RESET_REG_CONNV3 0x70028610
#define MTK_BT_READ_DEV_ID 0x70010200

enum {
    BTMTK_WMT_PATCH_DWNLD = 0x1,
    BTMTK_WMT_TEST = 0x2,
    BTMTK_WMT_WAKEUP = 0x3,
    BTMTK_WMT_HIF = 0x4,
    BTMTK_WMT_FUNC_CTRL = 0x6,
    BTMTK_WMT_RST = 0x7,
    BTMTK_WMT_REGISTER = 0x8,
    BTMTK_WMT_SEMAPHORE = 0x17,
};

enum {
    BTMTK_WMT_INVALID,
    BTMTK_WMT_PATCH_UNDONE,
    BTMTK_WMT_PATCH_PROGRESS,
    BTMTK_WMT_PATCH_DONE,
    BTMTK_WMT_ON_UNDONE,
    BTMTK_WMT_ON_DONE,
    BTMTK_WMT_ON_PROGRESS,
};

struct btmtk_wmt_hdr {
    u8 dir;
    u8 op;
    __le16 dlen;
    u8 flag;
} __attribute__((packed));

struct btmtk_hci_wmt_evt {
    HciEventHdr hhdr;
    struct btmtk_wmt_hdr whdr;
} __attribute__((packed));

struct btmtk_hci_wmt_evt_funcc {
    struct btmtk_hci_wmt_evt hwhdr;
    __be16 status;
} __attribute__((packed));

struct btmtk_patch_header {
    u8 datetime[16];
    u8 platform[4];
    __le16 hwver;
    __le16 swver;
    __le32 magicnum;
} __attribute__((packed));

struct btmtk_global_desc {
    __le32 patch_ver;
    __le32 sub_sys;
    __le32 feature_opt;
    __le32 section_num;
} __attribute__((packed));

struct btmtk_section_map {
    __le32 sectype;
    __le32 secoffset;
    __le32 secsize;
    union {
        __le32 u4SecSpec[13];
        struct {
            __le32 dlAddr;
            __le32 dlsize;
            __le32 seckeyidx;
            __le32 alignlen;
            __le32 sectype;
            __le32 dlmodecrctype;
            __le32 crc;
            __le32 reserved[6];
        } bin_info_spec;
    };
} __attribute__((packed));

struct btmtk_hci_wmt_params {
    u8 op;
    u8 flag;
    u16 dlen;
    const void *data;
    u32 *status;
};

class BtMtk : public OSObject {
    OSDeclareDefaultStructors(BtMtk)

public:
    virtual bool initWithDevice(IOService *client, IOUSBHostDevice *dev);

    /* The chip id read over the vendor control path. Published as a registry
     * property by the caller so the value is readable from outside without
     * depending on IOLog (which does not reach the unified log on this
     * machine) - and without touching the HCI event endpoint, which the
     * Bluetooth stack owns. */
    u32 devId() const { return m_devId; }

    /* Program the controller's Bluetooth device address.
     *
     * Linux btmtk_set_bdaddr() sends vendor HCI command 0xfc1a with the
     * 6-byte address as payload (drivers/bluetooth/btmtk.c). This is the
     * step whose absence leaves the controller reporting 00:00:00:00:00:00,
     * which is what makes macOS keep the adapter powered off
     * (localAddress 0x0 / State: Off).
     *
     * Timing matters: this must run during bring-up, before bluetoothd opens
     * the device, so the HCI command/event endpoints are ours alone. */
    bool setBdAddr(const u8 *addr6);

    /* Status byte the controller returned for the last setBdAddr() attempt,
     * 0xff when the command or the event transfer itself failed. Published as
     * a property by the caller: on this machine IOLog does not reach the
     * unified log, so a failure with no readable reason is the default outcome
     * unless the value is exported. */
    u8 bdAddrStatus() const { return m_bdAddrStatus; }

    /* The two IOReturn values that lead to the 0xff sentinel: which side of
     * the exchange failed is the whole question when status reads 0xff - "the
     * control transfer to the chip was refused" and "the chip never answered"
     * are different repairs (interface/pipe vs chip/firmware state). */
    IOReturn bdAddrSendRv() const { return m_bdAddrSendRv; }
    IOReturn bdAddrEventRv() const { return m_bdAddrEventRv; }

    /* Raw bytes of whatever the controller answered with, and how many. The
     * first attempt returned a successful transfer whose payload failed the
     * Command-Complete shape check, which cannot be told apart from "a
     * different event arrived" without the bytes themselves. */
    const u8 *bdAddrEvent() const { return m_bdAddrEvt; }
    u32 bdAddrEventLen() const { return m_bdAddrEvtLen; }

    /*
     * Read the address back out of the controller - HCI Read_BD_ADDR (0x1009).
     *
     * This is the measurement that decides the whole Bluetooth direction, and
     * nothing in the stack could produce it before:
     *
     *   the 0xfc1a vendor command is accepted (status 0x00, confirmed),
     *   but macOS still reports localAddress 0x0 / Vendor 0 / State Off.
     *
     * Two readings of that are possible and they need opposite repairs:
     *
     *   (a) read-back after the write returns the address we sent -> the chip is
     *       fine and macOS is losing it, i.e. the Apple-side hook is required
     *       (this is what SimpleMtkBtPatcher exists for, and it currently never
     *       fires because it hard-codes an on-disk path that macOS 15 does not
     *       have);
     *   (b) read-back still returns all zeros -> the write does not stick on
     *       this silicon, and no amount of hooking the Apple stack can help
     *       because the address never existed in the controller.
     *
     * Reading it both before and after the write separates "the chip has no
     * address at all" from "the chip had one and we replaced it".
     */
    bool readBdAddr(u8 out[6]);

    u8 readBdAddrStatus() const { return m_readBdAddrStatus; }
    IOReturn readBdAddrSendRv() const { return m_readBdAddrSendRv; }
    IOReturn readBdAddrEventRv() const { return m_readBdAddrEventRv; }
    const u8 *readBdAddrEvent() const { return m_readBdAddrEvt; }
    u32 readBdAddrEventLen() const { return m_readBdAddrEvtLen; }
    virtual void free() override;

    bool setup();
    bool shutdown();
    bool getFirmwareName(char *fwname, size_t len);

private:
    bool setupUsb();
    bool setupFirmware79xx(const char *fwname, u32 devId);
    bool hciWmtSync(btmtk_hci_wmt_params *params);
    bool buildFirmwareName(char *fwname, size_t len, u32 devId, u32 fwVersion, u32 fwFlavor);

    bool readDeviceId(u32 reg, u32 *value);
    bool regRead(u32 reg, u32 *value);
    bool uhwRegRead(u32 reg, u32 *value);
    bool uhwRegWrite(u32 reg, u32 value);
    bool subsystemReset(u32 devId);
    bool resetDone();
    bool enableBluetoothProtocol(bool enable);

    OSData *firmwareConvertion(OSData *originalFirmware);
    OSData *requestFirmwareData(const char *fwName, bool noWarn = false);

private:
    USBDeviceController *m_pUSBDeviceController {nullptr};
    u32 m_devId {0};
    u8 m_bdAddrStatus {0xff};
    IOReturn m_bdAddrSendRv {kIOReturnNotReady};
    IOReturn m_bdAddrEventRv {kIOReturnNotReady};
    u8 m_bdAddrEvt[16] {};
    u32 m_bdAddrEvtLen {0};
    u8 m_readBdAddrStatus {0xff};
    IOReturn m_readBdAddrSendRv {kIOReturnNotReady};
    IOReturn m_readBdAddrEventRv {kIOReturnNotReady};
    u8 m_readBdAddrEvt[24] {};
    u32 m_readBdAddrEvtLen {0};
    u32 m_fwVersion {0};
    u32 m_fwFlavor {0};
    char m_loadedFirmwareName[64] {};
};

#endif /* BtMtk_h */
