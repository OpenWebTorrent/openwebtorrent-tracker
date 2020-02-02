.PHONY: deps build clean

deps:
	git submodule update --init --recursive

build:
	mkdir -p build && cd build && cmake .. && make

run:
	make build && ./build/openwebtorrent-tracker

clean:
	rm -rf build
