PROJECT_ROOT = $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

OBJS = jackmon.o utils.o audio.o
LIBS += -ljack -lm -pthread

ifeq ($(BUILD_MODE),debug)
	CFLAGS += -g -O0
else ifeq ($(BUILD_MODE),profile)
	CFLAGS += -g -pg -fprofile-arcs -ftest-coverage
	LDFLAGS += -pg -fprofile-arcs -ftest-coverage
	EXTRA_CLEAN += jackmon.gcda jackmon.gcno $(PROJECT_ROOT)gmon.out
	EXTRA_CMDS = rm -rf jackmon.gcda
else
    CFLAGS += -O2
endif

all:	clean jackmon

jackmon:	$(OBJS)
	$(CC) -o $@ $^ $(LIBS)
	$(EXTRA_CMDS)

%.o:	$(PROJECT_ROOT)%.cpp
	$(CXX) -c $(CFLAGS) $(CXXFLAGS) $(CPPFLAGS) -o $@ $<

%.o:	$(PROJECT_ROOT)%.c
	$(CC) -c $(CFLAGS) $(CPPFLAGS) -o $@ $<

clean:
	rm -fr jackmon $(OBJS) $(EXTRA_CLEAN)
