# Host-Testbuild (Linux). make -> ./dashboard_host
FW  := ../firmware/DruckerDashboardRP2350-MK1
LIB := ../firmware/libraries
MB  := $(LIB)/mbedtls_dd/src
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wno-unused-function -I$(FW) -I$(MB) -I$(LIB)/ArduinoJson/src -I.
CFLAGS := -O2 -I$(MB) -I$(MB)/library
APP_SRC := $(filter-out $(FW)/plat_arduino.cpp,$(wildcard $(FW)/*.cpp))
MB_SRC := $(wildcard $(MB)/library/*.c)
MB_OBJ := $(patsubst $(MB)/library/%.c,obj/mb/%.o,$(MB_SRC))
APP_OBJ := $(patsubst $(FW)/%.cpp,obj/app/%.o,$(APP_SRC)) obj/app/plat_posix.o obj/app/main.o
dashboard_host: $(APP_OBJ) obj/libmbedtls.a
	$(CXX) -o $@ $(APP_OBJ) obj/libmbedtls.a -lpthread
obj/libmbedtls.a: $(MB_OBJ)
	ar rcs $@ $^
obj/mb/%.o: $(MB)/library/%.c $(MB)/mbedtls/mbedtls_config.h
	@mkdir -p obj/mb
	$(CC) $(CFLAGS) -c $< -o $@
obj/app/%.o: $(FW)/%.cpp $(wildcard $(FW)/*.h)
	@mkdir -p obj/app
	$(CXX) $(CXXFLAGS) -c $< -o $@
obj/app/%.o: %.cpp $(wildcard $(FW)/*.h)
	@mkdir -p obj/app
	$(CXX) $(CXXFLAGS) -c $< -o $@
clean:
	rm -rf obj dashboard_host
