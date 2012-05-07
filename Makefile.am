
bin_PROGRAMS = dapper
nodist_man_MANS = dapper.1
dist_doc_DATA = AUTHORS COPYING HISTORY README.md

dist-hook:
	cp $(srcdir)/dapper.pod $(distdir)/

AM_CFLAGS = -g -std=c99 \
		-Wall -Wextra -pedantic -Wshadow -Wpointer-arith -Wcast-align \
		-Wwrite-strings -Wmissing-prototypes -Wmissing-declarations \
		-Wredundant-decls -Wnested-externs -Winline -Wno-long-long \
		-Wuninitialized -Wconversion -Wstrict-prototypes
AM_CFLAGS += -D_BSD_SOURCE

dapper_SOURCES = main.c

dapper.1: dapper.pod
	pod2man --center="Desktop Applications Autostarter" --section=1 --release=$(PACKAGE_VERSION) dapper.pod dapper.1

