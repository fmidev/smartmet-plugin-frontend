REQUIRES = jsoncpp fmt

include $(shell smartbuildcfg --prefix)/share/smartmet/devel/makefile.inc

PROG = QEngineInfo

INCLUDES += -I../frontend/info

LIBS += \
	$(PREFIX_LDFLAGS) \
	-lsmartmet-spine \
	-lsmartmet-macgyver \
	$(REQUIRED_LIBS)

SPUTNIK := $(shell smartbuildcfg --prefix)/share/smartmet/engines/sputnik.so

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
	$(CXX) $(CFLAGS) $(foreach obj,$(EXTRA_OBJS),../obj/$(obj)) $(SPUTNIK) $< $(LIBS) -o $@

obj/%.o: %.cpp
	@mkdir -p obj
	$(CXX) $(CFLAGS) $(INCLUDES) -c -MD -MF $(patsubst obj/%.o, obj/%.d, $@) -MT $@ -o $@ $<

EXTRA_OBJS =
QEngineInfo: EXTRA_OBJS += BackendInfoRequests.o BackendInfoRec.o BackendInfoResponse.o BackendInfoFilter.o QEngineInfoRec.o GridGenerationsInfoRec.o

-include $(wildcard obj/*.d)
