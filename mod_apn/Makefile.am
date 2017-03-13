include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_apn

mod_LTLIBRARIES		= mod_apn.la
mod_apn_la_SOURCES	= mod_apn.c
mod_apn_la_CFLAGS	= $(AM_CFLAGS) -L$(libdir)/capn -I$(includedir)
mod_apn_la_LDFLAGS	= -avoid-version -module -no-undefined -shared -lcapn