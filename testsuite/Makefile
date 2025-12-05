REQUIRES = jsoncpp

include $(shell smartbuildcfg --prefix)/share/smartmet/devel/makefile.inc

PROG = $(patsubst %.cpp,%,$(wildcard *Test.cpp))

LIBS += \
	$(PREFIX_LDFLAGS) \
	-lsmartmet-spine \
	-lsmartmet-macgyver \
	$(REQUIRED_LIBS)

all: $(PROG)

check: all
	@ok=true; for prog in $(PROG); do \
	  echo "Running test $$prog"; \
	  LD_PRELOAD=$(JEMALLOC) ./$$prog || ok=false; \
	done
	@$$ok

clean:
	rm -f $(PROG)
	rm -rf obj output


$(PROG) : % : obj/%.o $(foreach obj,$(EXTRA_OBJS),../obj/$(obj))
	$(CXX) $(CFLAGS) $(foreach obj,$(EXTRA_OBJS),../obj/$(obj)) $< $(LIBS) -o $@

obj/%.o: %.cpp
	@mkdir -p obj
	$(CXX) $(CFLAGS) $(INCLUDES) -c -MD -MF $(patsubst obj/%.o, obj/%.d, $@) -MT $@ -o $@ $<

EXTRA_OBJS =
QEngineInfoTest: EXTRA_OBJS += BackendInfoRec.o BackendInfoResponse.o BackendInfoFilter.o QEngineInfoRec.o
GridGenerationsInfoTest: EXTRA_OBJS += BackendInfoRec.o BackendInfoResponse.o BackendInfoFilter.o GridGenerationsInfoRec.o

-include $(wildcard obj/*.d)
