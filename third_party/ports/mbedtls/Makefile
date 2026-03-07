# mbedtls port
# C crypto library - source files are copied directly, no build needed

PORTNAME=	mbedtls
DISTNAME=	mbedtls
VERSION=	3.6.5
DISTURL=	https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v${VERSION}.tar.gz

SRCDIR=	mbedtls-${VERSION}

PRE_PATCH_COMMANDS=	python3 ${SCRIPT_DIR}/scripts/format_c_to_cpp.py ${WRKSRC}

BUILD_COMMANDS=	cp ../../../ports/mbedtls/CMakeLists.txt.custom ../../../mbedtls/CMakeLists.txt

.include "${.CURDIR}/../bsd.port.mk"
