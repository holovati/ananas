ARCH?=		i386
TARGET=		${ARCH}-elf-ananas
PREFIX?=	/usr
DESTDIR?=	$(realpath ../output.${ARCH})

VERSION?=	3.1.2
MIRROR?=	http://www.mpfr.org/mpfr-current/

# tools to use
DOWNLOAD?=	wget
TAR?=		tar
TOUCH?=		touch

R=		${CURDIR}
CC=		${R}/../../toolchain/prefix.${ARCH}/bin/${TARGET}-gcc
CPP=		${R}/../../toolchain/prefix.${ARCH}/bin/${TARGET}-cpp
AR=		${R}/../../toolchain/prefix.${ARCH}/bin/${TARGET}-ar

mpfr:		.install

../distfiles/mpfr-${VERSION}.tar.gz:
		(cd ../distfiles && ${DOWNLOAD} ${MIRROR}/mpfr-${VERSION}.tar.gz)

.extract:	../distfiles/mpfr-${VERSION}.tar.gz
		${TAR} xf ../distfiles/mpfr-${VERSION}.tar.gz
		@${TOUCH} .extract

.patch:		.extract mpfr.ananas.diff
		(cd mpfr-${VERSION} && patch -p1 < ${R}/mpfr.ananas.diff)
		@${TOUCH} .patch

.configure:	.patch
		(cd mpfr-${VERSION} && ./configure --host=${TARGET} --prefix=${DESTDIR}${PREFIX} --with-gmp=${DESTDIR}${PREFIX})
		@${TOUCH} .configure

.build:		.configure
		(cd mpfr-${VERSION} && make)
		@${TOUCH} .build

.install:	.build
		(cd mpfr-${VERSION} && make install)
		mkdir -p ${R}/../../toolchain/prefix.${ARCH}/${ARCH}-elf-ananas/include
		mkdir -p ${R}/../../toolchain/prefix.${ARCH}/${ARCH}-elf-ananas/lib
		cp -R ${DESTDIR}${PREFIX}/include/* ${R}/../../toolchain/prefix.${ARCH}/${ARCH}-elf-ananas/include
		cp -R ${DESTDIR}${PREFIX}/lib/* ${R}/../../toolchain/prefix.${ARCH}/${ARCH}-elf-ananas/lib
		${TOUCH} .install

clean:
		rm -rf mpfr-${VERSION}
		rm -f .extract .patch .configure .build
