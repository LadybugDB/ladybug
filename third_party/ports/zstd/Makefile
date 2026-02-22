# Zstd port
# Zstandard - Fast lossless compression algorithm

PORTNAME=	zstd
DISTNAME=	zstd
VERSION=	1.5.7
DISTURL=	https://github.com/facebook/zstd/archive/refs/tags/v${VERSION}.tar.gz

SRCDIR=	zstd-${VERSION}

# Apply patches: rename .c to .cpp, add namespace wrapper, remove assembly
BUILD_COMMANDS=	python3 ../../../ports/zstd/do-patch.py .

.include "${.CURDIR}/../bsd.port.mk"
