FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://wpa_supplicant.conf file://wpa-init"

do_install:append() {
    install -d ${D}${sysconfdir}/wpa_supplicant/
    install -m 0600 ${WORKDIR}/wpa_supplicant.conf \
        ${D}${sysconfdir}/wpa_supplicant/wpa_supplicant.conf

    install -d ${D}${sysconfdir}/init.d/
    install -m 0755 ${WORKDIR}/wpa-init \
        ${D}${sysconfdir}/init.d/wpa-supplicant

    install -d ${D}${sysconfdir}/rc5.d/
    ln -sf ../init.d/wpa-supplicant \
        ${D}${sysconfdir}/rc5.d/S20wpa-supplicant
}
