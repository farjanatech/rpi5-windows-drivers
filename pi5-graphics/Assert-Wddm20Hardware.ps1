# SPDX-License-Identifier: BSD-2-Clause-Patent
# Read-only deployment gate. Run before staging or activating this candidate.
[CmdletBinding()]
param([string]$InstanceId = 'ACPI\RPI1001\0')
$ErrorActionPreference = 'Stop'
$ids = @((Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_HardwareIds).Data)
if ($ids -contains 'ACPI\VEN_RPI&DEV_1001&REV_0000') {
    throw 'WDDM 2.0 activation blocked on Pi 5 C1: the working copy-scanout path does not implement the required FlipOnVSyncMmIo contract. Keep the verified WDDM 1.2 package installed. Do not enable the capability without fixing C1 direct scanout.'
}
if ($ids -notcontains 'ACPI\VEN_RPI&DEV_1001&REV_0001') {
    throw 'WDDM 2.0 activation blocked: the supported D0 hardware revision could not be verified.'
}
Write-Output 'D0 revision verified. This only passes the C1 compatibility gate; startup, rendering, display and recovery still require hardware validation.'
