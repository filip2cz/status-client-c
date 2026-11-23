# ServerStatus C client

A small Linux client for [BotoX/ServerStatus](https://github.com/BotoX/ServerStatus). It connects to the ServerStatus TCP listener, authenticates, and sends system statistics as newline-delimited updates. The client reconnects after network or server failures.

## Build

```sh
make
```

This builds `status-client` with a C compiler. To choose another compiler or flags, set `CC` or `CFLAGS` when running `make`.

## Configure credentials

Create `login.txt` in the current directory, or point to a different file with `-c`:

```text
server_username server_password
```

`login.txt.example` shows the format. Keep the real file private:

```sh
cp login.txt.example login.txt
chmod 600 login.txt
```

The credentials file is ignored by Git.

## Run

```sh
./status-client [options] host port
```

For example:

```sh
./status-client -c /etc/serverstatus/login.txt -i 1 status.example.net 35601
```

Options:

* `-c, --credentials FILE` — credentials file (default: `login.txt`)
* `-i, --interval SECONDS` — update interval from 0.1 to 3600 seconds (default: 1)
* `-m, --mountpoint PATH` — filesystem reported as HDD (default: `/`)
* `-q, --quiet` — suppress routine connection messages
* `-h, --help` — show help

Memory values use KiB, disk values use MiB, and network values are bytes per second. CPU usage is calculated from `/proc/stat`; traffic is summed across interfaces except loopback and `tun*`. The client periodically checks IPv4 or IPv6 connectivity, as requested by the ServerStatus server. It requires Linux procfs and the standard POSIX socket and filesystem interfaces.

Stop the client with `Ctrl-C` or `SIGTERM`.
