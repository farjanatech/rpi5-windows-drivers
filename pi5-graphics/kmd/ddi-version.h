// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// All KMD translation units must use the same WDK structure layouts as the
// registered DriverEntry interface. These are DDI revisions, not WDDM labels.
#ifdef PI5_EXPERIMENTAL_WDDM20
#define DXGKDDI_INTERFACE_VERSION 0x5023
#else
#define DXGKDDI_INTERFACE_VERSION 0x300E
#endif
