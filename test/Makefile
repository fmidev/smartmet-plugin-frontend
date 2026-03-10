
include $(shell echo $${PREFIX-/usr})/share/smartmet/devel/makefile.inc

TEST_PREPARE_TARGETS := cnf/engines/geonames.conf cnf/engines/gis.conf
TEST_FINISH_TARGETS := dummy

TEST_DB_DIR := $(shell pwd)/tmp-geonames-db

GEONAMES_CONF_IN ?= cnf/engines/geonames.conf.in

ifdef CI
GEONAMES_HOST_EDIT := sed -e 's|"smartmet-test"|"$(TEST_DB_DIR)"|g'
TEST_PREPARE_TARGETS += start-geonames-db
TEST_FINISH_TARGETS += stop-test-db
else
GEONAMES_HOST_EDIT := cat
endif

TEST_TARGETS += \
	RunTests

LIBS += \
	-lsmartmet-macgyver

all: $(TEST_TARGETS)

test: all
	$(MAKE) $(TEST_PREPARE_TARGETS)
	./RunTests
	$(MAKE) $(TEST_FINISH_TARGETS)

clean:
	rm -rf obj
	rm -rf log
	rm -f cnf/geonames.conf
	rm -f cnf/gis.conf
	rm -rf $(TEST_DB_DIR)
	rm -f $(TEST_TARGETS)

RunTests: obj/RunTests.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

geonames-database:
	@-$(MAKE) stop-test-db
	rm -rf $(TEST_DB_DIR)
	mkdir -p $(TEST_DB_DIR) log
	if ! /usr/share/smartmet/test/db/create-local-db.sh $(TEST_DB_DIR) >log/tmp-geonames-db.log 2>&1 ; then \
		cat log/tmp-geonames-db.log; \
	false; \
		fi

start-geonames-db: geonames-database
	/usr/share/smartmet/test/db/test-db-ctl.sh $(TEST_DB_DIR) start -w

stop-test-db:
	-if test -d $(TEST_DB_DIR) ; then /usr/share/smartmet/test/db/test-db-ctl.sh $(TEST_DB_DIR) stop ; fi

cnf/engines/geonames.conf:
	$(GEONAMES_HOST_EDIT) <$(GEONAMES_CONF_IN) >$@

cnf/engines/gis.conf:
	$(GEONAMES_HOST_EDIT) $@.in >$@

dummy:

obj/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CFLAGS) $(INCLUDES) -c -MD -MF $(patsubst obj/%.o, obj/%.d, $@) -MT $@ -o $@ $<

.PHONY: cnf/engines/geonames.conf cnf/engines/gis.conf dummy
