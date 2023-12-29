# status C client for Linux

Original client of https://github.com/BotoX/ServerStatus compiled into C with Nuitka.

Changes in original client: https://github.com/filip2cz/status-client-c/commit/84a4d6b5ee3e50abaa3b0ee80d0ee887e05fadd6

## Usage
```bash
./client.bin hostname port user password
```

for example:
```bash
./client.bin status.botox.bz 35601 s01 some-hard-to-guess-copy-paste-password
```

## How to build
This tutorial is written for Ubuntu 22.04 LTS

1. Install python2 and python2-dev
```bash
sudo apt update
sudo apt install python2 python2-dev
```

2. Install pip for python2
```bash
curl https://bootstrap.pypa.io/pip/2.7/get-pip.py --output get-pip.py
sudo python2 get-pip.py
```

3. Install Nuitka
```bash
python2 -m pip install nuitka
```

4. Compile program with Nuitka
```bash
python2 -m nuitka --standalone client.py
```

`client.bin` is now in `client.dist` directory.
