PROJECT_ROOT = $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

TARGET := jackmon
OBJS = $(TARGET).o utils.o audio.o
LIBS += -ljack -lm -pthread

ifeq ($(BUILD_MODE),debug)
	CFLAGS += -g -O0
else ifeq ($(BUILD_MODE),profile)
	CFLAGS += -g -pg -fprofile-arcs -ftest-coverage
	LDFLAGS += -pg -fprofile-arcs -ftest-coverage
	EXTRA_CLEAN += $(TARGET).gcda $(TARGET).gcno $(PROJECT_ROOT)gmon.out
	EXTRA_CMDS = rm -rf $(TARGET).gcda
else
    CFLAGS += -O2
endif

all:	clean $(TARGET)

$(TARGET):	$(OBJS)
	$(CC) -o $@ $^ $(LIBS)
	$(EXTRA_CMDS)

%.o:	$(PROJECT_ROOT)%.cpp
	$(CXX) -c $(CFLAGS) $(CXXFLAGS) $(CPPFLAGS) -o $@ $<

%.o:	$(PROJECT_ROOT)%.c
	$(CC) -c $(CFLAGS) $(CPPFLAGS) -o $@ $<

clean:
	rm -fr $(TARGET) $(OBJS) $(EXTRA_CLEAN)

install: $(TARGET)
	sudo mkdir -p /etc/$(TARGET).d
	sudo cp -r install/* /
	sudo cp $(TARGET) /usr/sbin/