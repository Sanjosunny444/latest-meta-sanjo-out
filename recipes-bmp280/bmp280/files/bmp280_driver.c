// ============================================================
// BMP280 I2C Kernel Driver — Phase 3 (IPC infrastructure)
// ============================================================
// Role: allocate shared RAM, expose via mmap, write bootstrap config.
// Core 3 owns all BMP280 sensor reading (bare-metal I2C in core3_loop.c).
// Linux userspace reads ring buffer zero-copy via /dev/bmp280 mmap.
//
// DMA buffer layout:
//   [0x000..0x03F]  bootstrap_cfg (magic, ring_phys_addr, thresholds, flags)
//   [0x040..end]    ring_entry[1000] (timestamp + pressure + temp)
// ============================================================

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/miscdevice.h>   // misc_register — creates /dev/bmp280
#include <linux/fs.h>           // file_operations
#include <linux/mm.h>           // remap_pfn_range, pgprot_noncached, get_order
#include <linux/page-flags.h>   // SetPageReserved, ClearPageReserved
#include <linux/hrtimer.h>      // heartbeat monitor (Phase 5)
#include <linux/ktime.h>

// ============================================================
// Constants
// ============================================================

#define BOOTSTRAP_MAGIC       0xDEADBEEFCAFEBABEULL
#define RING_ENTRY_COUNT      1000
#define BOOTSTRAP_SIZE        72
#define RING_SIZE             (RING_ENTRY_COUNT * sizeof(struct ring_entry))
#define DMA_BUF_SIZE          PAGE_ALIGN(BOOTSTRAP_SIZE + RING_SIZE)

// Heartbeat monitor (Phase 5)
#define HEARTBEAT_CHECK_NS    (500ULL * NSEC_PER_MSEC)  // hrtimer period

// ============================================================
// Shared memory structures — must match core3_loop.c exactly
// ============================================================
//
// Phase 5 — heartbeat health monitor. Each field has exactly ONE writer,
// so there is no shared read-modify-write race anywhere in this struct:
//   heartbeat_counter — Core 3 writes only  → this driver reads
//   core3_dead        — this driver writes  → daemon (Phase 7) reads

struct bootstrap_cfg {
    u64  magic;              // 0xDEADBEEFCAFEBABE — signals buffer ready to Core 3
    u64  ring_phys_addr;     // physical address of ring buffer start
    u64  ring_entry_count;   // number of slots in ring buffer
    u64  threshold_press;    // alarm threshold in Pa (Linux configures)
    u64  threshold_temp;     // alarm threshold in millidegC (Linux configures)
    u64  flags;              // bit0=ALARM, bit1=PIPE_BURST, bit2=OVERFLOW (Core 3 sets)
    u64  write_idx;          // current ring write index (Core 3 writes, Linux reads)
    u64  heartbeat_counter;  // Core 3 increments every 1ms loop iteration
    u64  core3_dead;         // this driver sets: 1 = missed heartbeat since last check
} __packed;

struct ring_entry {
    u64  timestamp;    // ARM CNTVCT_EL0 cycle counter at sample time
    u32  pressure;     // Pa
    s32  temp;         // 0.01 degC
} __packed;

// ============================================================
// Per-device private data
// ============================================================

struct bmp280_data {
    struct device       *dev;

    void               *dma_vaddr;   // kernel virtual address of shared buffer
    dma_addr_t          dma_handle;  // physical address — Core 3 reads bootstrap to get this
    size_t              dma_size;

    struct bootstrap_cfg *bootstrap; // points to start of dma_vaddr
    struct ring_entry    *ring;      // points to dma_vaddr + BOOTSTRAP_SIZE

    struct miscdevice    misc;       // /dev/bmp280

    struct hrtimer        hb_timer;    // heartbeat monitor (Phase 5)
    u64                   hb_last_seen; // driver's private copy — never shared
};

// ============================================================
// sysfs — debug attributes
// ============================================================

// Physical address of shared buffer — used to verify Core 3 gets same addr
static ssize_t dma_phys_addr_show(struct device *dev,
                                   struct device_attribute *attr, char *buf)
{
    struct bmp280_data *data = dev_get_drvdata(dev);
    return sprintf(buf, "0x%llx\n", (unsigned long long)data->dma_handle);
}

// Current ring write index — Core 3 increments this each sample
// If increasing → Core 3 loop is running
static ssize_t write_idx_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
    struct bmp280_data *data = dev_get_drvdata(dev);
    return sprintf(buf, "%llu\n", data->bootstrap->write_idx);
}

// Alarm flags set by Core 3 — bit0=ALARM bit1=PIPE_BURST bit2=OVERFLOW
static ssize_t flags_show(struct device *dev,
                           struct device_attribute *attr, char *buf)
{
    struct bmp280_data *data = dev_get_drvdata(dev);
    return sprintf(buf, "0x%llx\n", data->bootstrap->flags);
}

// Core 3 health — set by this driver's heartbeat hrtimer (Phase 5), read by daemon
// 0 = heartbeat counter changed since last check (alive)
// 1 = heartbeat counter unchanged since last check (single-sample result — no
//     debounce here; daemon decides how many consecutive 1s mean "really dead")
static ssize_t core3_dead_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    struct bmp280_data *data = dev_get_drvdata(dev);
    return sprintf(buf, "%llu\n", data->bootstrap->core3_dead);
}

static DEVICE_ATTR_RO(dma_phys_addr);
static DEVICE_ATTR_RO(write_idx);
static DEVICE_ATTR_RO(flags);
static DEVICE_ATTR_RO(core3_dead);

static struct attribute *bmp280_attrs[] = {
    &dev_attr_dma_phys_addr.attr,
    &dev_attr_write_idx.attr,
    &dev_attr_flags.attr,
    &dev_attr_core3_dead.attr,
    NULL,
};
ATTRIBUTE_GROUPS(bmp280);

// ============================================================
// mmap — maps shared buffer into userspace VMA (zero-copy)
// ============================================================

static int bmp280_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct bmp280_data *data = file->private_data;
    unsigned long size = vma->vm_end - vma->vm_start;

    if (size > data->dma_size) {
        dev_err(data->dev, "mmap size %lu > buffer size %zu\n",
                size, data->dma_size);
        return -EINVAL;
    }

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    return remap_pfn_range(vma,
                           vma->vm_start,
                           PHYS_PFN(data->dma_handle),
                           size,
                           vma->vm_page_prot);
}

static int bmp280_open(struct inode *inode, struct file *file)
{
    struct bmp280_data *data = container_of(file->private_data,
                                            struct bmp280_data, misc);
    file->private_data = data;
    return 0;
}

static const struct file_operations bmp280_fops = {
    .owner = THIS_MODULE,
    .open  = bmp280_open,
    .mmap  = bmp280_mmap,
};

// ============================================================
// Heartbeat monitor — Phase 5
// ============================================================
// Core 3 increments bootstrap->heartbeat_counter every 1ms in its own loop.
// This hrtimer fires every 250ms in interrupt/atomic context (no sleeping, no
// blocking calls allowed here) and compares the counter against the value it
// saw last tick — purely a read on heartbeat_counter, purely a write on
// core3_dead. Neither field is touched by Core 3, so there is no race.
//
// No debounce logic here on purpose — this reports the raw single-sample
// result each tick. Deciding how many consecutive misses count as "really
// dead" is left to the userspace daemon (Phase 7), which can poll this value
// as often as it needs and tune that threshold without touching the kernel.
// ============================================================

static enum hrtimer_restart bmp280_heartbeat_tick(struct hrtimer *timer)
{
    struct bmp280_data *data = container_of(timer, struct bmp280_data, hb_timer);
    u64 cur = data->bootstrap->heartbeat_counter;

    data->bootstrap->core3_dead = (cur == data->hb_last_seen) ? 1 : 0;
    data->hb_last_seen = cur;

    hrtimer_forward_now(timer, ns_to_ktime(HEARTBEAT_CHECK_NS));
    return HRTIMER_RESTART;
}

// ============================================================
// probe()
// ============================================================

static int bmp280_probe(struct i2c_client *client)
{
    struct bmp280_data *data;
    unsigned long addr;
    int ret;

    data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->dev = &client->dev;
    i2c_set_clientdata(client, data);
    // No I2C access here — Core 3 owns the I2C bus entirely

    // --------------------------------------------------------
    // Allocate shared RAM for bootstrap + ring buffer
    // --------------------------------------------------------
    // __get_free_pages + virt_to_phys used instead of dma_alloc_coherent:
    //   - All 4 Cortex-A53 cores share L2 cache (hardware coherent)
    //   - virt_to_phys() gives stable linear-mapped physical address
    //   - Core 3 accesses via raw physical address (no MMU)
    data->dma_size  = DMA_BUF_SIZE;
    data->dma_vaddr = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
                                               get_order(data->dma_size));
    if (!data->dma_vaddr) {
        dev_err(data->dev, "shared buffer alloc failed (size=%zu)\n", data->dma_size);
        return -ENOMEM;
    }
    data->dma_handle = (dma_addr_t)virt_to_phys(data->dma_vaddr);

    // SetPageReserved on all pages — required for remap_pfn_range to work
    for (addr = (unsigned long)data->dma_vaddr;
         addr < (unsigned long)data->dma_vaddr + data->dma_size;
         addr += PAGE_SIZE)
        SetPageReserved(virt_to_page((void *)addr));

    // Convenience pointers
    data->bootstrap = (struct bootstrap_cfg *)data->dma_vaddr;
    data->ring      = (struct ring_entry *)((u8 *)data->dma_vaddr + BOOTSTRAP_SIZE);

    // --------------------------------------------------------
    // Write bootstrap config — Core 3 reads this at startup
    // --------------------------------------------------------
    data->bootstrap->magic            = BOOTSTRAP_MAGIC;
    data->bootstrap->ring_phys_addr   = data->dma_handle + BOOTSTRAP_SIZE;
    data->bootstrap->ring_entry_count = RING_ENTRY_COUNT;
    data->bootstrap->threshold_press  = 95000;  // Pa — low pressure alarm default
    data->bootstrap->threshold_temp   = 8000;   // 80.00°C — high temp alarm default
    smp_wmb();  // ensure all bootstrap fields visible before magic is seen by Core 3

    dev_info(data->dev, "DMA buffer: phys=0x%llx size=%zu\n",
             (unsigned long long)data->dma_handle, data->dma_size);
    dev_info(data->dev, "Ring buffer: phys=0x%llx entries=%d\n",
             (unsigned long long)data->bootstrap->ring_phys_addr, RING_ENTRY_COUNT);

    // --------------------------------------------------------
    // Register misc device — creates /dev/bmp280
    // --------------------------------------------------------
    data->misc.minor = MISC_DYNAMIC_MINOR;
    data->misc.name  = "bmp280";
    data->misc.fops  = &bmp280_fops;
    ret = misc_register(&data->misc);
    if (ret) {
        dev_err(data->dev, "misc_register failed: %d\n", ret);
        for (addr = (unsigned long)data->dma_vaddr;
             addr < (unsigned long)data->dma_vaddr + data->dma_size;
             addr += PAGE_SIZE)
            ClearPageReserved(virt_to_page((void *)addr));
        free_pages((unsigned long)data->dma_vaddr, get_order(data->dma_size));
        return ret;
    }

    dev_info(data->dev, "/dev/bmp280 created — mmap ready\n");

    // --------------------------------------------------------
    // Start heartbeat monitor (Phase 5)
    // --------------------------------------------------------
    data->hb_last_seen = data->bootstrap->heartbeat_counter;  // 0 at this point
    hrtimer_init(&data->hb_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    data->hb_timer.function = bmp280_heartbeat_tick;
    hrtimer_start(&data->hb_timer, ns_to_ktime(HEARTBEAT_CHECK_NS), HRTIMER_MODE_REL);
    dev_info(data->dev, "heartbeat monitor started (check every %llums)\n",
             (unsigned long long)(HEARTBEAT_CHECK_NS / NSEC_PER_MSEC));

    return 0;
}

// ============================================================
// remove()
// ============================================================

static void bmp280_remove(struct i2c_client *client)
{
    struct bmp280_data *data = i2c_get_clientdata(client);
    unsigned long addr;

    // Stop the heartbeat monitor first — its callback touches data->bootstrap,
    // which must not happen after the memory below is freed
    hrtimer_cancel(&data->hb_timer);

    // Clear magic first — Core 3 checks this and stops writing
    data->bootstrap->magic = 0;
    smp_wmb();

    misc_deregister(&data->misc);

    for (addr = (unsigned long)data->dma_vaddr;
         addr < (unsigned long)data->dma_vaddr + data->dma_size;
         addr += PAGE_SIZE)
        ClearPageReserved(virt_to_page((void *)addr));

    free_pages((unsigned long)data->dma_vaddr, get_order(data->dma_size));
    dev_info(&client->dev, "bmp280: removed\n");
}

// ============================================================
// Device matching
// ============================================================

static const struct i2c_device_id bmp280_id[] = {
    { "bmp280_ipc", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, bmp280_id);

static const struct of_device_id bmp280_of_match[] = {
    { .compatible = "bosch,bmp280" },
    { }
};
MODULE_DEVICE_TABLE(of, bmp280_of_match);

static struct i2c_driver bmp280_driver = {
    .driver = {
        .name           = "bmp280_ipc",
        .of_match_table = bmp280_of_match,
        .dev_groups     = bmp280_groups,
    },
    .probe    = bmp280_probe,
    .remove   = bmp280_remove,
    .id_table = bmp280_id,
};
module_i2c_driver(bmp280_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sanjo");
MODULE_DESCRIPTION("BMP280 IPC driver (bmp280_ipc) — shared ring buffer for Core 3 telemetry");