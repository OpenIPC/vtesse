# libvtesse.so for the Android app: the VTun client, libnetcore (lwIP stack,
# local proxy) and the JNI bridge, built from the repository sources.
cmake_minimum_required(VERSION 3.22.1)
project(vtesse C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
set(LWIP ${ROOT}/third_party/lwip/src)

set(LWIP_SRC
    ${LWIP}/core/def.c ${LWIP}/core/inet_chksum.c ${LWIP}/core/init.c ${LWIP}/core/ip.c
    ${LWIP}/core/mem.c ${LWIP}/core/memp.c ${LWIP}/core/netif.c ${LWIP}/core/pbuf.c
    ${LWIP}/core/stats.c ${LWIP}/core/sys.c ${LWIP}/core/tcp.c ${LWIP}/core/tcp_in.c
    ${LWIP}/core/tcp_out.c ${LWIP}/core/timeouts.c ${LWIP}/core/udp.c
    ${LWIP}/core/ipv4/dhcp.c ${LWIP}/core/ipv4/etharp.c ${LWIP}/core/ipv4/icmp.c
    ${LWIP}/core/ipv4/ip4.c ${LWIP}/core/ipv4/ip4_addr.c ${LWIP}/core/ipv4/ip4_frag.c
    ${LWIP}/netif/ethernet.c)

set(CORE_SRC
    ${ROOT}/netcore/tunnel.c ${ROOT}/netcore/loop.c ${ROOT}/netcore/route.c
    ${ROOT}/netcore/stack.c ${ROOT}/netcore/proxy.c ${ROOT}/netcore/udprelay.c
    ${ROOT}/netcore/hostaddr.c
    ${ROOT}/vtun/vtun_proto.c ${ROOT}/vtun/vtun_client.c)

add_library(vtesse SHARED vtesse_jni.c ${CORE_SRC} ${LWIP_SRC})
target_include_directories(vtesse PRIVATE ${ROOT}/include ${ROOT}/port/lwip ${LWIP}/include)
target_compile_options(vtesse PRIVATE -std=gnu99 -Os -Wall -Wno-unused-parameter -Wno-address
                       -ffunction-sections -fdata-sections)
target_link_options(vtesse PRIVATE -Wl,--gc-sections)
target_link_libraries(vtesse PRIVATE log)
