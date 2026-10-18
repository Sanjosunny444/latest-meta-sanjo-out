// ============================================================
// core3_loop.c — Phase 3 Step 4
// BMP280 bare-metal reader + ring buffer writer
// Runs as Linux process pinned to isolated Core 3:
//   taskset -c 3 ./core3_loop
//
// Access paths:
//   /dev/mem        → mmap BCM2837 BSC1 I2C MMIO (phys 0x3F804000)
//   /dev/bmp280     → mmap ring buffer (shared with kernel driver)
//   CNTVCT_EL0      → ARM 19.2MHz timer for 1ms busy-wait
//
// No kernel driver calls. I2C done via raw register writes.
// Struct layout must match bmp280_driver.c exactly (byte-for-byte).
// ============================================================

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sched.h>

// ============================================================
// BCM2837 BSC1 (I2C1) physical MMIO — 0x3F804000
// ============================================================

#define BCM2837_PERIPH_BASE  0x3F000000UL
#define BSC1_PHYS            (BCM2837_PERIPH_BASE + 0x804000UL)
#define MMAP_PAGE_SIZE       4096

// BSC register array indices (each uint32_t wide)
#define BSC_C     0   // Control
#define BSC_S     1   // Status
#define BSC_DLEN  2   // Data Length
#define BSC_A     3   // Slave Address
#define BSC_FIFO  4   // Data FIFO

// Control bits (BCM2835 datasheet p.29)
#define BSC_C_EN    (1u << 15)  // BSC enable
#define BSC_C_ST    (1u << 7)   // start transfer
#define BSC_C_CLEAR (3u << 4)   // clear FIFO (bits 4+5 both set)
#define BSC_C_READ  (1u << 0)   // 1=read, 0=write

// Status bits
#define BSC_S_CLKT  (1u << 9)   // clock stretch timeout (w1c)
#define BSC_S_ERR   (1u << 8)   // slave no-ack (w1c)
#define BSC_S_RXD   (1u << 5)   // FIFO has data (read)
#define BSC_S_TXD   (1u << 4)   // FIFO can accept data (write)
#define BSC_S_DONE  (1u << 1)   // transfer complete (w1c)

// ============================================================
// BMP280 register defines
// ============================================================

#define BMP280_ADDR        0x76
#define BMP280_REG_ID      0xD0
#define BMP280_CHIP_ID     0x58
#define BMP280_REG_CALIB   0x88   // 24 bytes: dig_T1..dig_P9
#define BMP280_REG_CTRL    0xF4
#define BMP280_CTRL_NORMAL 0x57   // osrs_t=001 osrs_p=101 mode=11
#define BMP280_REG_ADC     0xF7   // 6 bytes: press[19:0] + temp[19:0]

// ============================================================
// Shared memory layout — must match bmp280_driver.c exactly
// ============================================================

#define BOOTSTRAP_MAGIC    0xDEADBEEFCAFEBABEULL
#define RING_ENTRY_COUNT   1000
#define BOOTSTRAP_SIZE     72
#define BUF_SIZE           16384   // PAGE_ALIGN(BOOTSTRAP_SIZE + RING_SIZE)

// Matches struct bootstrap_cfg in bmp280_driver.c
// write_idx stored at reserved[0] — kernel driver zeros it at probe, Core 3 owns it
//
// Phase 5 — heartbeat health monitor. Each field below has exactly ONE writer,
// so there is no shared read-modify-write race anywhere in this struct:
//   heartbeat_counter — Core 3 writes (++ every 1ms loop iteration) → driver reads
//   core3_dead        — driver writes (hrtimer every 250ms)         → daemon reads
typedef struct {
    uint64_t magic;
    uint64_t ring_phys_addr;
    uint64_t ring_entry_count;
    uint64_t threshold_press;
    uint64_t threshold_temp;
    uint64_t flags;
    uint64_t write_idx;          // de-facto name for reserved[0]: Core 3 writes current idx
    uint64_t heartbeat_counter;  // Core 3 writes only — driver reads, never writes back
    uint64_t core3_dead;         // driver writes only — Core 3 never touches this
} __attribute__((packed)) bootstrap_cfg;

// Matches struct ring_entry in bmp280_driver.c
typedef struct {
    uint64_t timestamp;    // CNTVCT_EL0 ARM cycle counter
    uint32_t pressure;     // Pa
    int32_t  temp;         // 0.01 degC
} __attribute__((packed)) ring_entry;

// ============================================================
// BSC1 MMIO pointer (set in main after mmap)
// ============================================================

static volatile uint32_t *bsc;

// ============================================================
// ARM generic timer — 19.2 MHz on RPi3
// ============================================================

static inline uint64_t cntvct(void)
{
    uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline uint64_t cntfrq(void)
{
    uint64_t v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

// ============================================================
// BSC I2C low-level
// ============================================================

// Clear sticky status bits (w1c)
static void bsc_clear_status(void)
{
    bsc[BSC_S] = BSC_S_CLKT | BSC_S_ERR | BSC_S_DONE;
}

// Write len bytes to I2C slave addr
static int i2c_write(uint8_t addr, const uint8_t *data, int len)
{
    bsc[BSC_A]    = addr;
    bsc[BSC_DLEN] = (uint32_t)len;
    bsc[BSC_C]    = BSC_C_EN | BSC_C_CLEAR;
    bsc_clear_status();

    // Fill TX FIFO (max 16 bytes deep; BMP280 writes are ≤2 bytes)
    for (int i = 0; i < len && i < 16; i++)
        bsc[BSC_FIFO] = data[i];

    bsc[BSC_C] = BSC_C_EN | BSC_C_ST;  // start

    uint32_t timeout = 100000;
    while (!(bsc[BSC_S] & BSC_S_DONE) && --timeout) {
        if (bsc[BSC_S] & BSC_S_ERR) return -1;
    }
    return (bsc[BSC_S] & BSC_S_ERR || !timeout) ? -1 : 0;
}

// Read len bytes from register reg of I2C slave addr
// Two transactions: write reg addr, then read data
static int i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, int len)
{
    // Transaction 1: write register address
    if (i2c_write(addr, &reg, 1) < 0) return -1;

    // Transaction 2: read data
    bsc[BSC_A]    = addr;
    bsc[BSC_DLEN] = (uint32_t)len;
    bsc[BSC_C]    = BSC_C_EN | BSC_C_CLEAR;
    bsc_clear_status();
    bsc[BSC_C]    = BSC_C_EN | BSC_C_ST | BSC_C_READ;

    int got = 0;
    uint32_t timeout = 1000000;
    while (got < len && --timeout) {
        if (bsc[BSC_S] & BSC_S_ERR) return -1;
        // Drain FIFO as bytes arrive (handles len > 16 byte FIFO depth)
        while ((bsc[BSC_S] & BSC_S_RXD) && got < len)
            buf[got++] = (uint8_t)(bsc[BSC_FIFO] & 0xFF);
        if (bsc[BSC_S] & BSC_S_DONE) {
            // Drain any remaining bytes left in FIFO after DONE
            while ((bsc[BSC_S] & BSC_S_RXD) && got < len)
                buf[got++] = (uint8_t)(bsc[BSC_FIFO] & 0xFF);
            break;
        }
    }
    return (bsc[BSC_S] & BSC_S_ERR) ? -1 : got;
}

// ============================================================
// BMP280 calibration
// ============================================================

static uint16_t dig_T1;
static int16_t  dig_T2, dig_T3;
static uint16_t dig_P1;
static int16_t  dig_P2, dig_P3, dig_P4, dig_P5;
static int16_t  dig_P6, dig_P7, dig_P8, dig_P9;
static int32_t  t_fine;

static int bmp280_read_calib(void)
{
    uint8_t b[24];
    if (i2c_read(BMP280_ADDR, BMP280_REG_CALIB, b, 24) < 0) return -1;

    dig_T1 = (uint16_t)((b[1] << 8) | b[0]);
    dig_T2 = (int16_t) ((b[3] << 8) | b[2]);
    dig_T3 = (int16_t) ((b[5] << 8) | b[4]);
    dig_P1 = (uint16_t)((b[7] << 8) | b[6]);
    dig_P2 = (int16_t) ((b[9] << 8) | b[8]);
    dig_P3 = (int16_t) ((b[11] << 8) | b[10]);
    dig_P4 = (int16_t) ((b[13] << 8) | b[12]);
    dig_P5 = (int16_t) ((b[15] << 8) | b[14]);
    dig_P6 = (int16_t) ((b[17] << 8) | b[16]);
    dig_P7 = (int16_t) ((b[19] << 8) | b[18]);
    dig_P8 = (int16_t) ((b[21] << 8) | b[20]);
    dig_P9 = (int16_t) ((b[23] << 8) | b[22]);
    return 0;
}

// ============================================================
// BMP280 compensation — integer only, matches Phase 2 driver
// ============================================================

static int32_t bmp280_compensate_temp(int32_t adc_T)
{
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * (int32_t)dig_T2) >> 11;
    int32_t var2 = (((((adc_T >> 4) - (int32_t)dig_T1) *
                       ((adc_T >> 4) - (int32_t)dig_T1)) >> 12) * (int32_t)dig_T3) >> 14;
    t_fine = var1 + var2;
    return (t_fine * 5 + 128) >> 8;  // 0.01 degC
}

static uint32_t bmp280_compensate_press(int32_t adc_P)
{
    int64_t var1 = (int64_t)t_fine - 128000;
    int64_t var2 = var1 * var1 * (int64_t)dig_P6;
    var2 = var2 + ((var1 * (int64_t)dig_P5) << 17);
    var2 = var2 + ((int64_t)dig_P4 << 35);
    var1 = ((var1 * var1 * (int64_t)dig_P3) >> 8) + ((var1 * (int64_t)dig_P2) << 12);
    var1 = (((int64_t)1 << 47) + var1) * (int64_t)dig_P1 >> 33;
    if (var1 == 0) return 0;
    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = ((int64_t)dig_P9 * (p >> 13) * (p >> 13)) >> 25;
    var2 = ((int64_t)dig_P8 * p) >> 19;
    p = ((p + var1 + var2) >> 8) + ((int64_t)dig_P7 << 4);
    return (uint32_t)(p >> 8);  // Pa
}

// ============================================================
// Main
// ============================================================

int main(void)
{
    // Pin this process to Core 3 (matches isolcpus=3 in cmdline)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0)
        perror("sched_setaffinity (non-fatal)");

    // Real-time priority — reduces jitter on isolated core
    struct sched_param sp = { .sched_priority = 99 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
        perror("sched_setscheduler (non-fatal, run as root)");

    // Map BCM2837 BSC1 I2C registers via /dev/mem
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) { perror("open /dev/mem"); return 1; }
    bsc = (volatile uint32_t *)mmap(NULL, MMAP_PAGE_SIZE,
                                    PROT_READ | PROT_WRITE,
                                    MAP_SHARED, mem_fd, BSC1_PHYS);
    if (bsc == MAP_FAILED) { perror("mmap BSC1"); return 1; }
    printf("BSC1 mapped at phys=0x%lX\n", BSC1_PHYS);

    // Map ring buffer via /dev/bmp280 (kernel driver allocated it)
    int bmp_fd = open("/dev/bmp280", O_RDWR);
    if (bmp_fd < 0) { perror("open /dev/bmp280"); return 1; }
    void *shm = mmap(NULL, BUF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, bmp_fd, 0);
    if (shm == MAP_FAILED) { perror("mmap /dev/bmp280"); return 1; }

    bootstrap_cfg *boot = (bootstrap_cfg *)shm;
    ring_entry    *ring = (ring_entry *)((uint8_t *)shm + BOOTSTRAP_SIZE);

    // Verify bootstrap magic — kernel driver must be loaded
    if (boot->magic != BOOTSTRAP_MAGIC) {
        fprintf(stderr, "bad magic 0x%016llX — is bmp280_driver.ko loaded?\n",
                (unsigned long long)boot->magic);
        return 1;
    }
    printf("bootstrap OK  ring_phys=0x%llX  entries=%llu\n",
           (unsigned long long)boot->ring_phys_addr,
           (unsigned long long)boot->ring_entry_count);

    // Verify BMP280 chip ID
    uint8_t chip_id = 0;
    if (i2c_read(BMP280_ADDR, BMP280_REG_ID, &chip_id, 1) < 0 || chip_id != BMP280_CHIP_ID) {
        fprintf(stderr, "BMP280 not found chip_id=0x%02X (expected 0x58)\n", chip_id);
        return 1;
    }
    printf("BMP280 chip_id=0x%02X OK\n", chip_id);

    // Read calibration trimming params
    if (bmp280_read_calib() < 0) { fprintf(stderr, "calibration read failed\n"); return 1; }
    printf("calibration OK  dig_T1=%u dig_T2=%d dig_T3=%d\n", dig_T1, dig_T2, dig_T3);

    // Set normal mode — continuous sampling at max oversampling
    uint8_t ctrl_cmd[2] = { BMP280_REG_CTRL, BMP280_CTRL_NORMAL };
    if (i2c_write(BMP280_ADDR, ctrl_cmd, 2) < 0) {
        fprintf(stderr, "ctrl_meas write failed\n"); return 1;
    }
    printf("normal mode set (0xF4=0x57)\n");

    // 1ms loop timing
    uint64_t freq     = cntfrq();          // 19200000 on RPi3
    uint64_t tick_1ms = freq / 1000;       // cycles per millisecond
    printf("CNTFRQ=%llu  1ms=%llu cycles\n",
           (unsigned long long)freq, (unsigned long long)tick_1ms);
    printf("starting 1ms ring write loop...\n");
    fflush(stdout);

    uint32_t idx  = 0;
    uint64_t next = cntvct();

    while (1) {
        // Read raw ADC: 6 bytes from 0xF7
        // [0..2] = press_msb, press_lsb, press_xlsb
        // [3..5] = temp_msb, temp_lsb, temp_xlsb
        uint8_t adc[6];
        if (i2c_read(BMP280_ADDR, BMP280_REG_ADC, adc, 6) < 0) {
            next += tick_1ms;
            while (cntvct() < next);
            continue;
        }

        int32_t adc_P = ((int32_t)adc[0] << 12) | ((int32_t)adc[1] << 4) | (adc[2] >> 4);
        int32_t adc_T = ((int32_t)adc[3] << 12) | ((int32_t)adc[4] << 4) | (adc[5] >> 4);

        // Compensate — temp first (sets t_fine used by pressure)
        int32_t  temp  = bmp280_compensate_temp(adc_T);
        uint32_t press = bmp280_compensate_press(adc_P);
        uint64_t ts    = cntvct();

        // Write ring entry
        ring[idx].timestamp = ts;
        ring[idx].pressure  = press;
        ring[idx].temp      = temp;
        __asm__ volatile("dmb st" ::: "memory");  // ensure entry visible before idx advance
        idx = (idx + 1) % RING_ENTRY_COUNT;
        boot->write_idx = idx;  // stored in reserved[0] — Linux reads for debug

        // Heartbeat (Phase 5) — Core 3 is the only writer of this field.
        // Driver reads it every 250ms to detect if Core 3 has stalled.
        boot->heartbeat_counter++;

        // Alarm threshold check — set flags bit0 = ALARM
        uint64_t flags = 0;
        if (press < boot->threshold_press)           flags |= 1;  // pressure too low
        if (temp  > (int32_t)boot->threshold_temp)  flags |= 1;  // temp too high
        boot->flags = flags;

        // Busy-wait until next 1ms slot
        next += tick_1ms;
        while (cntvct() < next);
    }

    // Unreachable but clean
    munmap(shm, BUF_SIZE);
    munmap((void *)bsc, MMAP_PAGE_SIZE);
    close(bmp_fd);
    close(mem_fd);
    return 0;
}
