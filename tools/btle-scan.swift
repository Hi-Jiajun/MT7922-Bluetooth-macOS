// btle-scan.swift - prove the Bluetooth radio actually receives.
//
// Why this exists: "State: On" from system_profiler only says the stack
// accepted the controller. It does not say a single advertisement ever reached
// the host, and on this machine every BT symptom so far has been a stack-state
// question rather than a radio question. An LE scan is the cheapest end-to-end
// receive test that does not need a second device put into pairing mode - any
// nearby phone, watch or earbud answers.
//
// Usage:  swiftc -O -o tools/btle-scan tools/btle-scan.swift
//         tools/btle-scan [seconds]
//
// Reading the output:
//   state: 0..4  -> the scan never started (5 = poweredOn). 3 = the calling
//                   process has no Bluetooth TCC permission, which is a
//                   permission problem, not a radio one.
//   found[n] ... -> the radio is receiving: address, RSSI and adv data arrived.

import CoreBluetooth
import Foundation

final class Scanner: NSObject, CBCentralManagerDelegate {
    var found = 0

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        print("state: \(central.state.rawValue) (\(central.state))")
        if central.state == .poweredOn {
            central.scanForPeripherals(withServices: nil, options: nil)
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        found += 1
        print("found[\(found)] \(peripheral.identifier) rssi=\(RSSI) name=\(peripheral.name ?? "-") adv=\(advertisementData)")
    }
}

let scanner = Scanner()
let manager = CBCentralManager(delegate: scanner, queue: nil)
let seconds = CommandLine.arguments.count > 1 ? (Double(CommandLine.arguments[1]) ?? 10.0) : 10.0
RunLoop.main.run(until: Date().addingTimeInterval(seconds))
print("total: \(scanner.found)")
