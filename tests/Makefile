include Makefile.sources

SERIAL_TARGET_NAME = gearboy-serial-tests
LINK_CABLE_WIRE_TARGET_NAME = gearboy-link-cable-wire-tests
LINK_CABLE_MANAGER_TARGET_NAME = gearboy-link-cable-manager-tests
EVERDRIVE_RTC_TARGET_NAME = gearboy-everdrive-rtc-tests
EVERDRIVE_RTC_TIMING_TARGETS = gearboy-everdrive-rtc-timing gearboy-everdrive-rtc-timing-performance
GIT_VERSION := $(shell git describe --abbrev=7 --dirty --always --tags)
UNAME_S := $(shell uname -s)
PLATFORM = "undefined"

OBJECTS += $(SOURCES_C:.c=.o) $(SOURCES_CXX:.cpp=.o)

INCLUDES += -I$(SRC_DIR)
INCLUDES += -I$(DEPS_DIR)/miniz
INCLUDES += -I../platforms/shared/desktop

USE_CLANG ?= 0
ifeq ($(USE_CLANG), 1)
    CXX = clang++
    CC = clang
else
    CXX = g++
    CC = gcc
endif

CPPFLAGS += $(INCLUDES)
CPPFLAGS += -Wall -Wextra -Wformat -fno-exceptions -DGEARBOY_DISABLE_DISASSEMBLER=1
CPPFLAGS += -DGEARBOY_DISABLE_VGMRECORDER=1 -DEMULATOR_BUILD=\"$(GIT_VERSION)\"
CXXFLAGS += -std=c++11
CFLAGS += -std=c99

$(DEPS_DIR)/%.o: CPPFLAGS += -w

DEBUG ?= 0
ifeq ($(DEBUG), 1)
    CPPFLAGS += -DDEBUG -g3
else
    CPPFLAGS += -DNDEBUG -O3 -flto=auto
    LDFLAGS += -O3 -flto=auto
endif

SANITIZE ?= 0
ifeq ($(SANITIZE), 1)
    CPPFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all
    LDFLAGS += -fsanitize=address,undefined
endif

ifeq ($(UNAME_S), Linux)
    PLATFORM = "Linux"
    LDFLAGS += -latomic
else ifeq ($(UNAME_S), Darwin)
    PLATFORM = "macOS"
    CPPFLAGS += -I/opt/local/include
else
    PLATFORM = "Generic Unix-like/BSD"
    CXXFLAGS += -std=gnu++11
endif

all: serial link-cable-wire link-cable-manager everdrive

serial: $(SERIAL_TARGET_NAME)
	@echo Serial tests build complete for $(PLATFORM)

link-cable-wire: $(LINK_CABLE_WIRE_TARGET_NAME)
	@echo Link cable wire tests build complete for $(PLATFORM)

link-cable-manager: $(LINK_CABLE_MANAGER_TARGET_NAME)
	@echo Link cable manager tests build complete for $(PLATFORM)

$(SERIAL_TARGET_NAME): serial_tests.cpp $(OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(LINK_CABLE_WIRE_TARGET_NAME): link_cable_wire_tests.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I../platforms/shared/desktop -pthread -o $@ $^ $(LDFLAGS)

$(LINK_CABLE_MANAGER_TARGET_NAME): link_cable_manager_tests.cpp ../platforms/shared/desktop/link_cable/link_cable_manager.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I../platforms/shared/desktop -pthread -o $@ $^ $(LDFLAGS)

$(LINK_CABLE_MANAGER_TARGET_NAME): CPPFLAGS += -DLINK_CABLE_SHM_PREFIX=\"gearboy-link-tests-\"

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(SERIAL_TARGET_NAME) $(LINK_CABLE_WIRE_TARGET_NAME) $(LINK_CABLE_MANAGER_TARGET_NAME) gearboy-everdrive-tests $(EVERDRIVE_RTC_TARGET_NAME) $(EVERDRIVE_RTC_TIMING_TARGETS) gearboy-igkb-tests
	rm -rf $(SERIAL_TARGET_NAME).dSYM $(LINK_CABLE_WIRE_TARGET_NAME).dSYM $(LINK_CABLE_MANAGER_TARGET_NAME).dSYM

# Compile these small independent components directly, without reusing the
# desktop core objects (which have different feature flags).
.PHONY: everdrive
everdrive: gearboy-everdrive-tests $(EVERDRIVE_RTC_TARGET_NAME)
	./gearboy-everdrive-tests
	./$(EVERDRIVE_RTC_TARGET_NAME)

gearboy-everdrive-tests: $(SRC_DIR)/EverdriveX.h $(SRC_DIR)/VirtualFat.h $(SRC_DIR)/Cartridge.h everdrive_tests.cpp $(SRC_DIR)/Cartridge.cpp $(SRC_DIR)/EverdriveX.cpp $(SRC_DIR)/VirtualFat.cpp $(DEPS_DIR)/miniz/miniz.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(filter %.cpp %.o,$^) $(LDFLAGS)

$(EVERDRIVE_RTC_TARGET_NAME): everdrive_rtc_tests.cpp \
		$(SRC_DIR)/definitions.h $(SRC_DIR)/EverdriveOSMemoryRule.h \
		$(SRC_DIR)/EverdriveX.h $(SRC_DIR)/VirtualFat.h $(SRC_DIR)/Cartridge.h \
		$(SRC_DIR)/EverdriveOSMemoryRule.cpp $(SRC_DIR)/MemoryRule.cpp \
		$(SRC_DIR)/Cartridge.cpp $(SRC_DIR)/EverdriveX.cpp $(SRC_DIR)/VirtualFat.cpp \
		$(DEPS_DIR)/miniz/miniz.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(filter %.cpp %.o,$^) $(LDFLAGS)

# Run the same ROMs through normal and batched core dispatch. Build separately
# from the desktop and serial test objects, whose compile flags may differ.
.PHONY: everdrive-rtc-timing
everdrive-rtc-timing: $(EVERDRIVE_RTC_TIMING_TARGETS)
	./gearboy-everdrive-rtc-timing
	./gearboy-everdrive-rtc-timing-performance

gearboy-everdrive-rtc-timing: RTC_TIMING_FLAGS = -UGEARBOY_DISABLE_DISASSEMBLER
gearboy-everdrive-rtc-timing-performance: RTC_TIMING_FLAGS = -DPERFORMANCE
$(EVERDRIVE_RTC_TIMING_TARGETS): everdrive_rtc_timing_tests.cpp $(SOURCES_CXX) \
		$(addprefix $(SRC_DIR)/,EverdriveOSMemoryRule.cpp EverdriveX.cpp VirtualFat.cpp PS2Keyboard.cpp) \
		$(wildcard $(SRC_DIR)/*.h $(SRC_DIR)/audio/*.h) $(DEPS_DIR)/miniz/miniz.o
	$(CXX) $(CPPFLAGS) $(RTC_TIMING_FLAGS) $(CXXFLAGS) $(shell pkg-config --cflags sdl3) \
		-o $@ $(filter %.cpp %.o,$^) $(LDFLAGS)

.PHONY: igkb
igkb: gearboy-igkb-tests
	./gearboy-igkb-tests
gearboy-igkb-tests: igkb_tests.cpp $(SRC_DIR)/IGKeyboard.cpp $(SRC_DIR)/IGKeyboard.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(filter %.cpp,$^) $(LDFLAGS)
