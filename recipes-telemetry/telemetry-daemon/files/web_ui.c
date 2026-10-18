// ============================================================
// web_ui.c — Phase 12: Lightweight C HTTP server for dashboard
// ============================================================
// Serves index.html + JSON API. No external dependencies.
// Reads /var/run/telemetry_status.json and /root/bmp280/fdd.db
// Port 8080, single-threaded (handles one request at a time).
// ============================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sqlite3.h>

#define PORT             8080
#define STATUS_JSON_PATH "/var/run/telemetry_status.json"
#define FDD_DB_PATH      "/root/bmp280/fdd.db"
#define BACKLOG          5
#define REQ_BUF_SIZE     4096
#define RESP_BUF_SIZE    65536
#define PAGE_SIZE        50

static volatile int running = 1;

static void sig_handler(int sig) { (void)sig; running = 0; }

// Send a full HTTP response
static void send_response(int fd, const char *status, const char *content_type,
                           const char *body, int body_len)
{
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n", status, content_type, body_len);
    write(fd, header, hlen);
    if (body_len > 0)
        write(fd, body, body_len);
}

// Read a file into a malloc'd buffer, return size (-1 on error)
static int read_file(const char *path, char **out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = malloc(sz + 1);
    if (!*out) { fclose(f); return -1; }
    int n = fread(*out, 1, sz, f);
    (*out)[n] = '\0';
    fclose(f);
    return n;
}

// Serve /api/status — just proxy the JSON file
static void handle_api_status(int fd)
{
    char *body = NULL;
    int len = read_file(STATUS_JSON_PATH, &body);
    if (len < 0) {
        send_response(fd, "200 OK", "application/json", "{}", 2);
    } else {
        send_response(fd, "200 OK", "application/json", body, len);
        free(body);
    }
}

// Parse query string param: returns malloc'd value or NULL
static char *get_param(const char *qs, const char *key)
{
    if (!qs) return NULL;
    int klen = strlen(key);
    const char *p = qs;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == qs || *(p-1) == '&' || *(p-1) == '?') && p[klen] == '=') {
            const char *val = p + klen + 1;
            const char *end = strchr(val, '&');
            int vlen = end ? (int)(end - val) : (int)strlen(val);
            char *result = malloc(vlen + 1);
            memcpy(result, val, vlen);
            result[vlen] = '\0';
            return result;
        }
        p++;
    }
    return NULL;
}

// Serve /api/fdd/events?page=N&algo=X&sensor=Y&sort=col&dir=DESC
static void handle_fdd_events(int fd, const char *qs)
{
    char *p_page   = get_param(qs, "page");
    char *p_algo   = get_param(qs, "algo");
    char *p_sensor = get_param(qs, "sensor");
    char *p_sort   = get_param(qs, "sort");
    char *p_dir    = get_param(qs, "dir");

    int page = p_page ? atoi(p_page) : 1;
    if (page < 1) page = 1;

    const char *sort_col = "id";
    if (p_sort && (strcmp(p_sort,"id")==0 || strcmp(p_sort,"ts")==0 ||
        strcmp(p_sort,"algorithm")==0 || strcmp(p_sort,"sensor")==0 ||
        strcmp(p_sort,"value")==0 || strcmp(p_sort,"score")==0 ||
        strcmp(p_sort,"fault_label")==0))
        sort_col = p_sort;

    const char *sort_dir = "DESC";
    if (p_dir && (strcmp(p_dir,"ASC")==0 || strcmp(p_dir,"asc")==0))
        sort_dir = "ASC";

    sqlite3 *db;
    if (sqlite3_open_v2(FDD_DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        const char *err = "{\"error\":\"cannot open db\",\"rows\":[],\"total\":0,\"page\":1,\"pages\":0}";
        send_response(fd, "200 OK", "application/json", err, strlen(err));
        goto cleanup;
    }

    // Build WHERE clause
    char where[256] = "";
    int wpos = 0;
    if (p_algo && strlen(p_algo) > 0)
        wpos += snprintf(where+wpos, sizeof(where)-wpos, " WHERE algorithm='%s'", p_algo);
    if (p_sensor && strlen(p_sensor) > 0) {
        if (wpos > 0)
            wpos += snprintf(where+wpos, sizeof(where)-wpos, " AND sensor='%s'", p_sensor);
        else
            wpos += snprintf(where+wpos, sizeof(where)-wpos, " WHERE sensor='%s'", p_sensor);
    }

    // Count total
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM events%s", where);
    sqlite3_stmt *stmt;
    int total = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            total = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    int pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    int offset = (page - 1) * PAGE_SIZE;

    // Fetch rows
    snprintf(sql, sizeof(sql),
        "SELECT id,ts,algorithm,sensor,value,score,fault_label FROM events%s "
        "ORDER BY %s %s LIMIT %d OFFSET %d",
        where, sort_col, sort_dir, PAGE_SIZE, offset);

    // Build JSON response dynamically
    char *resp = malloc(RESP_BUF_SIZE);
    int rpos = 0;
    rpos += snprintf(resp+rpos, RESP_BUF_SIZE-rpos,
        "{\"rows\":[");

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        int first = 1;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) resp[rpos++] = ',';
            first = 0;
            rpos += snprintf(resp+rpos, RESP_BUF_SIZE-rpos,
                "{\"id\":%d,\"ts\":\"%s\",\"algorithm\":\"%s\","
                "\"sensor\":\"%s\",\"value\":%.2f,\"score\":%.2f,"
                "\"fault_label\":\"%s\"}",
                sqlite3_column_int(stmt, 0),
                sqlite3_column_text(stmt, 1) ? (const char*)sqlite3_column_text(stmt, 1) : "",
                sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "",
                sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "",
                sqlite3_column_double(stmt, 4),
                sqlite3_column_double(stmt, 5),
                sqlite3_column_text(stmt, 6) ? (const char*)sqlite3_column_text(stmt, 6) : "");
        }
        sqlite3_finalize(stmt);
    }

    // Get distinct algorithms and sensors for filter dropdowns
    rpos += snprintf(resp+rpos, RESP_BUF_SIZE-rpos,
        "],\"total\":%d,\"page\":%d,\"page_size\":%d,\"pages\":%d,"
        "\"filters\":{\"algorithms\":[", total, page, PAGE_SIZE, pages);

    if (sqlite3_prepare_v2(db, "SELECT DISTINCT algorithm FROM events ORDER BY algorithm",
                            -1, &stmt, NULL) == SQLITE_OK) {
        int first = 1;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) resp[rpos++] = ',';
            first = 0;
            rpos += snprintf(resp+rpos, RESP_BUF_SIZE-rpos, "\"%s\"",
                sqlite3_column_text(stmt, 0) ? (const char*)sqlite3_column_text(stmt, 0) : "");
        }
        sqlite3_finalize(stmt);
    }

    rpos += snprintf(resp+rpos, RESP_BUF_SIZE-rpos, "],\"sensors\":[");

    if (sqlite3_prepare_v2(db, "SELECT DISTINCT sensor FROM events ORDER BY sensor",
                            -1, &stmt, NULL) == SQLITE_OK) {
        int first = 1;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) resp[rpos++] = ',';
            first = 0;
            rpos += snprintf(resp+rpos, RESP_BUF_SIZE-rpos, "\"%s\"",
                sqlite3_column_text(stmt, 0) ? (const char*)sqlite3_column_text(stmt, 0) : "");
        }
        sqlite3_finalize(stmt);
    }

    rpos += snprintf(resp+rpos, RESP_BUF_SIZE-rpos, "]}}");

    send_response(fd, "200 OK", "application/json", resp, rpos);
    free(resp);
    sqlite3_close(db);

cleanup:
    free(p_page); free(p_algo); free(p_sensor); free(p_sort); free(p_dir);
}

// Serve /api/fdd/export.csv
static void handle_fdd_csv(int fd, const char *qs)
{
    char *p_algo   = get_param(qs, "algo");
    char *p_sensor = get_param(qs, "sensor");

    sqlite3 *db;
    if (sqlite3_open_v2(FDD_DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        send_response(fd, "500 Internal Server Error", "text/plain", "DB error", 8);
        free(p_algo); free(p_sensor);
        return;
    }

    char where[256] = "";
    int wpos = 0;
    if (p_algo && strlen(p_algo) > 0)
        wpos += snprintf(where+wpos, sizeof(where)-wpos, " WHERE algorithm='%s'", p_algo);
    if (p_sensor && strlen(p_sensor) > 0) {
        if (wpos > 0)
            wpos += snprintf(where+wpos, sizeof(where)-wpos, " AND sensor='%s'", p_sensor);
        else
            wpos += snprintf(where+wpos, sizeof(where)-wpos, " WHERE sensor='%s'", p_sensor);
    }

    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT id,ts,algorithm,sensor,value,score,fault_label FROM events%s ORDER BY id DESC", where);

    sqlite3_stmt *stmt;
    // Build CSV in memory
    char *csv = malloc(RESP_BUF_SIZE * 4);
    int cpos = 0;
    cpos += snprintf(csv+cpos, RESP_BUF_SIZE*4-cpos, "id,ts,algorithm,sensor,value,score,fault_label\n");

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            cpos += snprintf(csv+cpos, RESP_BUF_SIZE*4-cpos,
                "%d,%s,%s,%s,%.2f,%.2f,%s\n",
                sqlite3_column_int(stmt, 0),
                sqlite3_column_text(stmt, 1) ? (const char*)sqlite3_column_text(stmt, 1) : "",
                sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "",
                sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "",
                sqlite3_column_double(stmt, 4),
                sqlite3_column_double(stmt, 5),
                sqlite3_column_text(stmt, 6) ? (const char*)sqlite3_column_text(stmt, 6) : "");
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);

    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/csv\r\n"
        "Content-Disposition: attachment; filename=fdd_events.csv\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n", cpos);
    write(fd, header, hlen);
    write(fd, csv, cpos);
    free(csv);
    free(p_algo); free(p_sensor);
}

// Serve /api/fdd/analysis — events grouped by algorithm for charts
// Returns: { "algorithms": { "zscore": [{ts,score,sensor,fault_label},...], "cusum": [...], ... },
//             "timeline": [{ts,algorithm,score,sensor},...] (last 200 events, chronological),
//             "summary": { "zscore": count, "cusum": count, ... } }
static void handle_fdd_analysis(int fd)
{
    sqlite3 *db;
    if (sqlite3_open_v2(FDD_DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        const char *err = "{\"algorithms\":{},\"timeline\":[],\"summary\":{}}";
        send_response(fd, "200 OK", "application/json", err, strlen(err));
        return;
    }

    char *resp = malloc(RESP_BUF_SIZE * 2);
    int rpos = 0;

    // --- Summary: count per algorithm ---
    rpos += snprintf(resp+rpos, RESP_BUF_SIZE*2-rpos, "{\"summary\":{");
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT algorithm, COUNT(*) FROM events GROUP BY algorithm ORDER BY algorithm",
            -1, &stmt, NULL) == SQLITE_OK) {
        int first = 1;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) resp[rpos++] = ',';
            first = 0;
            rpos += snprintf(resp+rpos, RESP_BUF_SIZE*2-rpos, "\"%s\":%d",
                sqlite3_column_text(stmt, 0) ? (const char*)sqlite3_column_text(stmt, 0) : "",
                sqlite3_column_int(stmt, 1));
        }
        sqlite3_finalize(stmt);
    }

    // --- Per-algorithm events (last 100 per algorithm) ---
    rpos += snprintf(resp+rpos, RESP_BUF_SIZE*2-rpos, "},\"algorithms\":{");

    // Get distinct algorithms
    char algos[8][32];
    int nalgo = 0;
    if (sqlite3_prepare_v2(db, "SELECT DISTINCT algorithm FROM events ORDER BY algorithm",
                            -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW && nalgo < 8) {
            const char *a = (const char*)sqlite3_column_text(stmt, 0);
            if (a) strncpy(algos[nalgo++], a, 31);
        }
        sqlite3_finalize(stmt);
    }

    for (int i = 0; i < nalgo; i++) {
        if (i > 0) resp[rpos++] = ',';
        rpos += snprintf(resp+rpos, RESP_BUF_SIZE*2-rpos, "\"%s\":[", algos[i]);

        char sql[512];
        snprintf(sql, sizeof(sql),
            "SELECT ts,score,sensor,fault_label,value FROM events "
            "WHERE algorithm='%s' ORDER BY id DESC LIMIT 100", algos[i]);

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            int first = 1;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) resp[rpos++] = ',';
                first = 0;
                rpos += snprintf(resp+rpos, RESP_BUF_SIZE*2-rpos,
                    "{\"ts\":\"%s\",\"score\":%.2f,\"sensor\":\"%s\","
                    "\"fault\":\"%s\",\"value\":%.2f}",
                    sqlite3_column_text(stmt, 0) ? (const char*)sqlite3_column_text(stmt, 0) : "",
                    sqlite3_column_double(stmt, 1),
                    sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "",
                    sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "",
                    sqlite3_column_double(stmt, 4));
            }
            sqlite3_finalize(stmt);
        }
        resp[rpos++] = ']';
    }

    // --- Timeline: last 200 events chronological ---
    rpos += snprintf(resp+rpos, RESP_BUF_SIZE*2-rpos, "},\"timeline\":[");

    if (sqlite3_prepare_v2(db,
            "SELECT ts,algorithm,score,sensor,fault_label FROM events "
            "ORDER BY id DESC LIMIT 200",
            -1, &stmt, NULL) == SQLITE_OK) {
        int first = 1;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) resp[rpos++] = ',';
            first = 0;
            rpos += snprintf(resp+rpos, RESP_BUF_SIZE*2-rpos,
                "{\"ts\":\"%s\",\"algo\":\"%s\",\"score\":%.2f,"
                "\"sensor\":\"%s\",\"fault\":\"%s\"}",
                sqlite3_column_text(stmt, 0) ? (const char*)sqlite3_column_text(stmt, 0) : "",
                sqlite3_column_text(stmt, 1) ? (const char*)sqlite3_column_text(stmt, 1) : "",
                sqlite3_column_double(stmt, 2),
                sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "",
                sqlite3_column_text(stmt, 4) ? (const char*)sqlite3_column_text(stmt, 4) : "");
        }
        sqlite3_finalize(stmt);
    }

    rpos += snprintf(resp+rpos, RESP_BUF_SIZE*2-rpos, "]}");

    send_response(fd, "200 OK", "application/json", resp, rpos);
    free(resp);
    sqlite3_close(db);
}

// Resolve path relative to the binary's own directory
static char binary_dir[512] = "";

static void resolve_binary_dir(void)
{
    ssize_t n = readlink("/proc/self/exe", binary_dir, sizeof(binary_dir) - 1);
    if (n > 0) {
        binary_dir[n] = '\0';
        char *slash = strrchr(binary_dir, '/');
        if (slash) *(slash + 1) = '\0';
    } else {
        strcpy(binary_dir, "/root/telemetry_daemon/");
    }
}

// Serve index.html (from same directory as binary)
static void handle_index(int fd)
{
    char path[600];
    snprintf(path, sizeof(path), "%sindex.html", binary_dir);
    char *body = NULL;
    int len = read_file(path, &body);
    if (len < 0) {
        const char *err = "<h1>index.html not found</h1>";
        send_response(fd, "404 Not Found", "text/html", err, strlen(err));
    } else {
        send_response(fd, "200 OK", "text/html; charset=utf-8", body, len);
        free(body);
    }
}

// Handle one HTTP request
static void handle_request(int client_fd)
{
    char buf[REQ_BUF_SIZE];
    int n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    // Parse GET /path?query HTTP/1.x
    if (strncmp(buf, "GET ", 4) != 0) {
        send_response(client_fd, "405 Method Not Allowed", "text/plain",
                      "Only GET supported", 18);
        return;
    }

    char *path_start = buf + 4;
    char *path_end = strchr(path_start, ' ');
    if (!path_end) return;
    *path_end = '\0';

    // Split path and query string
    char *qs = strchr(path_start, '?');
    if (qs) { *qs = '\0'; qs++; }

    if (strcmp(path_start, "/") == 0 || strcmp(path_start, "/index.html") == 0)
        handle_index(client_fd);
    else if (strcmp(path_start, "/api/status") == 0)
        handle_api_status(client_fd);
    else if (strcmp(path_start, "/api/fdd/events") == 0)
        handle_fdd_events(client_fd, qs);
    else if (strcmp(path_start, "/api/fdd/export.csv") == 0)
        handle_fdd_csv(client_fd, qs);
    else if (strcmp(path_start, "/api/fdd/analysis") == 0)
        handle_fdd_analysis(client_fd);
    else
        send_response(client_fd, "404 Not Found", "text/plain", "Not found", 9);
}

int main(void)
{
    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGPIPE, SIG_IGN);

    resolve_binary_dir();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    fprintf(stderr, "[web_ui] serving on http://0.0.0.0:%d\n", PORT);

    while (running) {
        fd_set fds;
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);

        int ret = select(server_fd + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0) continue;

        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        handle_request(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
