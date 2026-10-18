// ============================================================
// modbus_thread.c — Phase 9: Modbus TCP server
// ============================================================
// Read-only Input Register server (function code 0x04), port 502.
// SCADA/HMI polls this; the daemon never writes back via Modbus.
//
// 6 registers:
//   0 — pressure (Pa), latest ring buffer sample
//   1 — temperature (×0.01 °C), latest ring buffer sample
//   2 — alarm flags (ALARM | PIPE_BURST | OVERFLOW bits)
//   3 — threshold_press (from shared memory bootstrap_cfg)
//   4 — threshold_temp  (from shared memory bootstrap_cfg)
//   5 — daemon health: 0=healthy 1=degraded 2=given_up
//
// If sensor is not available (boot/ring NULL), all registers
// return 0 — same sentinel approach as mqtt_thread.
// ============================================================

#include "modbus_thread.h"
#include "logging.h"
#include "status_buf.h"
#include <modbus/modbus.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#define MODBUS_PORT      502   // standard Modbus TCP port (IEC 61158)
#define NUM_REGISTERS    6

// Register indices — SCADA/HMI reads these by address number
#define REG_PRESSURE     0
#define REG_TEMP         1
#define REG_FLAGS        2
#define REG_THRESH_PRESS 3
#define REG_THRESH_TEMP  4
#define REG_HEALTH       5

void *modbus_thread(void *arg)
{
    thread_ctx *ctx = (thread_ctx *)arg;

    log_msg("[modbus] thread started, port=%d", MODBUS_PORT);

    // modbus_new_tcp() — creates a Modbus TCP context.
    // "0.0.0.0" means listen on ALL network interfaces (WiFi, Ethernet, etc.)
    // so any client on any network can reach us — transport-agnostic by design.
    // If you wanted to restrict to one interface, pass that IP instead.
    modbus_t *mb = modbus_new_tcp("0.0.0.0", MODBUS_PORT);
    if (!mb) {
        log_msg("[modbus] FATAL: modbus_new_tcp failed: %s — thread exiting",
                modbus_strerror(errno));
        return NULL;
    }

    // modbus_mapping_new_start_address() — allocates the register map in memory.
    // Modbus has 4 types of registers; we only use Input Registers (read-only 16-bit).
    // Args: (coil_start, coil_count,        ← 1-bit read/write — not used
    //         di_start,   di_count,          ← 1-bit read-only  — not used
    //         hr_start,   hr_count,          ← 16-bit read/write (holding) — not used
    //         ir_start,   ir_count)          ← 16-bit read-only (input) — OUR 6 registers
    // map->tab_input_registers[0..5] is the array we write sensor values into.
    modbus_mapping_t *map = modbus_mapping_new_start_address(
        0, 0,           // coils: none
        0, 0,           // discrete inputs: none
        0, 0,           // holding registers: none
        0, NUM_REGISTERS // input registers: 6, starting at address 0
    );
    if (!map) {
        log_msg("[modbus] FATAL: modbus_mapping_new failed: %s — thread exiting",
                modbus_strerror(errno));
        modbus_free(mb);
        return NULL;
    }

    // modbus_tcp_listen() — opens the TCP socket and starts listening.
    // Arg "1" = backlog (max queued connections waiting to be accepted).
    // Returns a server file descriptor (like a normal BSD socket listen()).
    int server_fd = modbus_tcp_listen(mb, 1);
    if (server_fd < 0) {
        log_msg("[modbus] FATAL: modbus_tcp_listen failed: %s — thread exiting",
                modbus_strerror(errno));
        modbus_mapping_free(map);
        modbus_free(mb);
        return NULL;
    }

    log_msg("[modbus] listening on port %d", MODBUS_PORT);
    g_status.modbus.listening = 1;
    status_log(g_status.modbus_log, &g_status.modbus_log_idx, "listening on port 502");

    // Buffer for incoming Modbus request frames
    uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];

    while (*ctx->running) {
        // select() — wait up to 1 second for a client to connect, then loop.
        // Why not just call modbus_tcp_accept() directly? Because accept()
        // blocks forever — the thread would hang and never see *ctx->running=0
        // on shutdown. select() with a 1s timeout lets us check the shutdown
        // flag every second, same idea as interruptible_sleep().
        //
        // fd_set = a set of file descriptors to watch.
        // FD_ZERO clears it, FD_SET adds server_fd to it.
        // select() returns: 0=timeout, >0=something ready, <0=error.
        fd_set rfds;
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);

        int ret = select(server_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;   // interrupted by signal, try again
            log_msg("[modbus] select error: %s", strerror(errno));
            break;
        }
        if (ret == 0) continue;   // 1s timeout — no client yet, check *running

        // modbus_tcp_accept() — accepts the incoming client connection.
        // Internally does accept() on server_fd and updates mb's internal
        // socket so subsequent send/receive calls go to this client.
        if (modbus_tcp_accept(mb, &server_fd) < 0) {
            log_msg("[modbus] accept failed: %s", modbus_strerror(errno));
            continue;
        }

        log_msg("[modbus] client connected");
        g_status.modbus.client_connected = 1;
        status_log(g_status.modbus_log, &g_status.modbus_log_idx, "client connected");

        // Serve this client until it disconnects or daemon shuts down
        while (*ctx->running) {
            // Fill register map with latest values before answering each poll
            bootstrap_cfg *boot = ctx->boot;
            ring_entry    *ring = ctx->ring;

            if (boot && ring) {
                uint64_t wi = boot->write_idx;
                ring_entry latest = ring[wi == 0 ? RING_ENTRY_COUNT - 1 : wi - 1];

                // Cast to uint16_t — Modbus registers are 16-bit.
                // pressure in Pa fits in 16 bits (max ~65535 Pa, BMP280 range is fine).
                // temp is stored as int×100 (e.g. 2500 = 25.00°C), also fits in 16 bits.
                map->tab_input_registers[REG_PRESSURE]     = (uint16_t)(latest.pressure & 0xFFFF);
                map->tab_input_registers[REG_TEMP]         = (uint16_t)(latest.temp & 0xFFFF);
                map->tab_input_registers[REG_FLAGS]        = (uint16_t)(boot->flags & 0xFFFF);
                map->tab_input_registers[REG_THRESH_PRESS] = (uint16_t)(boot->threshold_press & 0xFFFF);
                map->tab_input_registers[REG_THRESH_TEMP]  = (uint16_t)(boot->threshold_temp & 0xFFFF);
            } else {
                // Sensor not ready — return zeros for all sensor registers
                for (int i = 0; i < NUM_REGISTERS - 1; i++)
                    map->tab_input_registers[i] = 0;
            }
            // Health is always available regardless of sensor state
            map->tab_input_registers[REG_HEALTH] = (uint16_t)(*ctx->health);

            // modbus_receive() — waits for a request from the connected client.
            // Returns the number of bytes in the request, or -1 on disconnect/error.
            int rc = modbus_receive(mb, query);
            if (rc < 0) {
                log_msg("[modbus] client disconnected or error: %s",
                        modbus_strerror(errno));
                g_status.modbus.client_connected = 0;
                status_log(g_status.modbus_log, &g_status.modbus_log_idx,
                           "client disconnected");
                break;   // go back to outer loop, wait for next client
            }

            // modbus_reply() — reads the request in query[], looks up the
            // answer from map, and sends the response back to the client.
            // Handles all Modbus framing (transaction ID, unit ID, CRC, etc.)
            modbus_reply(mb, query, rc, map);
            g_status.modbus.last_poll = time(NULL);
            for (int i = 0; i < NUM_REGISTERS; i++)
                g_status.modbus.regs[i] = map->tab_input_registers[i];
        }
    }

    // Clean up — close socket, free register map and context
    modbus_close(mb);
    modbus_mapping_free(map);
    modbus_free(mb);

    log_msg("[modbus] thread stopping (shutdown requested)");
    return NULL;
}
