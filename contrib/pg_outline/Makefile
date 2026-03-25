# contrib/pg_outline/Makefile

MODULE_big = pg_outline
OBJS = pg_outline.o $(WIN32RES)

EXTENSION = pg_outline
DATA = pg_outline--1.0.sql
PGFILEDESC = "pg_outline - plan stabilization through execution plan outline hints"

REGRESS = pg_outline

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_outline
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
