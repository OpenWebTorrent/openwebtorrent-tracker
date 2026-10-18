.PHONY: deps build test clean

deps:
	git submodule update --init
	git -C deps/uWebSockets submodule update --init uSockets

build:
	mkdir -p build && cd build && cmake .. && make

test:
	mkdir -p build && cd build && cmake -DOWT_BUILD_TESTS=ON .. && make && ctest --output-on-failure

run:
	make build && ./build/openwebtorrent-tracker

clean:
	rm -rf build
