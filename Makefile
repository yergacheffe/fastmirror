# Makefile for fswebcam
# [25/12/2009]

SHELL = /bin/sh

prefix      = /usr/local
exec_prefix = ${prefix}
bindir      = ${exec_prefix}/bin
mandir      = ${datarootdir}/man
datarootdir = ${prefix}/share

CC      = gcc
CFLAGS  =  -g -O2 -DHAVE_CONFIG_H -I/opt/vc/include -I/opt/vc/include/interface/vmcs_host/linux -I/opt/vc/include/interface/vcos/pthreads
LDFLAGS = -lgd -L/opt/vc/lib -lGLESv2 -lEGL -lbcm_host -lpthread  -ljpeg

OBJS = fastmirror.o ../openvg/openvg-master/libshapes.o ../openvg/openvg-master/oglinit.o

all: fastmirror

fastmirror: $(OBJS)
	$(CC) -o fastmirror $(OBJS) $(LDFLAGS)

.c.o:
	${CC} ${CFLAGS} -c $< -o $@


clean:
	rm -f core* *.o fastmirror

distclean: clean
	rm -rf config.h *.cache config.log config.status Makefile *.jp*g *.png *~

