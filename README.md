# OpenWebtorrent Tracker

Fast and simple webtorrent tracker written in C++.

## Requirements
- C++ 17
- OpenSSL 1.1+
- ZLib

```sh
# Install OpenSSL (1.1.1)
wget https://www.openssl.org/source/openssl-1.1.1h.tar.gz
tar xvf openssl-1.1.1h.tar.gz
cd openssl-1.1.1h
./config
make
make install

# Install ZLib
sudo apt-get install zlib1g-dev
```

## Build

First, install dependencies:

```sh
make deps
```

Then, build the project:

```sh
make build
```

## Usage

You can run it with the command below:

```sh
./build/openwebtorrent-tracker \
	--port [PORT]
	--ssl-cert [PATH]
	--ssl-key [PATH]
```

## License

MIT. Copyright (c) [Alex](https://github.com/alxhotel)
