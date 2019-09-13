include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_dialer

mod_LTLIBRARIES = mod_dialer.la
mod_skel_la_SOURCES  = mod_dialer.c
mod_skel_la_CFLAGS   = $(AM_CFLAGS)
mod_skel_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_skel_la_LDFLAGS  = -avoid-version -module -no-undefined -shared
