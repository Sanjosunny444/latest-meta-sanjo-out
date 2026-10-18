SUMMARY = "BMP280 telemetry: IPC driver + Core 3 bare-metal writer + firstboot service"
DESCRIPTION = "Phase 4 — ships driver/core3_loop source + firstboot init script. \
Compiles on first boot (kernel-devsrc), registers sensor, loads driver, \
starts Core 3 1ms ring-buffer writer."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://bmp280_driver.c \
           file://core3_loop.c \
           file://read_ring.c \
           file://Makefile \
           file://bmp280-init"

S = "${WORKDIR}"

# Source compiled on-device at first boot (Route A) — nothing to build here.
do_compile[noexec] = "1"

RDEPENDS:${PN} = "kernel-devsrc gcc make binutils"

do_install() {
    # source + Makefile → /root/bmp280 (built on first boot)
    install -d ${D}/root/bmp280
    install -m 0644 ${WORKDIR}/bmp280_driver.c ${D}/root/bmp280/
    install -m 0644 ${WORKDIR}/core3_loop.c    ${D}/root/bmp280/
    install -m 0644 ${WORKDIR}/read_ring.c     ${D}/root/bmp280/
    install -m 0644 ${WORKDIR}/Makefile        ${D}/root/bmp280/

    # Copy 1 — /root/bmp280/bmp280-init
    # For manual/debug use: ssh in and run status|start|stop|restart by hand,
    # sits next to the source + Makefile it operates on. sysvinit never looks
    # here — this copy alone would NOT auto-run at boot.
    install -m 0755 ${WORKDIR}/bmp280-init     ${D}/root/bmp280/bmp280-init

    # Copy 2 — /etc/init.d/bmp280 + rc5.d/S99bmp280 symlink
    # This is what actually triggers auto-start at boot. sysvinit only scans
    # /etc/init.d + /etc/rcN.d for startup scripts — it has no knowledge of
    # /root/bmp280/. S99 = runs late in runlevel 5, after networking/I2C are up.
    install -d ${D}${sysconfdir}/init.d
    install -m 0755 ${WORKDIR}/bmp280-init ${D}${sysconfdir}/init.d/bmp280

    install -d ${D}${sysconfdir}/rc5.d
    ln -sf ../init.d/bmp280 ${D}${sysconfdir}/rc5.d/S99bmp280
}

FILES:${PN} = "/root/bmp280 ${sysconfdir}/init.d/bmp280 ${sysconfdir}/rc5.d/S99bmp280"