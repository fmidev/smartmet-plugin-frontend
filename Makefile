SUBNAME = frontend
SPEC = smartmet-plugin-$(SUBNAME)
INCDIR = smartmet/plugins/$(SUBNAME)

REQUIRES = jsoncpp configpp gdal

include $(shell echo $${PREFIX-/usr})/share/smartmet/devel/makefile.inc

# Compiler options

DEFINES = -DUNIX -D_REENTRANT

LIBS += $(PREFIX_LDFLAGS) \
	-lsmartmet-spine \
	-lsmartmet-macgyver \
	-lsmartmet-timeseries \
	-lsmartmet-grid-files \
	$(REQUIRED_LIBS) \
	-lboost_thread

# What to install

LIBFILE = $(SUBNAME).so

# Compilation directories

vpath %.cpp $(SUBNAME)
vpath %.cpp $(SUBNAME)/info
vpath %.h $(SUBNAME)
vpath %.h $(SUBNAME)/info

# The files to be compiled

SRCS = $(wildcard $(SUBNAME)/*.cpp) $(wildcard $(SUBNAME)/info/*.cpp)
HDRS = $(wildcard $(SUBNAME)/*.h) $(wildcard $(SUBNAME)/info/*.h)
OBJS = $(patsubst %.cpp, obj/%.o, $(notdir $(SRCS)))

INCLUDES := -I$(SUBNAME) $(INCLUDES)

.PHONY: test rpm examples

# Detect jemalloc shared library for LD_PRELOAD environment variable
# Fall back to a common location if ldconfig is not available
# (LD_PRELOAD failure does not prevent service start)
JEMALLOC := $(shell ldconfig -p 2>/dev/null | grep libjemalloc\.so\. | head -n1 | awk '{print $$4}')
ifneq ($(JEMALLOC),)
	JEMALLOC_DIR := $(shell dirname $(JEMALLOC))
	JEMALLOC := $(shell readlink -f $(JEMALLOC_DIR))/$(shell basename $(JEMALLOC))
endif

# The rules

all: objdir $(LIBFILE) check
debug: all
release: all
profile: all

test:
	$(MAKE) -C test $@

check: $(LIBFILE)
	@echo "Running frontend plugin self-test"
	$(MAKE) -C testsuite check

$(LIBFILE): $(OBJS)
	$(CXX) $(LDFLAGS) -shared -rdynamic -o $(LIBFILE) $(OBJS) $(LIBS)

examples: $(OBJS)
	$(MAKE) -C examples all

clean:
	rm -f $(LIBFILE) *~ $(SUBNAME)/*~
	rm -rf obj
	$(MAKE) -C testsuite clean
	$(MAKE) -C examples clean
	$(MAKE) -C test clean

format:
	clang-format -i -style=file $(SUBNAME)/*.h $(SUBNAME)/*.cpp

install:
	@mkdir -p $(plugindir)
	$(INSTALL_PROG) $(LIBFILE) $(plugindir)/$(LIBFILE)
	@mkdir -p $(sysconfdir)/smartmet
	$(INSTALL_DATA) etc/smartmet-frontend.env $(sysconfdir)/smartmet/smartmet-frontend.env
	sed -e "s,@LD_PRELOAD@,LD_PRELOAD=$(JEMALLOC)," etc/smartmet-frontend.defaults.env.in > $(sysconfdir)/smartmet/smartmet-frontend.defaults.env
	@mkdir -p $(libdir)/../lib/systemd/system
	$(INSTALL_DATA) systemd/smartmet-frontend.service $(libdir)/../lib/systemd/system/

objdir:
	@mkdir -p $(objdir)

rpm: clean $(SPEC).spec
	rm -f $(SPEC).tar.gz # Clean a possible leftover from previous attempt
	tar -czvf $(SPEC).tar.gz --exclude test --exclude-vcs --transform "s,^,$(SPEC)/," *
	rpmbuild -tb $(SPEC).tar.gz
	rm -f $(SPEC).tar.gz

.SUFFIXES: $(SUFFIXES) .cpp

obj/%.o: %.cpp
	$(CXX) $(CFLAGS) $(INCLUDES) -c -MD -MF $(patsubst obj/%.o, obj/%.d, $@) -MT $@ -o $@ $<

-include $(wildcard obj/*.d)
