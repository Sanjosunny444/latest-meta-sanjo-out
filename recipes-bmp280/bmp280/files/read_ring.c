// Phase 3 Step 3 — userspace mmap verify
// Opens /dev/bmp280, reads bootstrap magic, prints ring entries
// Must match struct layout in bmp280_driver.c exactly

#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>

#define BOOTSTRAP_MAGIC     0xDEADBEEFCAFEBABEULL
#define RING_ENTRY_COUNT    1000
#define BOOTSTRAP_SIZE      72
#define RING_ENTRY_SIZE     16                              // 8+4+4
#define RING_SIZE           (RING_ENTRY_COUNT * RING_ENTRY_SIZE)
#define BUF_SIZE            16384   // PAGE_ALIGN(16064) — must match driver DMA_BUF_SIZE

// Must byte-match bmp280_driver.c struct bootstrap_cfg
typedef struct {
    uint64_t magic;
    uint64_t ring_phys_addr;
    uint64_t ring_entry_count;
    uint64_t threshold_press;
    uint64_t threshold_temp;
    uint64_t flags;
    uint64_t write_idx;          // Core 3 writes only
    uint64_t heartbeat_counter;  // Core 3 writes only — driver reads (Phase 5)
    uint64_t core3_dead;         // driver writes only — daemon reads (Phase 5)
} __attribute__((packed)) bootstrap_cfg;

// Must byte-match bmp280_driver.c struct ring_entry
typedef struct {
    uint64_t timestamp;
    uint32_t pressure;
    int32_t  temp;
} __attribute__((packed)) ring_entry;

int main(void)
{
    int fd = open("/dev/bmp280", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/bmp280");
        return 1;
    }

    void *buf = mmap(NULL, BUF_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    bootstrap_cfg *boot = (bootstrap_cfg *)buf;
    ring_entry    *ring = (ring_entry *)((uint8_t *)buf + BOOTSTRAP_SIZE);

    // Step 1: verify magic
    printf("magic     = 0x%016llX  ", (unsigned long long)boot->magic);
    if (boot->magic == BOOTSTRAP_MAGIC)
        printf("[OK]\n");
    else {
        printf("[FAIL — expected 0x%016llX]\n", BOOTSTRAP_MAGIC);
        munmap(buf, BUF_SIZE);
        close(fd);
        return 1;
    }

    printf("ring_phys = 0x%016llX\n", (unsigned long long)boot->ring_phys_addr);
    printf("entries   = %llu\n",      (unsigned long long)boot->ring_entry_count);
    printf("thr_press = %llu Pa\n",   (unsigned long long)boot->threshold_press);
    printf("thr_temp  = %llu (×0.01°C)\n", (unsigned long long)boot->threshold_temp);
    printf("flags     = 0x%llX  (bit0=ALARM bit1=PIPE_BURST bit2=OVERFLOW)\n",
           (unsigned long long)boot->flags);
    printf("write_idx = %llu\n", (unsigned long long)boot->write_idx);
    printf("heartbeat = %llu  (Core 3's counter, driver samples this every 250ms)\n",
           (unsigned long long)boot->heartbeat_counter);
    printf("core3     = %s\n", boot->core3_dead ? "DEAD (last check saw no change)" : "alive");
    printf("\n");

    // Step 2: scan ring for non-zero entries (Core 3 has written)
    int found = 0;
    for (int i = 0; i < RING_ENTRY_COUNT; i++) {
        if (ring[i].timestamp != 0) {
            printf("[%4d] ts=%llu  press=%u Pa  temp=%d (%.2f°C)\n",
                   i,
                   (unsigned long long)ring[i].timestamp,
                   ring[i].pressure,
                   ring[i].temp,
                   ring[i].temp / 100.0);
            found++;
            if (found >= 10) {          // print first 10 valid entries
                printf("  ... (showing first 10 non-zero entries)\n");
                break;
            }
        }
    }

    if (!found)
        printf("ring: all zeros — Core 3 not writing yet (expected at Step 3)\n");

    // Step 3: timestamp delta check (1ms verify)
    if (found >= 2) {
        uint64_t first_ts = 0, second_ts = 0;
        int cnt = 0;
        for (int i = 0; i < RING_ENTRY_COUNT && cnt < 2; i++) {
            if (ring[i].timestamp != 0) {
                if (cnt == 0) first_ts  = ring[i].timestamp;
                if (cnt == 1) second_ts = ring[i].timestamp;
                cnt++;
            }
        }
        uint64_t delta = second_ts - first_ts;
        printf("\ntimestamp delta [0→1] = %llu cycles\n", (unsigned long long)delta);
        printf("  RPi3 CNTFRQ = 19.2 MHz → 1ms = ~19200 cycles\n");
        printf("  measured: %llu cycles (~%.3f ms)\n",
               (unsigned long long)delta, delta / 19200.0);
    }

    munmap(buf, BUF_SIZE);
    close(fd);
    return 0;
}
