#ifndef INTEL_CASCADELAKE_PARAMS_H
#define INTEL_CASCADELAKE_PARAMS_H

// MSR locations for IIO occupancy measurements
// IIO write occupancy
#define IRP_MSR_PMON_CTL_BASE 0x0A5BL
#define IRP_MSR_PMON_CTR_BASE 0x0A59L
#define IRP_OCC_VAL 0x0040010F
#define IIO_WR_COUNTER_OFFSET 0
// IIO read occupancy
#define IIO_MSR_PMON_CTL_BASE 0x0A48L
#define IIO_MSR_PMON_CTR_BASE 0x0A41L
#define IIO_OCC_VAL 0x00004000004001D5
#define IIO_RD_COUNTER_OFFSET 2

// MSR locations for PCIe bandwidth measurements
// Based on lspci analysis: Mellanox NIC on Bus 0x3a -> IIO Stack 2, Socket 0
#define IIO_PCIE_1_PORT_0_BW_IN \
	0x0B20 // IIO Stack 2, Port 0 ingress (Mellanox ConnectX-5)
#define IIO_PCIE_1_PORT_0_BW_OUT \
	0x0B24 // IIO Stack 2, Port 0 egress (Mellanox ConnectX-5)

// Other stacks for reference:
// IIO Stack 0: 0x0B00 (ingress), 0x0B04 (egress) - for chipset devices
// IIO Stack 1: 0x0B20 (ingress), 0x0B24 (egress) - for Broadcom devices
// IIO Stack 3: 0x0B60 (ingress), 0x0B64 (egress) - for additional devices

// MSR location for MBA
#define PQOS_MSR_MBA_MASK_START 0xD50L
#define PQOS_MSR_ASSOC 0xC8F

#endif
