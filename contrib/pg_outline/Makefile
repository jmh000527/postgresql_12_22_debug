# pg_outline extension SQL shim.
# The executable code lives in the pg_hint_plan shared library; this Makefile
# only installs the SQL/control files.

EXTENSION = pg_outline
DATA = pg_outline--1.0.sql
PGFILEDESC = "pg_outline - outline/hint injection using pg_hint_plan syntax"

ifdef NO_PGXS
subdir = contrib/pg_outline
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
else
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
endif
