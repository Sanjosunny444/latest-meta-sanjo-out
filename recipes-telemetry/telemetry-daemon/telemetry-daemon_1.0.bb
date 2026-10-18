SUMMARY = "Telemetry daemon + web dashboard (Phase 9-12)"
DESCRIPTION = "Reads the BMP280 ring buffer, debounces/self-heals Core 3 \
health (core3_monitor), publishes sensor data and board health to MQTT \
(mqtt_thread), serves a Modbus TCP Input Register server on port 502 \
(modbus_thread), runs 8 FDD algorithms with SQLite logging (fdd_thread), \
and hosts a web dashboard on port 8080 (web_ui). Source compiled on-device \
at first boot, same Route A pattern as the bmp280 recipe."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://shared_mem.h \
           file://logging.h \
           file://logging.c \
           file://core3_monitor.h \
           file://core3_monitor.c \
           file://can_thread.h \
           file://can_thread.c \
           file://modbus_thread.h \
           file://modbus_thread.c \
           file://mqtt_thread.h \
           file://mqtt_thread.c \
           file://system_health.h \
           file://system_health.c \
           file://fdd_thread.h \
           file://fdd_thread.c \
           file://status_buf.h \
           file://status_writer.h \
           file://status_writer.c \
           file://telemetry_daemon.c \
           file://Makefile \
           file://web_ui.c \
           file://index.html \
           file://telemetry-init"

S = "${WORKDIR}"

# Source compiled on-device at first boot (Route A, same as the bmp280
# recipe) — nothing to build here at Yocto build time.
do_compile[noexec] = "1"

# gcc/make/binutils for the on-device compile; paho-mqtt-c(-dev) for
# mqtt_thread.c's #include <MQTTClient.h> and -lpaho-mqtt3c link step —
# both need to actually be present on the target, not just assumed.
RDEPENDS:${PN} = "kernel-devsrc gcc make binutils paho-mqtt-c paho-mqtt-c-dev libmodbus libmodbus-dev sqlite3 libsqlite3-dev"

# Yocto's QA normally flags an RDEPENDS on a -dev package as a mistake —
# -dev packages (headers, unversioned .so) are meant for build-time only,
# a normal binary doesn't need them at runtime. That assumption doesn't
# hold here: do_compile is disabled above, the REAL compile happens on the
# device itself at firstboot (Route A), so paho-mqtt-c-dev genuinely is
# needed at what Yocto calls "runtime" for this recipe. Confirmed real
# build failure without this skip: "do_package_qa: QA Issue: telemetry-daemon
# rdepends on paho-mqtt-c-dev [dev-deps]" — not a hypothetical concern.
INSANE_SKIP:${PN} += "dev-deps"

do_install() {
    # source + Makefile → /root/telemetry_daemon (built on first boot)
    install -d ${D}/root/telemetry_daemon
    install -m 0644 ${WORKDIR}/shared_mem.h       ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/logging.h          ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/logging.c          ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/core3_monitor.h    ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/core3_monitor.c    ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/can_thread.h       ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/can_thread.c       ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/modbus_thread.h    ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/modbus_thread.c    ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/mqtt_thread.h      ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/mqtt_thread.c      ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/system_health.h    ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/system_health.c    ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/fdd_thread.h       ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/fdd_thread.c       ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/status_buf.h       ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/status_writer.h    ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/status_writer.c    ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/telemetry_daemon.c ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/Makefile           ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/web_ui.c           ${D}/root/telemetry_daemon/
    install -m 0644 ${WORKDIR}/index.html         ${D}/root/telemetry_daemon/

    # firstboot init script
    install -m 0755 ${WORKDIR}/telemetry-init     ${D}/root/telemetry_daemon/telemetry-init

    # sysvinit service → runs at boot (rc5.d). Same "S99" number as bmp280 —
    # sorts after it alphabetically (b < t), so bmp280-init always fires
    # first; telemetry-init's own worker still waits for /dev/bmp280 itself
    # rather than relying on rc5.d ordering alone (bmp280-init's worker is
    # detached, so ordering alone wouldn't guarantee it's actually ready).
    install -d ${D}${sysconfdir}/init.d
    install -m 0755 ${WORKDIR}/telemetry-init ${D}${sysconfdir}/init.d/telemetrydaemon

    install -d ${D}${sysconfdir}/rc5.d
    ln -sf ../init.d/telemetrydaemon ${D}${sysconfdir}/rc5.d/S99telemetrydaemon
}

FILES:${PN} = "/root/telemetry_daemon ${sysconfdir}/init.d/telemetrydaemon ${sysconfdir}/rc5.d/S99telemetrydaemon"
