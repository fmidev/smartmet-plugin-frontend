
REQUIRES = configpp

include $(shell echo $${PREFIX-/usr})/share/smartmet/devel/makefile.inc

TEST_PREPARE_TARGETS := cnf/engines/geonames.conf cnf/engines/gis.conf cnf/engines/avi.conf
TEST_FINISH_TARGETS := dummy

TEST_DB_DIR := $(shell pwd)/tmp-geonames-db

ifdef CI
DB_HOST_EDIT := sed -e 's|"smartmet-test"|"$(TEST_DB_DIR)"|g'
TEST_PREPARE_TARGETS += start-geonames-db
TEST_FINISH_TARGETS += stop-test-db
else
DB_HOST_EDIT := cat
endif

TEST_TARGETS += \
	RunTests \
	RunClusterTests \
	RunLoadTest \
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
	ok=true; \
	./RunTests || ok=false; \
	./RunClusterTests || ok=false; \
	$(MAKE) $(TEST_FINISH_TARGETS); \
	$$ok

# Regression tests for cluster behaviour: connection reuse across a backend
# leaving and rejoining, the backend timeout, and what a client gets when a
# backend dies. "make test" runs these as well; this target is for iterating on
# them without waiting for the file-driven tests first.
cluster-test: all
	rm -rf log/cluster failures
	$(MAKE) $(TEST_PREPARE_TARGETS)
	ok=false; ./RunClusterTests && ok=true; \
	$(MAKE) $(TEST_FINISH_TARGETS); \
	$$ok

# Load the frontend while perf records it. Not a test - nothing here passes or
# fails - so it is not part of "make test" and never will be. Pass options
# through, e.g.:
#
#   make load-test LOAD_ARGS="--seconds=60 --threads=16"
#   make load-test LOAD_ARGS="--target=/timeseries?places=Helsinki&param=name,time"
load-test: all
	$(MAKE) $(TEST_PREPARE_TARGETS)
	ok=false; ./RunLoadTest $(LOAD_ARGS) && ok=true; \
	$(MAKE) $(TEST_FINISH_TARGETS); \
	$$ok

debug-backend1: all
	$(MAKE) $(TEST_PREPARE_TARGETS)
	./RunTests --debug-backend 1; \
	$(MAKE) $(TEST_FINISH_TARGETS); \
	true

debug-backend2: all
	$(MAKE) $(TEST_PREPARE_TARGETS)
	./RunTests --debug-backend 2; \
	$(MAKE) $(TEST_FINISH_TARGETS); \
	true

debug-frontend: all
	$(MAKE) $(TEST_PREPARE_TARGETS)
	./RunTests --debug-frontend; \
	$(MAKE) $(TEST_FINISH_TARGETS); \
	true

clean:
	rm -rf obj log failures
	rm -f cnf/geonames.conf cnf/gis.conf
	rm -rf $(TEST_DB_DIR)
	rm -f $(TEST_TARGETS)
	rm -rf test_plugin/obj
	rm -f test_plugin/denytest.so
	rm -rf input/deny output/deny

RunTests: obj/RunTests.o obj/TestHarness.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

RunClusterTests: obj/RunClusterTests.o obj/TestHarness.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

RunLoadTest: obj/RunLoadTest.o obj/TestHarness.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS) -lpthread

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
	$(DB_HOST_EDIT) <$@.in >$@

cnf/engines/gis.conf:
	$(DB_HOST_EDIT) <$@.in >$@

cnf/engines/avi.conf:
	$(DB_HOST_EDIT) <$@.in >$@

dummy:

obj/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CFLAGS) $(INCLUDES) -c -MD -MF $(patsubst obj/%.o, obj/%.d, $@) -MT $@ -o $@ $<

# The .d files have always been generated here but never used, which went
# unnoticed while every source in this directory was self-contained. Now that
# they share TestHarness.h, editing it has to rebuild both programs.
-include $(wildcard obj/*.d)

.PHONY: cnf/engines/geonames.conf cnf/engines/gis.conf cnf/engines/avi.conf dummy prepare-tests
