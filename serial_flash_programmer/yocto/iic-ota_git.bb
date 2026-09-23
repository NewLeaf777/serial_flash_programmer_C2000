SUMMARY = "iic_ota: Live DFU firmware update library for TI C2000 DSPs over serial"
HOMEPAGE = "https://github.com/NewLeaf777/serial_flash_programmer_C2000"
LICENSE = "Unlicense"
LIC_FILES_CHKSUM = "file://LICENSE;md5=d88e9e08385d2a17052dac348bde4bc1"

SRC_URI = "git://github.com/NewLeaf777/serial_flash_programmer_C2000.git;protocol=https;branch=master"
# Pin to a commit that contains serial_flash_programmer/Makefile.am62x for reproducible builds.
SRCREV = "${AUTOREV}"
PV = "1.0+git${SRCPV}"

S = "${WORKDIR}/git"
B = "${S}/serial_flash_programmer"

# Makefile.am62x keeps CXX/AR from the environment (Yocto's cross toolchain) and
# uses "?=" for CXXFLAGS, so the -std/-fPIC it needs are passed explicitly.
# LDFLAGS is appended because the Makefile's shared-library link line only uses CXXFLAGS.
do_compile() {
    oe_runmake -C ${B} -f Makefile.am62x \
        CXXFLAGS="${CXXFLAGS} -std=c++11 -fPIC ${LDFLAGS}"
}

do_install() {
    install -d ${D}${libdir}
    install -m 0755 ${B}/build-am62x/libiic_ota.so ${D}${libdir}/libiic_ota.so
    install -m 0644 ${B}/build-am62x/libiic_ota.a ${D}${libdir}/libiic_ota.a
    install -d ${D}${includedir}
    install -m 0644 ${B}/include/iic_ota.h ${D}${includedir}/iic_ota.h
}

# The library is unversioned and loaded by name (e.g. Python ctypes), so keep the
# .so in the main package instead of -dev.
FILES_SOLIBSDEV = ""
FILES:${PN} += "${libdir}/libiic_ota.so"
INSANE_SKIP:${PN} += "dev-so"
