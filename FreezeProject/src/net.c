/*
 * FreezeProject/src/net.c
 * Multi-NIC Ethernet driver
 *
 * Supported NICs:
 *   Intel e1000     0x8086:0x100E+  MMIO, full RX ring
 *   Realtek RTL8139 0x10EC:0x8139   I/O port, full RX ring
 *   VirtIO-net      0x1AF4:0x1000   I/O port, link + MAC
 *   AMD PCnet       0x1022:0x2000   I/O port, MAC read
 *
 * QEMU examples:
 *   -device e1000,netdev=n0          (default)
 *   -device rtl8139,netdev=n0
 *   -device virtio-net-pci,netdev=n0
 *   -device pcnet,netdev=n0
 *   combined with: -netdev user,id=n0
 */

#include "net.h"
#include "vga.h"
#include <stdint.h>

/* ── I/O port helpers ─────────────────────────────────────── */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0,%1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0,%1" :: "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0,%1" :: "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ── PCI config helpers ───────────────────────────────────── */

static uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func <<  8)
                  | (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val) {
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func <<  8)
                  | (off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

static void pci_enable_bus_master(uint8_t bus, uint8_t slot) {
    uint32_t cmd = pci_read(bus, slot, 0, 0x04);
    cmd |= (1 << 2) | (1 << 1);   /* bus master + memory space enable */
    pci_write(bus, slot, 0, 0x04, cmd);
}

/* ── active_net (exported) ────────────────────────────────── */

struct net_device *active_net = 0;

void net_poll(void) {
    if (active_net && active_net->poll)
        active_net->poll();
}

/* ════════════════════════════════════════════════════════════
 * 1.  Intel e1000 (82540EM and common variants)   0x8086:0x100E
 * ══════════════════════════════════════════════════════════ */

#define E1000_REG_CTRL   0x0000
#define E1000_REG_STATUS 0x0008
#define E1000_REG_RCTL   0x0100
#define E1000_REG_RDBAL  0x2800
#define E1000_REG_RDBAH  0x2804
#define E1000_REG_RDLEN  0x2808
#define E1000_REG_RDH    0x2810
#define E1000_REG_RDT    0x2818

#define E1000_NUM_RX_DESC    32
#define E1000_RX_BUF_SIZE  2048

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

static uint8_t  e1000_bus  = 0xFF;
static uint8_t  e1000_slot = 0xFF;
static volatile uint32_t *e1000_mmio = 0;

static struct e1000_rx_desc e1000_rx_descs[E1000_NUM_RX_DESC];
static uint8_t e1000_rx_bufs[E1000_NUM_RX_DESC][E1000_RX_BUF_SIZE];
static int e1000_rx_idx = 0;

static void e1000_wr(uint32_t reg, uint32_t val) { e1000_mmio[reg / 4] = val; }

static int e1000_link_up(void) {
    return (e1000_mmio[E1000_REG_STATUS / 4] & (1 << 1)) != 0;
}

static void e1000_poll(void) {
    struct e1000_rx_desc *d = &e1000_rx_descs[e1000_rx_idx];
    if (d->status & 0x1) {
        print("[e1000] pkt len=");
        print_int(d->length);
        print("\n");
        d->status = 0;
        e1000_wr(E1000_REG_RDT, (uint32_t)e1000_rx_idx);
        e1000_rx_idx = (e1000_rx_idx + 1) % E1000_NUM_RX_DESC;
    }
}

static void e1000_init(void) {
    print("[e1000] init...\n");
    pci_enable_bus_master(e1000_bus, e1000_slot);

    uint32_t bar0 = pci_read(e1000_bus, e1000_slot, 0, 0x10);
    e1000_mmio = (volatile uint32_t *)(bar0 & ~0xFu);

    e1000_wr(E1000_REG_CTRL, 1u << 26);          /* software reset */
    for (volatile int i = 0; i < 100000; i++);    /* wait */

    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        e1000_rx_descs[i].addr   = (uint64_t)(uint32_t)e1000_rx_bufs[i];
        e1000_rx_descs[i].status = 0;
    }
    e1000_wr(E1000_REG_RDBAL, (uint32_t)e1000_rx_descs);
    e1000_wr(E1000_REG_RDBAH, 0);
    e1000_wr(E1000_REG_RDLEN, E1000_NUM_RX_DESC * sizeof(struct e1000_rx_desc));
    e1000_wr(E1000_REG_RDH, 0);
    e1000_wr(E1000_REG_RDT, E1000_NUM_RX_DESC - 1);
    e1000_wr(E1000_REG_RCTL, (1 << 1) | (1 << 15)); /* EN | BSIZE 2K */

    print("[e1000] link: ");
    print(e1000_link_up() ? "up\n" : "down\n");
}

/* ════════════════════════════════════════════════════════════
 * 2.  Realtek RTL8139                              0x10EC:0x8139
 * ══════════════════════════════════════════════════════════ */

/* RTL8139 register offsets (I/O port based) */
#define RTL_IDR0    0x00  /* MAC address bytes 0-5 */
#define RTL_CR      0x37  /* Command Register */
#define RTL_RBSTART 0x30  /* RX Buffer Start Address */
#define RTL_CAPR    0x38  /* Current Address of Packet Read */
#define RTL_IMR     0x3C  /* Interrupt Mask Register */
#define RTL_ISR     0x3E  /* Interrupt Status Register */
#define RTL_RCR     0x44  /* Receive Configuration */
#define RTL_CONFIG1 0x52  /* Configuration Register 1 */
#define RTL_MSR     0x58  /* Media Status Register */

/* CR bits */
#define RTL_CR_RST  0x10  /* software reset (self-clearing) */
#define RTL_CR_TE   0x08  /* Transmit Enable */
#define RTL_CR_RE   0x04  /* Receive Enable */
#define RTL_CR_BUFE 0x01  /* Buffer Empty (read-only) */

/* RX buffer: 8 KiB ring + 16-byte header guard + 1500-byte overflow guard */
#define RTL_RX_BUF_SIZE 8192

static uint16_t rtl_iobase = 0;
static uint8_t  rtl_rx_buf[RTL_RX_BUF_SIZE + 16 + 1500];
static uint16_t rtl_rx_ptr = 0;   /* current read offset into rtl_rx_buf */

static int rtl8139_link_up(void) {
    /* MSR bit2 = 0 → link OK */
    return !(inb(rtl_iobase + RTL_MSR) & 0x04);
}

static void rtl8139_poll(void) {
    /* Loop while buffer is not empty */
    while (!(inb(rtl_iobase + RTL_CR) & RTL_CR_BUFE)) {
        uint8_t *hdr    = rtl_rx_buf + (rtl_rx_ptr & (RTL_RX_BUF_SIZE - 1));
        uint16_t status = *(uint16_t *)(hdr);
        uint16_t pkt_len = *(uint16_t *)(hdr + 2);

        if (status & 0x01) {  /* ROK */
            print("[rtl8139] pkt len=");
            print_int(pkt_len - 4);  /* strip 4-byte CRC */
            print("\n");
        }

        /* Advance ring pointer (dword-aligned, skip 4-byte header) */
        rtl_rx_ptr = (uint16_t)((rtl_rx_ptr + pkt_len + 4 + 3) & ~3u);
        rtl_rx_ptr %= RTL_RX_BUF_SIZE;

        /* Tell card how far we have read (CAPR = read_ptr - 0x10) */
        outw(rtl_iobase + RTL_CAPR, (uint16_t)(rtl_rx_ptr - 0x10));
    }
    outw(rtl_iobase + RTL_ISR, 0x0001);  /* clear ROK */
}

static void rtl8139_init(void) {
    print("[rtl8139] init...\n");

    outb(rtl_iobase + RTL_CONFIG1, 0x00);   /* power on */

    outb(rtl_iobase + RTL_CR, RTL_CR_RST);  /* software reset */
    while (inb(rtl_iobase + RTL_CR) & RTL_CR_RST);

    /* Print MAC */
    print("[rtl8139] MAC: ");
    for (int i = 0; i < 6; i++) {
        print_hex(inb(rtl_iobase + RTL_IDR0 + i));
        if (i < 5) print(":");
    }
    print("\n");

    /* Set RX buffer address */
    outl(rtl_iobase + RTL_RBSTART, (uint32_t)rtl_rx_buf);

    /* Accept all pkts, 8 KiB ring, wrap-around mode */
    outl(rtl_iobase + RTL_RCR, 0x0F | (1 << 7));  /* AAP|APM|AM|AB | WRAP */

    /* Enable RX + TX */
    outb(rtl_iobase + RTL_CR, RTL_CR_TE | RTL_CR_RE);

    /* Unmask receive-OK interrupt */
    outw(rtl_iobase + RTL_IMR, 0x0001);

    print("[rtl8139] link: ");
    print(rtl8139_link_up() ? "up\n" : "down\n");
}

/* ════════════════════════════════════════════════════════════
 * 3.  VirtIO-net  (legacy pre-1.0)                0x1AF4:0x1000
 * ══════════════════════════════════════════════════════════
 *
 * Legacy I/O layout (offset from BAR0):
 *   0x00  HostFeatures   (R  4 bytes)
 *   0x04  GuestFeatures  (W  4 bytes)
 *   0x08  QueueAddress   (W  4 bytes)
 *   0x0C  QueueSize      (R  2 bytes)
 *   0x0E  QueueSelect    (W  2 bytes)
 *   0x10  QueueNotify    (W  2 bytes)
 *   0x12  DeviceStatus   (RW 1 byte)
 *   0x13  ISR            (R  1 byte)
 *   0x14  DeviceConfig   (device-specific)
 *
 * Net device config (at 0x14):
 *   [0:5]  MAC  (6 bytes, when VIRTIO_NET_F_MAC offered)
 *   [6:7]  Status (bit0 = link up, when VIRTIO_NET_F_STATUS offered)
 */

#define VIRTIO_STATUS_ACK    1u
#define VIRTIO_STATUS_DRIVER 2u
#define VIRTIO_NET_F_MAC     (1u << 5)
#define VIRTIO_NET_F_STATUS  (1u << 16)

static uint16_t virtio_iobase = 0;

static int virtio_link_up(void) {
    uint32_t features = inl(virtio_iobase + 0x00);
    if (features & VIRTIO_NET_F_STATUS) {
        uint16_t st = inw(virtio_iobase + 0x14 + 6);
        return st & 1;
    }
    return 1;  /* assume up when status feature absent */
}

static void virtio_poll(void) {
    /* virtqueue TX/RX not yet implemented */
    (void)0;
}

static void virtio_init(void) {
    print("[virtio-net] init...\n");

    outb(virtio_iobase + 0x12, 0);                                     /* reset */
    outb(virtio_iobase + 0x12, VIRTIO_STATUS_ACK);                     /* acknowledge */
    outb(virtio_iobase + 0x12, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER); /* driver */

    uint32_t features = inl(virtio_iobase + 0x00);
    /* Accept only MAC + STATUS features for now */
    outl(virtio_iobase + 0x04, features & (VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS));

    if (features & VIRTIO_NET_F_MAC) {
        print("[virtio-net] MAC: ");
        for (int i = 0; i < 6; i++) {
            print_hex(inb(virtio_iobase + 0x14 + i));
            if (i < 5) print(":");
        }
        print("\n");
    }

    print("[virtio-net] link: ");
    print(virtio_link_up() ? "up\n" : "down\n");
}

/* ════════════════════════════════════════════════════════════
 * 4.  AMD PCnet  (AM79C970A / AM79C973)            0x1022:0x2000
 * ══════════════════════════════════════════════════════════
 *
 * WORD I/O mode (default after reset):
 *   iobase+0x00..0x0F  APROM  (contains MAC at bytes 0–5)
 *   iobase+0x10        RDP    (Register Data Port, 16-bit)
 *   iobase+0x12        RAP    (Register Address Port, 16-bit)
 *   iobase+0x14        RESET  (read to issue software reset)
 *   iobase+0x16        BDP    (BCR Data Port, 16-bit)
 *
 * BCR4 bit14 = LNKST (link status: 1 = link OK)
 * Full LANCE init block / TX-RX ring setup is deferred.
 */

static uint16_t pcnet_iobase = 0;

static int pcnet_link_up(void) {
    /* Select BCR4 via RAP, then read BDP */
    outw(pcnet_iobase + 0x12, 4);
    uint16_t bcr4 = inw(pcnet_iobase + 0x16);
    return (bcr4 >> 14) & 1;   /* LNKST */
}

static void pcnet_poll(void) {
    /* LANCE RX ring not yet implemented */
    (void)0;
}

static void pcnet_init(void) {
    print("[pcnet] init...\n");

    /* Software reset: read RESET register twice (WORD mode) */
    (void)inw(pcnet_iobase + 0x14);
    (void)inw(pcnet_iobase + 0x14);

    /* CSR0 STOP bit should now be set; switch device to WORD I/O */
    outw(pcnet_iobase + 0x12, 0);    /* RAP = 0  (CSR0) */
    outw(pcnet_iobase + 0x10, 0x04); /* write STOP to CSR0 (no-op; already set) */

    /* Read MAC from APROM */
    print("[pcnet] MAC: ");
    for (int i = 0; i < 6; i++) {
        print_hex(inb(pcnet_iobase + i));
        if (i < 5) print(":");
    }
    print("\n");

    print("[pcnet] full TX/RX pending\n");
}

/* ════════════════════════════════════════════════════════════
 * net_device descriptors
 * ══════════════════════════════════════════════════════════ */

static struct net_device e1000_dev  = { "e1000",      e1000_init,    e1000_link_up,    e1000_poll   };
static struct net_device rtl_dev    = { "rtl8139",    rtl8139_init,  rtl8139_link_up,  rtl8139_poll };
static struct net_device virtio_dev = { "virtio-net", virtio_init,   virtio_link_up,   virtio_poll  };
static struct net_device pcnet_dev  = { "pcnet",      pcnet_init,    pcnet_link_up,    pcnet_poll   };

/* ════════════════════════════════════════════════════════════
 * PCI scan + driver selection
 * ══════════════════════════════════════════════════════════ */

void net_scan_pci(void) {
    print("[PCI] Scanning...\n");

    int found_e1000  = 0;
    int found_rtl    = 0;
    int found_virtio = 0;
    int found_pcnet  = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {

            uint32_t id = pci_read((uint8_t)bus, slot, 0, 0);
            uint16_t vendor = id & 0xFFFF;
            if (vendor == 0xFFFF || vendor == 0x0000)
                continue;

            uint16_t device = id >> 16;

            print("[PCI] ");
            print_hex(vendor);
            print(":");
            print_hex(device);
            print(" @ ");
            print_hex(bus);
            print(":");
            print_hex(slot);
            print("\n");

            uint32_t cls = pci_read((uint8_t)bus, slot, 0, 0x08);
            if ((cls >> 24) == 0x02)
                print("      ^ network controller\n");

            /* ── Intel e1000 family ── */
            if (vendor == 0x8086) {
                switch (device) {
                case 0x100E: /* 82540EM (QEMU default)  */
                case 0x100F: /* 82545EM                 */
                case 0x1019: /* 82547EI                 */
                case 0x101A: /* 82547GI                 */
                case 0x10D3: /* 82574L                  */
                case 0x1533: /* I210                    */
                case 0x1F41: /* I354                    */
                    print("[NET] Intel e1000 (");
                    print_hex(device);
                    print(")\n");
                    e1000_bus  = (uint8_t)bus;
                    e1000_slot = slot;
                    found_e1000 = 1;
                    break;
                default:
                    break;
                }
            }

            /* ── Realtek RTL8139 ── */
            if (vendor == 0x10EC && device == 0x8139) {
                print("[NET] Realtek RTL8139\n");
                uint32_t bar0 = pci_read((uint8_t)bus, slot, 0, 0x10);
                rtl_iobase = (uint16_t)(bar0 & ~1u);
                pci_enable_bus_master((uint8_t)bus, slot);
                found_rtl = 1;
            }

            /* RTL8169/8168 detected but no driver yet */
            if (vendor == 0x10EC &&
                (device == 0x8169 || device == 0x8168 || device == 0x8167)) {
                print("[NET] Realtek RTL816x detected (use rtl8139 in QEMU for now)\n");
            }

            /* ── VirtIO-net (legacy 0x1000 or transitional 0x1041) ── */
            if (vendor == 0x1AF4 && (device == 0x1000 || device == 0x1041)) {
                uint32_t sub    = pci_read((uint8_t)bus, slot, 0, 0x2C);
                uint16_t subsys = (uint16_t)(sub >> 16);
                if (subsys == 1 || device == 0x1041) {
                    print("[NET] VirtIO-net\n");
                    uint32_t bar0 = pci_read((uint8_t)bus, slot, 0, 0x10);
                    virtio_iobase = (uint16_t)(bar0 & ~1u);
                    pci_enable_bus_master((uint8_t)bus, slot);
                    found_virtio = 1;
                }
            }

            /* ── AMD PCnet (AM79C970A / AM79C973) ── */
            if (vendor == 0x1022 && (device == 0x2000 || device == 0x2001)) {
                print("[NET] AMD PCnet\n");
                uint32_t bar0 = pci_read((uint8_t)bus, slot, 0, 0x10);
                pcnet_iobase = (uint16_t)(bar0 & ~1u);
                pci_enable_bus_master((uint8_t)bus, slot);
                found_pcnet = 1;
            }
        }
    }

    /*
     * Driver priority: virtio-net > e1000 > rtl8139 > pcnet
     * virtio-net is preferred in VM environments; e1000 is the
     * most fully implemented driver here.
     */
    if      (found_virtio) active_net = &virtio_dev;
    else if (found_e1000)  active_net = &e1000_dev;
    else if (found_rtl)    active_net = &rtl_dev;
    else if (found_pcnet)  active_net = &pcnet_dev;

    if (active_net) {
        active_net->init();
        print("[NET] Active driver: ");
        print(active_net->name);
        print("\n");
    } else {
        print("[NET] No supported NIC found\n");
    }
}
