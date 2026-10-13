# VTesse test bench: VTun 3.0.2 server built like the target deployment
# (no OpenSSL -> XOR challenge response, no LZO, no zlib), plus a bridge
# segment with a DHCP server and a "remote device" behind a router.

FROM debian:bookworm-slim AS build

ARG VTUN_VERSION=3.0.2
ARG VTUN_SHA256=db26d4dbc35a2b3d65519f68b8ae55c80d7ec4e6569aeec7a432aa82f24c56d6

RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential bison flex curl ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
RUN curl -fsSL -o vtun.tar.gz "http://prdownloads.sourceforge.net/vtun/vtun-${VTUN_VERSION}.tar.gz" \
 && echo "${VTUN_SHA256}  vtun.tar.gz" | sha256sum -c - \
 && tar xzf vtun.tar.gz

WORKDIR /src/vtun-${VTUN_VERSION}
# -fcommon: the 2008 sources define globals in headers (modern GCC defaults to -fno-common).
RUN CFLAGS="-O2 -fcommon" ./configure --build=x86_64-linux-gnu \
        --disable-lzo --disable-zlib --disable-ssl --prefix='' \
 && make \
 && strip vtund \
 && install -D -m 0755 vtund /out/opt/sbin/vtund

FROM debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        iproute2 bridge-utils dnsmasq-base isc-dhcp-client udhcpc iputils-ping \
        tcpdump curl python3 procps \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /out/ /
COPY vtund.conf /etc/vtund.conf
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
COPY device/ /opt/device/
COPY client/ /opt/client/

EXPOSE 5000/tcp
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["server"]
