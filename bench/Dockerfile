# VTesse test bench: VTun 3.0.2 server with our patches, built like the target
# deployment (no OpenSSL -> XOR challenge response, no LZO, no zlib), plus a bridge
# segment with a DHCP server and a "remote device" behind a router.

FROM debian:bookworm-slim AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential bison flex curl ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# The one vtund build script of the repository: vtun sources plus our patches
# (server/patches), configured like the target deployment. Build context: repo root.
COPY server/ /src/server/
RUN /src/server/build-vtund.sh /out/opt/sbin/vtund

FROM debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        iproute2 bridge-utils dnsmasq-base isc-dhcp-client udhcpc iputils-ping \
        tcpdump curl python3 procps \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /out/ /
COPY bench/vtund.conf /etc/vtund.conf
COPY bench/entrypoint.sh /usr/local/bin/entrypoint.sh
COPY bench/device/ /opt/device/
COPY bench/client/ /opt/client/

EXPOSE 5000/tcp
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["server"]
