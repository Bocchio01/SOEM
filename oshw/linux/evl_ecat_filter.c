/* SPDX-License-Identifier: GPL-2.0 */
/*
 * evl_ecat_filter.c -- PEGASUS / Xenomai 4 (EVL) out-of-band variant
 *
 * Minimal eBPF ingress filter that accepts EtherCAT (ETH_P_ECAT,
 * 0x88A4) frames into the EVL out-of-band network stack.
 *
 * WHY THIS EXISTS
 * Absent an eBPF filter, EVL's default ingress-selection rules only
 * divert IPv4 (ETH_P_IP, 0x0800) traffic to the out-of-band stack --
 * both the "reserved interface" rule and the VLAN-tagging rule are
 * explicitly scoped to IPv4 in the docs. EtherCAT is not IPv4, so on
 * a NIC dedicated to an EtherCAT segment, ecx_setupnic() would
 * otherwise open a perfectly good SOCK_OOB raw socket that never
 * receives a single frame -- everything still "works" (compiles,
 * binds, the cyclic loop runs) while quietly getting zero real-time
 * guarantee. This filter is the documented escape hatch:
 *   https://v4.xenomai.org/core/net/index.html#evl-input-diversion
 *   https://v4.xenomai.org/core/net/setup/index.html
 *
 * BUILD
 *   clang -O2 -g -target bpf -c evl_ecat_filter.c -o evl_ecat_filter.o
 *
 * LOAD
 * Either at deployment/boot time via the evl-net CLI:
 *   evl net -F evl_ecat_filter.o -i <ifname>
 * or programmatically, once per interface, e.g. from ecx_setupnic()
 * in nicdrv.c via evl_net_set_filter(devfd, "/path/to/evl_ecat_filter.o")
 * -- which is what the patched ecx_setupnic() does by default; adjust
 * EVL_ECAT_FILTER_PATH in nicdrv.c to wherever this .o ends up on
 * your target's filesystem, or remove that call if you'd rather load
 * the filter once at boot instead.
 *
 * This filter only classifies EtherCAT frames; it does not need (or
 * use) any EtherCAT-specific header knowledge beyond the EtherType,
 * so it does not depend on SOEM's own headers and is compiled as a
 * fully separate translation unit, per standard eBPF practice.
 */
#include <linux/types.h>
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include <evl/net/bpf-abi.h>

/* IEEE-registered EtherCAT EtherType. */
#ifndef ETH_P_ECAT
#define ETH_P_ECAT 0x88A4
#endif

SEC("socket")
int bpf_netrx(struct __sk_buff *skb)
{
   if (bpf_ntohs(skb->protocol) == ETH_P_ECAT)
      return EVL_RX_ACCEPT;

   return EVL_RX_SKIP;
}

char _license[] SEC("license") = "GPL";
