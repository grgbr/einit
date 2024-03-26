config-in           := Config.in
config-h            := tinit/config.h

common-cflags       := $(EXTRA_CFLAGS) -Wall -Wextra -Wformat=2 -D_GNU_SOURCE
common-ldflags      := $(common-cflags) $(EXTRA_LDFLAGS)

solibs              := libtinit.so
libtinit.so-objs     = lib.o conf.o strarr.o common.o
libtinit.so-cflags   = $(common-cflags) -DPIC -fpic
libtinit.so-ldflags  = $(common-ldflags) -shared -fpic -Wl,-soname,libtinit.so
libtinit.so-pkgconf  = libconfig libelog libutils libstroll

bins                := init
init-objs            = init.o mnt.o notif.o repo.o sigchan.o srv.o svc.o \
                       sys.o target.o log.o
init-cflags          = $(common-cflags)
init-ldflags         = $(EXTRA_LDFLAGS) -ltinit
init-pkgconf        := libelog libutils libstroll
init-path            = $(SBINDIR)/init

bins                += svctl
svctl-objs           = svctl.o
svctl-cflags         = $(common-cflags)
svctl-ldflags        = $(common-ldflags) -ltinit
svctl-path           = $(SBINDIR)/svctl
svctl-pkgconf        = smartcols

HEADERDIR           := $(CURDIR)/include
headers              = tinit/tinit.h

define libtinit_pkgconf_tmpl
prefix=$(PREFIX)
exec_prefix=$${prefix}
libdir=$${exec_prefix}/lib
includedir=$${prefix}/include

Name: libtinit
Description: Tinit service library
Version: $(VERSION)
Requires: libconfig libelog libutils libstroll
Cflags: -I$${includedir}
Libs: -L$${libdir} -ltinit
endef

pkgconfigs          := libtinit.pc
libtinit.pc-tmpl    := libtinit_pkgconf_tmpl
