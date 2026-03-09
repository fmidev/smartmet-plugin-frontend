
include $(shell echo $${PREFIX-/usr})/share/smartmet/devel/makefile.inc

TEST_PREPARE_TARGETS := cnf/geonames.conf cnf/gis.conf
TEST_FINISH_TARGETS := dummy

TEST_DB_DIR := $(shell pwd)/tmp-geonames-db

GEONAMES_CONF_IN ?= cnf/geonames.conf.in

ifdef CI
GEONAMES_HOST_EDIT := sed -e 's|"smartmet-test"|"$(TEST_DB_DIR)"|g'
TEST_PREPARE_TARGETS += start-geonames-db
TEST_FINISH_TARGETS += stop-test-db
EXTRA_IGNORE := input/.testignore_circle-ci
else
GEONAMES_HOST_EDIT := cat
EXTRA_IGNORE :=
endif

TEST_TARGETS += \
	RunTests

LIBS += \
	-lsmartmet-macgyver

all: $(TEST_TARGETS)

test: all
	./RunTests






clean:
	rm -rf obj
	rm -f cnf/geonames.conf
	rm -f cnf/gis.conf
	rm -rf $(TEST_DB_DIR)
	rm -f $(TEST_TARGETS)

RunTests: obj/RunTests.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

start-geonames-db: geonames-database
	/usr/share/smartmet/test/db/test-db-ctl.sh $(TEST_DB_DIR) start -w

stop-test-db:
	-/usr/share/smartmet/test/db/test-db-ctl.sh $(TEST_DB_DIR) stop

cnf/geonames.conf:
	$(GEONAMES_HOST_EDIT) <$(GEONAMES_CONF_IN) >$@

cnf/gis.conf:
	$(GEONAMES_HOST_EDIT) $@.in >$@

dummy:

obj/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CFLAGS) $(INCLUDES) -c -MD -MF $(patsubst obj/%.o, obj/%.d, $@) -MT $@ -o $@ $<

.PHONY: cnf/geonames.conf cnf/gis.conf dummy
