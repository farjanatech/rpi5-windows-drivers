// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#if defined(PI5_C1_SCANOUT_PROBE) && defined(PI5_EXPERIMENTAL_WDDM20)
#error The C1 scanout probe must not enable WDDM 2.0.
#endif
// All KMD translation units must use the same WDK structure layouts as the
// registered DriverEntry interface. These are DDI revisions, not WDDM labels.
#ifdef PI5_EXPERIMENTAL_WDDM20
#define DXGKDDI_INTERFACE_VERSION 0x5023
#else
#define DXGKDDI_INTERFACE_VERSION 0x300E
#endif
