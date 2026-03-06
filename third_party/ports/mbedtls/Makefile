# mbedtls port
# C crypto library - source files are copied directly, no build needed

PORTNAME=	mbedtls
DISTNAME=	mbedtls
VERSION=	3.1.0
DISTURL=	https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v${VERSION}.tar.gz

SRCDIR=	mbedtls-${VERSION}

BUILD_COMMANDS=	cp ../../../ports/mbedtls/CMakeLists.txt.custom ../../../mbedtls/CMakeLists.txt && python3 ${SCRIPT_DIR}/scripts/format_c_to_cpp.py ${WRKSRC}

.include "${.CURDIR}/../bsd.port.mk"
