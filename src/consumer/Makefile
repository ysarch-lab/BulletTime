default: consumer

# Variables
CPP_ARGS = -std=c++17 -O3 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-variable -Wno-unused-parameter
LIBS = -lpthread -lzstd -I.
FILES = consumer.cpp kernel_monitor.cpp cpu_monitor.cpp
consumer: consumer.cpp kernel_monitor.cpp cpu_monitor.cpp ../include/consumer.h
	g++ $(CPP_ARGS) -o consumer $(FILES) $(LIBS)

clean:
	rm -f consumer