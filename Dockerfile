# Vega Language - Linux Build Container
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    gcc \
    make \
    libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /vega
COPY . .

RUN make clean && make release

# Output binaries are in /vega/bin/
