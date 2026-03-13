
REQUIRES = configpp

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
	RunTests \
	test_plugin/denytest.so

LIBS += \
	$(PREFIX_LDFLAGS) \
	-lsmartmet-spine \
	-lsmartmet-macgyver \
	$(REQUIRED_LIBS)

all: $(TEST_TARGETS)

test: all
	rm -rf log failures
	$(MAKE) prepare-tests
	$(MAKE) $(TEST_PREPARE_TARGETS)
	ok=false; ./RunTests && ok=true; \
	$(MAKE) $(TEST_FINISH_TARGETS); \
	$$ok

clean:
	rm -rf obj log failures
	rm -f cnf/geonames.conf cnf/gis.conf
	rm -rf $(TEST_DB_DIR)
	rm -f $(TEST_TARGETS)
	rm -rf test_plugin/obj
	rm -f test_plugin/denytest.so
	rm -rf input/deny output/deny

RunTests: obj/RunTests.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

test_plugin/denytest.so: test_plugin/Plugin.cpp
	mkdir -p test_plugin/obj
	$(CXX) $(CFLAGS) $(INCLUDES) -fPIC -shared -o $@ $< $(LIBS)

prepare-tests:
	@rm -rf input/deny output/deny
	@mkdir -p input/deny output/deny
	@for i in {1..10} ; do \
		cp -p input/denytest_retry.get.in input/deny/attempt$${i}.get; \
		cp -p output/denytest_retry.get.in output/deny/attempt$${i}.get; \
	done

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

.PHONY: cnf/engines/geonames.conf cnf/engines/gis.conf dummy prepare-tests
