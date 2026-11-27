// Device & board unit tests for HvWinVirt (GICv3, PL031 RTC, PL011 UART,
// PL061 GPIO, ACPI GED, fw_cfg + ramfb, PCIe GPEX, VirtIO-Net, NVMe, and JIT guest IRQs).
#include "harness.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sysreg.h"
#include "winvirt.h"

DECLARE_PROGRAM(p_timer_irq);
DECLARE_PROGRAM(p_uart);
extern const char devices_programs_begin[], devices_programs_finish[];

#define RAM_SIZE (64ull << 20)

static HvMachine m;
static HvCpu cpu;
static HvWinVirt board;
static int checks, failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: %llx, expected %llx\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static uint32_t rd32(uint64_t pa) {
    uint32_t v = 0;
    hv_phys_read(&m, pa, &v, 4);
    return v;
}
static uint8_t rd8(uint64_t pa) {
    uint8_t v = 0;
    hv_phys_read(&m, pa, &v, 1);
    return v;
}
static void wr32(uint64_t pa, uint32_t v) { hv_phys_write(&m, pa, &v, 4); }
static void wr16(uint64_t pa, uint16_t v) { hv_phys_write(&m, pa, &v, 2); }
static void wr8(uint64_t pa, uint8_t v) { hv_phys_write(&m, pa, &v, 1); }
static void wr64(uint64_t pa, uint64_t v) { hv_phys_write(&m, pa, &v, 8); }
static uint8_t *ram(uint64_t pa) { return hv_phys_ptr(&m, pa, 1); }

#define ICC(op0, op1, crn, crm, op2) ((op0) << 14 | (op1) << 11 | (crn) << 7 | (crm) << 3 | (op2))
#define ICC_IAR1    ICC(3, 0, 12, 12, 0)
#define ICC_EOIR1   ICC(3, 0, 12, 12, 1)
#define ICC_PMR     ICC(3, 0, 4, 6, 0)
#define ICC_IGRPEN1 ICC(3, 0, 12, 12, 7)
#define ICC_SGI1R   ICC(3, 0, 12, 11, 5)

static void test_gic(void) {
    HvGic *g = board.gic;
    check("GICD_TYPER ITLines", rd32(WINVIRT_GIC_DIST_PA + 4) & 0x1f, HV_GIC_NUM_IRQS / 32 - 1);
    check("GICD_PIDR2 arch rev", (rd32(WINVIRT_GIC_DIST_PA + 0xffe8) >> 4) & 0xf, 3);
    check("GICR_TYPER Last", (rd32(WINVIRT_GIC_REDIST_PA + 8) >> 4) & 1, 1);
    check("GICR_WAKER asleep at reset", rd32(WINVIRT_GIC_REDIST_PA + 0x14), 6);
    wr32(WINVIRT_GIC_DIST_PA, 3);
    wr32(WINVIRT_GIC_REDIST_PA + 0x14, 0);
    check("GICR_WAKER awake", rd32(WINVIRT_GIC_REDIST_PA + 0x14), 0);
    m.icc_write(&m, &cpu, ICC_PMR, 0xff);
    m.icc_write(&m, &cpu, ICC_IGRPEN1, 1);

    /* SPI 33 (UART), level, group 1, priority 0x80 */
    wr32(WINVIRT_GIC_DIST_PA + 0x84, 1u << 1);
    wr32(WINVIRT_GIC_DIST_PA + 0x104, 1u << 1);
    wr8(WINVIRT_GIC_DIST_PA + 0x400 + 33, 0x80);
    check("ISENABLER1 reads back", rd32(WINVIRT_GIC_DIST_PA + 0x104) & 2, 2);
    hv_gicv3_set_irq(g, 33, 1);
    check("level SPI raises IRQ", m.percpu[0].irq_line, 1);
    check("ISPENDR1 shows it", (rd32(WINVIRT_GIC_DIST_PA + 0x204) >> 1) & 1, 1);
    check("IAR1 = 33", m.icc_read(&m, &cpu, ICC_IAR1), 33);
    check("active: line drops", m.percpu[0].irq_line, 0);
    check("nothing else pending", m.icc_read(&m, &cpu, ICC_IAR1), 1023);
    m.icc_write(&m, &cpu, ICC_EOIR1, 33);
    check("still-high level re-pends after EOI", m.percpu[0].irq_line, 1);
    hv_gicv3_set_irq(g, 33, 0);
    check("level low: no IRQ", m.percpu[0].irq_line, 0);

    /* SGI 5 to self */
    wr32(WINVIRT_GIC_REDIST_PA + 0x10000 + 0x80, 1u << 5);
    wr32(WINVIRT_GIC_REDIST_PA + 0x10000 + 0x100, 1u << 5);
    m.icc_write(&m, &cpu, ICC_SGI1R, 5ull << 24 | 1);
    check("SGI to self", m.icc_read(&m, &cpu, ICC_IAR1), 5);
    m.icc_write(&m, &cpu, ICC_EOIR1, 5);
    wr32(WINVIRT_GIC_DIST_PA + 0x184, 6);
    check("ICENABLER1 clears", rd32(WINVIRT_GIC_DIST_PA + 0x104) & 6, 0);
}

static void test_peripherals(void) {
    /* PL031 RTC */
    uint32_t t = rd32(WINVIRT_RTC_PA);
    check("RTC within 2 s of host time", (uint64_t)((int64_t)t - (int64_t)time(NULL) + 2) <= 4, 1);
    wr32(WINVIRT_RTC_PA + 8, 1000000);
    check("RTC load", rd32(WINVIRT_RTC_PA) - 1000000 <= 2, 1);
    check("RTC PeriphID0", rd32(WINVIRT_RTC_PA + 0xfe0), 0x31);
    wr32(WINVIRT_RTC_PA + 0x10, 1);
    wr32(WINVIRT_RTC_PA + 4, rd32(WINVIRT_RTC_PA));
    check("RTC alarm on INTID_RTC", board.gic->level[WINVIRT_INTID_RTC], 1);
    wr32(WINVIRT_RTC_PA + 0x1c, 1);
    check("RTC alarm cleared", board.gic->level[WINVIRT_INTID_RTC], 0);

    /* PL011 UART RX */
    wr32(WINVIRT_UART_PA + 0x38, 1u << 4);
    check("UART RX empty", (rd32(WINVIRT_UART_PA + 0x18) >> 4) & 1, 1);
    hv_pl011_receive(board.uart, 'Z');
    check("UART RX line", board.gic->level[WINVIRT_INTID_UART], 1);
    check("UART DR", rd32(WINVIRT_UART_PA), 'Z');
    check("UART RX line drops", board.gic->level[WINVIRT_INTID_UART], 0);
    wr32(WINVIRT_UART_PA + 0x38, 0);

    /* PL061 GPIO */
    wr32(WINVIRT_GPIO_PA + 0x40c, 1u << 3);
    wr32(WINVIRT_GPIO_PA + 0x410, 1u << 3);
    check("GPIO PeriphID0", rd32(WINVIRT_GPIO_PA + 0xfe0), 0x61);
    hv_pl061_set_input(board.gpio, 3, 1);
    check("GPIO pin 3 reads high", rd32(WINVIRT_GPIO_PA + (1u << 3 << 2)), 1u << 3);
    check("GPIO edge raises INTID_GPIO", board.gic->level[WINVIRT_INTID_GPIO], 1);
    wr32(WINVIRT_GPIO_PA + 0x41c, 1u << 3);
    check("GPIO interrupt cleared", board.gic->level[WINVIRT_INTID_GPIO], 0);

    /* ACPI GED power button */
    hv_winvirt_power_button(&board);
    check("ACPI GED power button latches bit 1", rd32(WINVIRT_GED_PA), 0x2);
    check("ACPI GED read clears latch", rd32(WINVIRT_GED_PA), 0x0);

    /* fw_cfg signature + ramfb stride validation */
    wr16(WINVIRT_FW_CFG_PA + 8, __builtin_bswap16(0x0000));
    char sig[4] = {(char)rd8(WINVIRT_FW_CFG_PA), (char)rd8(WINVIRT_FW_CFG_PA),
                   (char)rd8(WINVIRT_FW_CFG_PA), (char)rd8(WINVIRT_FW_CFG_PA)};
    check("fw_cfg QEMU signature", memcmp(sig, "QEMU", 4), 0);

    /* Locate etc/ramfb selector in fw_cfg directory */
    int ramfb_sel = -1;
    for (int i = 0; i < board.fw_cfg->nfiles; i++) {
        if (!strcmp(board.fw_cfg->files[i].name, "etc/ramfb")) ramfb_sel = 0x0020 + i;
    }
    check("etc/ramfb registered in fw_cfg", ramfb_sel >= 0x0020, 1);
    if (ramfb_sel >= 0x0020) {
        uint64_t cfg_pa = WINVIRT_RAM_PA + 0x500000, dma_pa = WINVIRT_RAM_PA + 0x501000, fb_pa = WINVIRT_RAM_PA + 0x600000;
        uint8_t *c = ram(cfg_pa);
        uint64_t be_addr = __builtin_bswap64(fb_pa);
        uint32_t be_fourcc = __builtin_bswap32(0x34325258u); /* XR24 */
        uint32_t be_flags = 0;
        uint32_t be_w = __builtin_bswap32(128), be_h = __builtin_bswap32(64);
        uint32_t be_bad_stride = __builtin_bswap32(256); /* < width * 4: must be rejected */
        memcpy(c + 0, &be_addr, 8);
        memcpy(c + 8, &be_fourcc, 4);
        memcpy(c + 12, &be_flags, 4);
        memcpy(c + 16, &be_w, 4);
        memcpy(c + 20, &be_h, 4);
        memcpy(c + 24, &be_bad_stride, 4);

        uint32_t ctl_be = __builtin_bswap32(((uint32_t)ramfb_sel << 16) | 0x08u | 0x10u); /* SELECT | WRITE */
        uint32_t len_be = __builtin_bswap32(28);
        uint64_t adr_be = __builtin_bswap64(cfg_pa);
        memcpy(ram(dma_pa) + 0, &ctl_be, 4);
        memcpy(ram(dma_pa) + 4, &len_be, 4);
        memcpy(ram(dma_pa) + 8, &adr_be, 8);
        wr32(WINVIRT_FW_CFG_PA + 0x10, __builtin_bswap32((uint32_t)(dma_pa >> 32)));
        wr32(WINVIRT_FW_CFG_PA + 0x14, __builtin_bswap32((uint32_t)dma_pa));
        check("ramfb rejects stride < width * 4", board.fb->active, 0);

        uint32_t be_good_stride = __builtin_bswap32(512);
        memcpy(c + 24, &be_good_stride, 4);
        memcpy(ram(dma_pa) + 0, &ctl_be, 4);
        wr32(WINVIRT_FW_CFG_PA + 0x10, __builtin_bswap32((uint32_t)(dma_pa >> 32)));
        wr32(WINVIRT_FW_CFG_PA + 0x14, __builtin_bswap32((uint32_t)dma_pa));
        check("ramfb accepts 32-bpp stride", board.fb->active, 1);
    }
}

static uint64_t find_slot(uint16_t vendor, uint16_t device) {
    /* GPEX ECAM uses BDF: Dev << 15 on bus 0 */
    for (int s = 0; s < 32; s++) {
        uint32_t id = rd32(WINVIRT_PCIE_ECAM_PA + ((uint64_t)s << 15));
        if (id == ((uint32_t)device << 16 | vendor)) return (uint64_t)s;
    }
    return ~0ull;
}

static void put_desc(uint64_t table, int i, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next) {
    uint8_t *d = ram(table + 16u * i);
    memcpy(d, &addr, 8);
    memcpy(d + 8, &len, 4);
    memcpy(d + 12, &flags, 2);
    memcpy(d + 14, &next, 2);
}

static void offer(uint64_t ring, uint16_t head) {
    uint16_t *avail = (uint16_t *)ram(ring + 0x1000);
    avail[2 + avail[1] % 16] = head;
    avail[1]++;
}

static uint8_t sent[2048];
static uint32_t sent_len;
static void capture_tx(void *opaque, const uint8_t *frame, uint32_t len) {
    (void)opaque;
    memcpy(sent, frame, len < sizeof sent ? len : sizeof sent);
    sent_len = len;
}

static void test_pcie_and_virtio_net(void) {
    check("GPEX host bridge enumerated", find_slot(0x1b36, 0x0008), 0);
    check("NVMe controller enumerated", find_slot(0x1b36, 0x0010) < 32, 1);
    check("xHCI controller enumerated", find_slot(0x1b36, 0x000d) < 32, 1);
    check("Intel HDA controller enumerated", find_slot(0x8086, 0x2668) < 32, 1);

    int slot = (int)find_slot(0x1af4, 0x1000);
    if (slot < 0) slot = (int)find_slot(0x1af4, 0x1041);
    check("virtio-net enumerated", slot >= 0 && slot < 32, 1);
    if (slot < 0 || slot >= 32) return;

    uint64_t cfg = WINVIRT_PCIE_ECAM_PA + ((uint64_t)slot << 15);
    uint64_t bar = WINVIRT_PCIE_MMIO_PA + 0x200000;
    wr32(cfg + 0x20, (uint32_t)bar);
    wr32(cfg + 0x24, 0);
    wr16(cfg + 0x04, 0x6);
    wr8(bar + 0x14, 0);
    wr8(bar + 0x14, 1 | 2);
    wr32(bar + 0x08, 1);
    wr32(bar + 0x0c, 1);
    wr8(bar + 0x14, 1 | 2 | 8);
    uint64_t ring = WINVIRT_RAM_PA + 0x300000;
    for (int q = 0; q < 2; q++) {
        uint64_t qr = ring + (uint64_t)q * 0x4000;
        memset(ram(qr), 0, 0x4000);
        wr16(bar + 0x16, (uint16_t)q);
        wr16(bar + 0x18, 16);
        wr64(bar + 0x20, qr);
        wr64(bar + 0x28, qr + 0x1000);
        wr64(bar + 0x30, qr + 0x2000);
        wr16(bar + 0x1c, 1);
    }
    wr8(bar + 0x14, 1 | 2 | 8 | 4);
    check("net MAC in config", rd8(bar + 0x2000 + 5), 0x56);

    board.net->transmit = capture_tx;
    uint64_t rxq = ring, txq = ring + 0x4000, buf = WINVIRT_RAM_PA + 0x400000;
    memset(ram(buf), 0, 12);
    for (int i = 0; i < 60; i++) ram(buf + 12)[i] = (uint8_t)i;
    put_desc(txq, 0, buf, 72, 0, 0);
    offer(txq, 0);
    wr16(bar + 0x3000 + 4, 1);
    check("TX frame length", sent_len, 60);
    check("TX frame bytes", sent[59], 59);

    uint8_t frame[64];
    for (int i = 0; i < 64; i++) frame[i] = (uint8_t)(0xa0 + i);
    check("RX with no buffer drops", hv_virtio_net_receive(board.net, frame, 64), 0);
    put_desc(rxq, 0, buf + 0x1000, 2048, 2, 0);
    offer(rxq, 0);
    wr16(bar + 0x3000, 0);
    check("RX delivered", hv_virtio_net_receive(board.net, frame, 64), 1);
    check("RX bytes", memcmp(ram(buf + 0x1000 + 12), frame, 64), 0);
}

static char uart_out[128];
static int uart_n;
static void capture_uart(void *opaque, uint8_t c) {
    (void)opaque;
    if (uart_n < (int)sizeof uart_out - 1) uart_out[uart_n++] = (char)c;
}

static void test_guest_programs(void) {
    static HvCpu init;
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)devices_programs_begin, devices_programs_finish - devices_programs_begin,
                       (void *)devices_programs_begin, 1);
    static const TestProgram timer = TEST_PROGRAM(p_timer_irq), uart = TEST_PROGRAM(p_uart);

    HvGic *g = board.gic;
    memset(g->enabled, 0, sizeof g->enabled);
    memset(g->cpu[0].enabled, 0, sizeof g->cpu[0].enabled);
    g->ctlr = 0;
    g->cpu[0].igrpen1 = 0;
    g->cpu[0].waker = 6;
    memset(&init, 0, sizeof init);
    seed_cpu(&init, 3, WINVIRT_RAM_PA + 0x10000);
    uint64_t t0 = hv_counter();
    if (run_jit(&m, &timer, &init, &cpu, NULL) == 0) {
        check("timer IRQ INTID", cpu.x[20], 27);
        check("one IRQ", cpu.x[21], 1);
        check("waited about 1 ms (counter delta >= 24000)", cpu.x[11] - cpu.x[10] >= 24000, 1);
        check("not absurdly long (< 100 ms)", hv_counter() - t0 < 24000 * 100, 1);
        check("no stray exceptions", cpu.x[23], 0);
        check("RPR idle after EOI", cpu.x[12], 0xff);
    } else {
        failures++;
    }

    board.uart->output = capture_uart;
    memset(&init, 0, sizeof init);
    seed_cpu(&init, 4, WINVIRT_RAM_PA + 0x10000);
    if (run_jit(&m, &uart, &init, &cpu, NULL) == 0) {
        check("UART output", strcmp(uart_out, "hello from the guest\n"), 0);
        check("PL011 PeriphID0", cpu.x[4], 0x11);
    } else {
        failures++;
    }
}

static int make_temp_file(char *tmpl, size_t bytes, uint8_t fill) {
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)bytes) != 0) {
        close(fd);
        unlink(tmpl);
        return -1;
    }
    if (fill) {
        uint8_t buf[4096];
        memset(buf, fill, sizeof buf);
        (void)!pwrite(fd, buf, sizeof buf, 0);
    }
    close(fd);
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char code_path[] = "/tmp/winvm_test_code_XXXXXX";
    char vars_path[] = "/tmp/winvm_test_vars_XXXXXX";
    char disk_path[] = "/tmp/winvm_test_disk_XXXXXX";
    if (make_temp_file(code_path, 64u << 20, 0) != 0) return 1;
    if (make_temp_file(vars_path, 64u << 20, 0) != 0) { unlink(code_path); return 1; }
    if (make_temp_file(disk_path, 1u << 20, 0xa5) != 0) { unlink(code_path); unlink(vars_path); return 1; }

    int rc = 1;
    static HvDisk disk0 = {.fd = -1};
    if (hv_disk_open(&disk0, disk_path, 0, 0) != 0) goto out;
    if (hv_machine_init(&m, 8u << 20) != 0) goto out;

    static const uint8_t mac[6] = {2, 0, 0, 0x12, 0x34, 0x56};
    if (hv_winvirt_create(&board, &m, &cpu, 1, RAM_SIZE, NULL, code_path, vars_path, &disk0, NULL, mac, 0) != 0) {
        hv_machine_free(&m);
        goto out;
    }
    hv_winvirt_reset_cpu(&board);
    check("reset pc at pflash0", cpu.pc, WINVIRT_FLASH0_PA);
    check("x0 holds DTB address at RAM base", cpu.x[0], WINVIRT_RAM_PA);

    test_gic();
    test_peripherals();
    test_pcie_and_virtio_net();
    test_guest_programs();

    hv_winvirt_destroy(&board);
    hv_machine_free(&m);
    printf("%d checks, %d failures\n", checks, failures);
    rc = failures != 0;

out:
    if (disk0.fd >= 0) hv_disk_close(&disk0);
    unlink(code_path);
    unlink(vars_path);
    unlink(disk_path);
    return rc;
}
