# ANTLR4 C++ runtime port
# Sources are copied directly; patches/ holds Ladybug's changes to them

PORTNAME=	antlr4_runtime
DISTNAME=	antlr4
VERSION=	4.13.1
DISTURL=	https://github.com/antlr/antlr4/archive/refs/tags/${VERSION}.tar.gz

SRCDIR=	antlr4-${VERSION}/runtime/Cpp/runtime

# Keep Ladybug's CMakeLists.txt instead of the runtime's own
BUILD_COMMANDS=	cp ${SCRIPT_DIR}/antlr4_runtime/CMakeLists.txt CMakeLists.txt

.include "${.CURDIR}/../bsd.port.mk"
