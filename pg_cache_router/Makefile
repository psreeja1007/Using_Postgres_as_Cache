MODULE_big = pg_cache_router
OBJS = pg_cache_router.o

EXTENSION = pg_cache_router
DATA = pg_cache_router--1.0.sql

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)