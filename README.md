# IN DEVELOPMENT
# status C client for Linux

[![wakatime](https://wakatime.com/badge/github/filip2cz/status-client-c.svg)](https://wakatime.com/badge/github/filip2cz/status-client-c)

Implementation of https://github.com/BotoX/ServerStatus client in C

It was created for devices that can run Linux but can't run python or more complex bash scripts, making official clients unusable on them.

## Usage

1) into login.txt file put username and password on one line:

```username password```

2) run it by `./status-x host port`

## How to compile
gcc status.c -o status
