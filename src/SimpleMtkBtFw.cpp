//
//  SimpleMtkBtFw.cpp
//  SimpleMtkBtFw
//
//  Created by laobamac on 2026/5/30.
//  Copyright © 2026 laobamac. All rights reserved.
//

#include "BtMtk.h"

#include "SimpleMtkBtFw.hpp"
#include <libkern/libkern.h>
#include <libkern/OSKextLib.h>
#include <libkern/version.h>
#include <libkern/OSTypes.h>
#include <IOKit/usb/StandardUSB.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <pexpert/pexpert.h>
#include "Hci.h"
#include "linux.h"
#include "Log.h"

#define super IOService
OSDefineMetaClassAndStructors(SimpleMtkBtFw, IOService)

enum { kMyOffPowerState = 0, kMyOnPowerState = 1 };

#define kIOPMPowerOff 0

/*
 * Bring-up gate, same shape as the Wi-Fi driver's (AirportMtk.cpp,
 * airportMtkAttachGate()): an NVRAM switch, with a boot-arg fallback.
 *
 * It exists because the firmware upload is the one step in this project that
 * cannot be undone inside the running system: once the new firmware is resident
 * in the chip, the only way back to the stock ROM state is a full power drain.
 * Being able to load the kext while leaving the device untouched is what makes
 * a controlled A/B possible - boot with the switch set, and observe the USB
 * port enumerate a stock device; boot without it, and observe whatever the
 * upload changes.
 *
 * Default is "run": an absent switch means the normal path, so a machine that
 * has never seen this key behaves exactly as upstream does. The switch is
 * `sudo nvram MtkBtSkip=1` to skip and `sudo nvram -d MtkBtSkip` to restore,
 * and OpenCore's NVRAM>Add does not carry either key, so a value set from macOS
 * survives a reboot (WriteFlash is true in this config.plist).
 */
#define MTK_BT_NVRAM_SKIP  "MtkBtSkip"
#define MTK_BT_BOOTARG_SKIP "mtkbt-skip"
#define MTK_BT_NVRAM_NOSHUTDOWN "MtkBtNoShutdown"
#define MTK_BT_BOOTARG_NOSHUTDOWN "mtkbt-noshutdown"

static bool
mtkOptionPresent(const char *nvramKey, const char *bootArg)
{
	IORegistryEntry *options =
	    IORegistryEntry::fromPath("/options", gIODTPlane);

	if (options != NULL) {
		OSObject *present = options->getProperty(nvramKey);

		options->release();
		if (present != NULL)
			return true;
	}

	int boot_value = 0;

	if (PE_parse_boot_argn(bootArg, &boot_value, sizeof(boot_value)))
		return true;

	return false;
}

static bool
mtkBtSkipRequested(void)
{
	return mtkOptionPresent(MTK_BT_NVRAM_SKIP, MTK_BT_BOOTARG_SKIP);
}

static bool
mtkBtShutdownDisableOff(void)
{
	return mtkOptionPresent(MTK_BT_NVRAM_NOSHUTDOWN,
	    MTK_BT_BOOTARG_NOSHUTDOWN);
}

static IOPMPowerState myTwoStates[2] =
{
    {1, kIOPMPowerOff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

bool SimpleMtkBtFw::init(OSDictionary *dictionary)
{
    XYLog("Driver init()\n");
    return super::init(dictionary);
}

void SimpleMtkBtFw::free()
{
    XYLog("Driver free()\n");
    super::free();
}

bool SimpleMtkBtFw::start(IOService *provider)
{
    XYLog("Driver Start()\n");
    /*
     * Leave a readable trace on every early exit.
     *
     * Why: on this machine the kext's own IOLog output reaches neither the
     * unified log nor dmesg, so "which step failed" was unreadable - the only
     * externally visible state was the properties this kext publishes, and the
     * gate / "not usb device" / super::start paths published nothing at all.
     * That left a device sitting in the registry as "Wireless_Device" with no
     * FirmwareLoaded attribute, which is indistinguishable from "the kext never
     * ran". A property named for the failing step removes the guesswork, and it
     * costs nothing at runtime.
     *
     * The locationID is included because the firmware upload ends in a chip
     * reset that re-enumerates the device: properties written to the old
     * instance disappear with it, and only the locationID tells the two
     * instances apart from outside.
     */
    auto noteStep = [this, provider](const char *tag) {
        OSNumber *loc = OSDynamicCast(OSNumber, provider->getProperty("locationID"));

        setProperty("MtkBtLastError", tag);
        setProperty("FirmwareLoaded", kOSBooleanFalse);
        if (loc != NULL)
            setProperty("MtkBtLocationID", loc);
        XYLog("start: step=%s locationID=0x%08x\n", tag,
            loc != NULL ? (unsigned)loc->unsigned32BitValue() : 0);
    };

    /*
     * Refuse before opening the device. The gate exists to leave the chip
     * exactly as the previous boot left it, so it has to be checked before the
     * first vendor request - and returning false here (the same shape as the
     * "not usb device" path below) leaves the device unclaimed for the A/B.
     */
    if (mtkBtSkipRequested()) {
        XYLog("Driver Start skipped (%s/%s): device left untouched\n",
            MTK_BT_NVRAM_SKIP, MTK_BT_BOOTARG_SKIP);
        noteStep("gate-skip");
        return false;
    }
    char fwName[64] {};
    m_pDevice = OSDynamicCast(IOUSBHostDevice, provider);
    if (m_pDevice == NULL) {
        XYLog("Driver Start fail, not usb device\n");
        noteStep("not-usb-device");
        return false;
    }
    PMinit();
    registerPowerDriver(this, myTwoStates, 2);
    provider->joinPMtree(this);
    makeUsable();

    if (!super::start(provider)) {
        noteStep("super-start-failed");
        return false;
    }

    if (!m_pDevice->open(this)) {
        XYLog("start fail, can not open device\n");
        noteStep("open-failed");
        publishReg(false, nullptr);
        cleanUp();
        stop(this);
        return false;
    }

    m_pBTMtk = new BtMtk();
    if (!m_pBTMtk || !m_pBTMtk->initWithDevice(this, m_pDevice)) {
        XYLog("start fail, can not init device\n");
        noteStep("init-failed");
        publishReg(false, nullptr);
        cleanUp();
        stop(this);
        return false;
    }

    XYLog("BT init succeed\n");
    {
        bool ok = m_pBTMtk->setup();

        /*
         * Publish what the vendor path learned, on success *and* failure.
         *
         * The chip id is read over endpoint 0 (vendor control transfers), so
         * recording it costs nothing and competes with nobody - whereas
         * reading the HCI event endpoint (0x81) would steal events from the
         * Bluetooth stack. On this machine IOLog does not reach the unified
         * log, so a registry property is the only way this value is readable
         * afterwards.
         */
        OSNumber *chip = OSNumber::withNumber(m_pBTMtk->devId(), 32);

        if (chip != NULL) {
            setProperty("MtkBtChipId", chip);
            if (m_pDevice != NULL)
                m_pDevice->setProperty("MtkBtChipId", chip);
            chip->release();
        }
        setProperty("MtkBtSetupOk", ok ? kOSBooleanTrue : kOSBooleanFalse);
        if (!ok) {
            XYLog("setup failed (chip id published for the record)\n");
            noteStep("setup-failed");
            publishReg(false, nullptr);
            cleanUp();
            stop(this);
            return false;
        }
    }
    m_bSetupDone = true;
    m_pBTMtk->getFirmwareName(fwName, sizeof(fwName));
    {
        /*
         * Give the controller an address.
         *
         * Everything upstream of this is confirmed working on this machine
         * (USB presentation with the three BT interfaces, firmware download,
         * BlueTool's boot and init scripts all report success) while the
         * adapter still refuses to power on with localAddress 0x0 - which is
         * the signature of a controller whose address reads back as all
         * zeroes. Linux covers exactly this case with btmtk_set_bdaddr()
         * (vendor HCI 0xfc1a), and upstream's own patcher for macOS carries a
         * "Sending Random Address" hook for the same reason.
         *
         * A fixed, locally administered address for now: it is stable across
         * boots, which is what matters for pairing. Reading the real address
         * out of EFUSE is the next refinement and does not change this call
         * site - only the bytes handed to it.
         */
        /*
         * ...and it must differ from the address macOS stores as the *internal*
         * controller's, or the dongle path is rejected before it starts.
         *
         * Measured 2026-10-14 (boot 08:58, and again 10:35 with the sampler in
         * place):
         *
         *   08:59:33.880 E  ERROR -- Third Party Dongle has the same address as
         *                   the internal module
         *   08:59:33.902    ...Third Party USB Bluetooth dongle failed
         *                   initialization. Switching to internal Bluetooth
         *                   module
         *
         * The comparison is between the dongle's BD_ADDR (what this kext writes
         * to the chip) and `bluetoothInternalControllerInfo` in NVRAM. Two
         * different values have been observed to sit in that NVRAM slot across
         * boots: `...:13`, the chip's own EFUSE address, which bluetoothd
         * writes back within the first ~9 s of a boot (the nvram sampler caught
         * it at t=+9 s on the 10:13/10:18/10:19 boots), and `...:14`, which
         * OpenCore's NVRAM/Add carries. Writing the same `...:13` therefore
         * collides under either timing, which is why this value was chosen to
         * differ from BOTH:
         *
         *   NVRAM = ...:13 (bluetoothd stored the EFUSE address)  -> 2a != 13  ok
         *   NVRAM = ...:14 (OpenCore's Add survived)              -> 2a != 14  ok
         *
         * Locally administered (0x02) and stable across boots, which is what
         * pairing needs. It also becomes the address macOS presents as this
         * machine's own, so a change here means re-pairing - nothing is paired
         * yet.
         */
        static const u8 kDefaultBdAddr[6] = { 0x02, 0x1a, 0x7d, 0xda, 0x71, 0x2a };
        /*
         * Read the controller's own address BEFORE touching it, and again
         * after. The pair is what decides the direction of the whole Bluetooth
         * effort, and nothing in this stack could produce it before:
         *
         *   read-back-after == what we wrote  -> the chip is fine and macOS is
         *       the one losing it; the fix is an Apple-side hook, i.e. exactly
         *       what SimpleMtkBtPatcher is for - and that patcher currently
         *       never fires because it hard-codes
         *       /System/Library/Extensions/IOBluetoothFamily.kext/Contents/MacOS/...
         *       which does not exist on macOS 15 (kexts live in the
         *       KernelCollection), so its fix is to use Lilu's KernelCollection
         *       patching instead of an on-disk MachInfo.
         *
         *   read-back-after == zeros -> the 0xfc1a write does not stick on this
         *       silicon, and no amount of hooking the Apple stack can help,
         *       because the address never existed in the controller.
         *
         * The read-back is also the only way to tell "the chip has no address
         * at all" from "the chip had one and we replaced it".
         */
        u8 addrBefore[6] = {};
        bool beforeOk = m_pBTMtk->readBdAddr(addrBefore);
        bool addrOk = m_pBTMtk->setBdAddr(kDefaultBdAddr);
        u8 addrAfter[6] = {};
        bool afterOk = m_pBTMtk->readBdAddr(addrAfter);
        /* The value actually handed to setBdAddr() above - kept in one place:
         * the property is what tells "did the chip take the write" from "did
         * the stack keep it". */
        OSString *addrStr = OSString::withCString("02:1a:7d:da:71:2a");

        setProperty("MtkBtBdAddrSet", addrOk ? kOSBooleanTrue : kOSBooleanFalse);
        /*
         * HCI reports the address least-significant octet first, so the
         * human-readable form is the byte array reversed. Printing it in raw
         * order would show a plausible-looking but wrong address - the same
         * mistake this project already made once when writing the address.
         */
        {
            char rb[24], ra[24];
            OSString *s;

            snprintf(rb, sizeof(rb), "%02x:%02x:%02x:%02x:%02x:%02x",
                     addrBefore[5], addrBefore[4], addrBefore[3],
                     addrBefore[2], addrBefore[1], addrBefore[0]);
            snprintf(ra, sizeof(ra), "%02x:%02x:%02x:%02x:%02x:%02x",
                     addrAfter[5], addrAfter[4], addrAfter[3],
                     addrAfter[2], addrAfter[1], addrAfter[0]);
            setProperty("MtkBtBdAddrBeforeOk", beforeOk ? kOSBooleanTrue : kOSBooleanFalse);
            setProperty("MtkBtBdAddrAfterOk", afterOk ? kOSBooleanTrue : kOSBooleanFalse);
            if ((s = OSString::withCString(rb)) != NULL) {
                setProperty("MtkBtBdAddrBefore", s);
                s->release();
            }
            if ((s = OSString::withCString(ra)) != NULL) {
                setProperty("MtkBtBdAddrAfter", s);
                s->release();
            }
            {
                OSNumber *n = OSNumber::withNumber((unsigned long long)m_pBTMtk->readBdAddrStatus(), 8);

                if (n != NULL) {
                    setProperty("MtkBtBdAddrReadStatus", n);
                    n->release();
                }
            }
            {
                /* Raw bytes of the last read-back reply, for the case where the
                 * shape check fails and the bytes are the only evidence. */
                const u8 *e = m_pBTMtk->readBdAddrEvent();
                u32 el = m_pBTMtk->readBdAddrEventLen();
                char hex[3 * 24 + 1];
                u32 i;

                for (i = 0; i < el && i < 24; i++) {
                    snprintf(hex + i * 3, 4, "%02x ", e[i]);
                }
                hex[el && el <= 24 ? el * 3 : 0] = '\0';
                if ((s = OSString::withCString(hex)) != NULL) {
                    setProperty("MtkBtBdAddrReadEvt", s);
                    s->release();
                }
            }
        }
        /* The controller's own status byte, or 0xff when the transfer itself
         * failed: the difference between "the chip refused the address" and
         * "we could not talk to it" is exactly what the next boot needs. */
        {
            OSNumber *st = OSNumber::withNumber(m_pBTMtk->bdAddrStatus(), 8);

            if (st != NULL) {
                setProperty("MtkBtBdAddrStatus", st);
                if (m_pDevice != NULL)
                    m_pDevice->setProperty("MtkBtBdAddrStatus", st);
                st->release();
            }
        }
        /*
         * Which half of the exchange failed, when the status byte is 0xff.
         * Observed once already: status 0xff with the kext otherwise green,
         * which by itself cannot distinguish "the control transfer was
         * refused" (interface/pipe) from "the chip never answered".
         */
        {
            OSNumber *rv;

            if ((rv = OSNumber::withNumber((unsigned long long)m_pBTMtk->bdAddrEventLen(), 32)) != NULL) {
                setProperty("MtkBtBdAddrEvtLen", rv);
                rv->release();
            }
            {
                const u8 *e = m_pBTMtk->bdAddrEvent();
                u32 el = m_pBTMtk->bdAddrEventLen();
                char hex[3 * 16 + 1];
                u32 i;

                for (i = 0; i < el && i < 16; i++)
                    snprintf(hex + i * 3, 4, "%02x ", e[i]);
                hex[el && el <= 16 ? el * 3 : 0] = '\0';
                {
                    OSString *s = OSString::withCString(hex);

                    if (s != NULL) {
                        setProperty("MtkBtBdAddrEvt", s);
                        s->release();
                    }
                }
            }
            if ((rv = OSNumber::withNumber((unsigned long long)m_pBTMtk->bdAddrSendRv(), 32)) != NULL) {
                setProperty("MtkBtBdAddrSendRv", rv);
                rv->release();
            }
            if ((rv = OSNumber::withNumber((unsigned long long)m_pBTMtk->bdAddrEventRv(), 32)) != NULL) {
                setProperty("MtkBtBdAddrEventRv", rv);
                rv->release();
            }
        }
        if (addrStr != NULL) {
            setProperty("MtkBtBdAddr", addrStr);
            if (m_pDevice != NULL)
                m_pDevice->setProperty("MtkBtBdAddr", addrStr);
            addrStr->release();
        }
    }
    publishReg(true, fwName);
    cleanUp();
    return true;
}

void SimpleMtkBtFw::publishReg(bool isSucceed, const char *fwName)
{
    /* "none" vs a step name: makes "it never got here" impossible to confuse
     * with "it got here and failed" when read from ioreg. */
    setProperty("MtkBtLastError", isSucceed ? "none" : "load-failed");
    m_pDevice->setProperty("FirmwareLoaded", isSucceed);
    if (isSucceed && fwName) {
        OSString *fwNameString = OSString::withCString(fwName);
        if (fwNameString) {
            setProperty("fw_name", fwNameString);
            fwNameString->release();
        }
    }
    /*
     * Which controller does the stack think this is?
     *
     * bluetoothd's bm3_usb walk (IOThreadFunc -> GetProductAndVendorID) accepts
     * exactly two shapes, and they lead to different init paths:
     *
     *   - "Bluetooth USB Host Controller" = the *internal* Apple controller. That
     *     is what this kext has claimed so far. The stack then wants Apple's
     *     private controller info, and without it falls back to chipset
     *     BCM_4350C2 - whose Broadcom VSC sequence this MediaTek chip answers with
     *     empty payloads, which aborts bluetoothd in bcm_vsc.c:1125.
     *   - a Broadcom (0x0A5C) or CSR (0x0A12) *external dongle*. Claiming one of
     *     those makes the walk report "Found external Bluetooth dongle", which is
     *     the third-party path: no Apple controller info required, and for CSR no
     *     vendor firmware loading either - i.e. plain HCI, which is the only
     *     thing this chip actually speaks.
     *
     * `mtkbtcsr` selects the second, default OFF. The mechanism is deliberately
     * the same one the internal claim already uses - an IORegistry property
     * override on the USB device node that bm3_usb reads - so the USB descriptor,
     * the hardware and this kext's own matching are all untouched, and the change
     * is undone by dropping one boot-arg.
     *
     * Whether it works is decided by the stack's own words in the log:
     * "Found external Bluetooth dongle -- setting chipset to THIRD_PARTY_DONGLE"
     * (success), versus staying on "...setting chipset to default BCM_4350C2".
     */
    {
        static int csr_ok = -1;
        int v = -1;

        if (csr_ok < 0)
            csr_ok = PE_parse_boot_argn("mtkbtcsr", &v, sizeof(v)) ? 1 : 0;
        if (csr_ok) {
            OSNumber *vid = OSNumber::withNumber((unsigned long long)0x0A12, 32);
            OSNumber *pid = OSNumber::withNumber((unsigned long long)0x0001, 32);
            OSString *name = OSString::withCString("CSR Bluetooth Dongle");

            if (vid != NULL) {
                m_pDevice->setProperty("idVendor", vid);
                vid->release();
            }
            if (pid != NULL) {
                m_pDevice->setProperty("idProduct", pid);
                pid->release();
            }
            if (name != NULL) {
                if (isSucceed)
                    m_pDevice->setProperty("USB Product Name", name);
                name->release();
            }
            setProperty("MtkBtShape", "csr-external");
            return;
        }
        setProperty("MtkBtShape", "internal-apple");
    }
    if (version_major >= 21) {
        m_pDevice->setName("Bluetooth USB Host Controller");
    }
    if (isSucceed) {
        /*
         * The registry entry name above is not what the daemon reads on
         * macOS 15. bluetoothd's bm3_usb walk (IOThreadFunc ->
         * GetProductAndVendorID) iterates every IOUSBHostDevice, keeps only
         * bDeviceClass 0xE0/0xEF/0xFF, and classifies the device as the
         * internal controller only when its "USB Product Name" property
         * CFStringCompare-equals "Bluetooth USB Host Controller". Left as
         * "Wireless_Device" the device is rejected and the daemon loops on
         * "Can't obtain vendorID and productID -- try again" every second
         * (observed in the system log on 24H32). Set the property too - the
         * USB descriptor and the matching by VID/PID are untouched.
         */
        OSString *controllerName =
            OSString::withCString("Bluetooth USB Host Controller");

        if (controllerName != NULL) {
            m_pDevice->setProperty("USB Product Name", controllerName);
            controllerName->release();
        }
    }
}

void SimpleMtkBtFw::cleanUp()
{
    XYLog("Clean up...\n");
    OSSafeReleaseNULL(m_pBTMtk);
    if (m_pDevice) {
        if (m_pDevice->isOpen(this)) {
            m_pDevice->close(this);
        }
        m_pDevice = NULL;
    }
}

IOReturn SimpleMtkBtFw::setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice)
{
    return IOPMAckImplied;
}

void SimpleMtkBtFw::stop(IOService *provider)
{
    XYLog("Driver Stop()\n");
    PMstop();
    super::stop(provider);
}

/*
 * Leave the chip disabled on the way down.
 *
 * This is the one step upstream never wired up: BtMtk::shutdown() has existed
 * all along (it is enableBluetoothProtocol(false), i.e. WMT FUNC_CTRL with the
 * function disabled) but nothing called it. Linux calls the equivalent from
 * hdev->shutdown -> btusb_mtk_shutdown() -> btmtk_usb_shutdown(), and
 * drivers/bluetooth/btusb.c explains why it matters on exactly this hardware -
 * the MTK quirk it applies to dev_id 0x7922/0x7925 when the machine is Ryzen
 * and the adapter is a direct child of the root hub:
 *
 *   "It is reported that remote wakeup events could sometimes cause some
 *    adapters completely unresponsive. Resetting the xHCI root hub doesn't
 *    help at all, and recovering from such a state needs a power cycle."
 *
 * That is a description of this machine's failure, symptom for symptom: after
 * the first boot that enabled the BT function (18:24), every later boot failed
 * at Address Device on XHCI port 16 - the adapter answering nothing - and a
 * warm reboot never helped.
 *
 * So the host now goes down with the function disabled instead of live. The
 * switch MtkBtNoShutdown/mtkbt-noshutdown turns this off for an A/B.
 */
void SimpleMtkBtFw::systemWillShutdown(IOOptionBits specifier)
{
    IOUSBHostDevice *device = m_pDevice;

    if (m_bSetupDone && device != NULL && !mtkBtShutdownDisableOff()) {
        if (device->open(this)) {
            BtMtk *bt = new BtMtk();

            if (bt != NULL) {
                if (bt->initWithDevice(this, device)) {
                    XYLog("shutdown: disabling BT function "
                        "(btmtk_usb_shutdown equivalent)\n");
                    bt->shutdown();
                } else {
                    XYLog("shutdown: cannot reach the device, "
                        "leaving it as it is\n");
                }
                bt->release();
            }
            device->close(this);
        } else {
            XYLog("shutdown: could not open the device\n");
        }
    }

    super::systemWillShutdown(specifier);
}

IOService *SimpleMtkBtFw::probe(IOService *provider, SInt32 *score)
{
    XYLog("Driver Probe()\n");
    if (!super::probe(provider, score)) {
        XYLog("super probe failed\n");
        return NULL;
    }
    m_pDevice = OSDynamicCast(IOUSBHostDevice, provider);
    if (!m_pDevice) {
        XYLog("is not usb device\n");
        return NULL;
    }
    UInt16 vendorID = USBToHost16(m_pDevice->getDeviceDescriptor()->idVendor);
    UInt16 productID = USBToHost16(m_pDevice->getDeviceDescriptor()->idProduct);
    XYLog("name=%s, class=%s, vendorID=0x%04X, productID=0x%04X\n",
          m_pDevice->getName(), provider->metaClass->getClassName(), vendorID, productID);
    m_pDevice = NULL;
    return this;
}
