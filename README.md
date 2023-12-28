# IN DEVELOPMENT
# status C client for Linux

[![wakatime](https://wakatime.com/badge/github/filip2cz/status-client-c.svg)](https://wakatime.com/badge/github/filip2cz/status-client-c)

Original Python2 client of https://github.com/BotoX/ServerStatus compiled into C.

It was created for devices that can run Linux but can't run python or more complex bash scripts, making official clients unusable on them.

## How to build
1. edit first lines of `client.py`, to your need config
2. install Nuitka by `pip install nuitka` command
3. compile it into c with python -m nuitka client.py
