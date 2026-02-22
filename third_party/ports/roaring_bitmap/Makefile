# RoaringBitmap port
# CRoaring library - Fast bitmap data structure

PORTNAME=	roaring_bitmap
DISTNAME=	CRoaring
VERSION=	4.5.1
DISTURL=	https://github.com/RoaringBitmap/CRoaring/archive/refs/tags/v${VERSION}.tar.gz

SRCDIR=	CRoaring-${VERSION}

# Build command: run amalgamation to create single-header files
# Also replace CMakeLists.txt with our simplified version
BUILD_COMMANDS=	./amalgamation.sh && cp ../../../ports/roaring_bitmap/CMakeLists.txt.custom CMakeLists.txt

# Files to sync (what we track in third_party)
# The amalgamation script creates roaring.c, roaring.h, roaring.hh

.include "${.CURDIR}/../bsd.port.mk"
