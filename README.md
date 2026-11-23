# ServerStatus C client

A Linux client for [BotoX/ServerStatus](https://github.com/BotoX/ServerStatus). It connects to the ServerStatus TCP listener, authenticates, sends system statistics, and reconnects after a connection failure.

## Build

```sh
make
```

The default output is `status-client`. You can set `CC` and `CFLAGS` when building. To keep an existing service's executable name, for example `status-x`, run `make TARGET=status-x`.

## Tests

Run the C test suite with:

```sh
make test
```

Tests exercise the internal functions directly and use local sockets; no ServerStatus server or external network connection is needed.

## Configuration modes

Choose one of these modes.

### 1. JSON configuration

The JSON file contains every setting. Start the client with `--config`:

```sh
./status-client --config config.json
```

`config.json.example` shows the required fields:

```json
{
  "host": "status.example.net",
  "port": 35601,
  "username": "server_username",
  "password": "server_password",
  "interval": 1,
  "mountpoint": "/",
  "quiet": false
}
```

Copy the example to `config.json` and set permissions so only the service account can read the password:

```sh
cp config.json.example config.json
chmod 600 config.json
```

### 2. Command-line arguments

Pass the server and credentials directly as options. Interval, mountpoint, and quiet mode are also available:

```sh
./status-client --host status.example.net --port 35601 \
  --username server_username --password server_password \
  --interval 1 --mountpoint / --quiet
```

The password may be visible to other local users in the process list. Use JSON mode if that matters for your system.

### 3. Legacy mode

Existing installations (version =< 0.2.0) can replace the executable and keep the same invocation and `login.txt` format:

```sh
./status-x host port
```

The client reads `login.txt` from its current working directory. The file contains `username password` on one line; `login.txt.example` shows the format. The credentials file can also be selected explicitly with `-C FILE`. Optional `-i`, `-m`, and `-q` settings remain available.

## Options

* `-c, --config FILE` — use a complete JSON configuration
* `-H, --host HOST` — server hostname or address
* `-p, --port PORT` — ServerStatus TCP port
* `-u, --username USER` — account username
* `--password PASS` — account password
* `-C, --credentials FILE` — credentials file for legacy mode
* `-i, --interval SECONDS` — update interval from 0.1 to 3600 seconds (default: 1)
* `-m, --mountpoint PATH` — filesystem reported as HDD (default: `/`)
* `-q, --quiet` — suppress routine connection messages
* `-h, --help` — show help

Memory values use KiB, disk values use MiB, and network values are bytes per second. CPU usage comes from `/proc/stat`; network traffic is summed across interfaces except loopback and `tun*`. The client periodically checks IPv4 or IPv6 connectivity as requested by ServerStatus. It requires Linux procfs and standard POSIX socket and filesystem interfaces.

Stop the client with `Ctrl-C` or `SIGTERM`.
