/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <errno.h>
#include <string.h>
#include <limits.h>

#include <rte_ether.h>

#include "vhost_kernel_tap.h"
#include "../virtio_logs.h"
#include "../virtio_pci.h"

static int
vhost_kernel_tap_set_offload(int fd, uint64_t features)
{
	unsigned int offload = 0;

	if (features & (1ULL << VIRTIO_NET_F_GUEST_CSUM)) {
		offload |= TUN_F_CSUM;
		if (features & (1ULL << VIRTIO_NET_F_GUEST_TSO4))
			offload |= TUN_F_TSO4;
		if (features & (1ULL << VIRTIO_NET_F_GUEST_TSO6))
			offload |= TUN_F_TSO6;
		if (features & ((1ULL << VIRTIO_NET_F_GUEST_TSO4) |
			(1ULL << VIRTIO_NET_F_GUEST_TSO6)) &&
			(features & (1ULL << VIRTIO_NET_F_GUEST_ECN)))
			offload |= TUN_F_TSO_ECN;
		if (features & (1ULL << VIRTIO_NET_F_GUEST_UFO))
			offload |= TUN_F_UFO;
	}

	if (offload != 0) {
		/* Check if our kernel supports TUNSETOFFLOAD */
		if (ioctl(fd, TUNSETOFFLOAD, 0) != 0 && errno == EINVAL) {
			PMD_DRV_LOG(ERR, "Kernel does't support TUNSETOFFLOAD\n");
			return -ENOTSUP;
		}

		if (ioctl(fd, TUNSETOFFLOAD, offload) != 0) {
			offload &= ~TUN_F_UFO;
			if (ioctl(fd, TUNSETOFFLOAD, offload) != 0) {
				PMD_DRV_LOG(ERR, "TUNSETOFFLOAD ioctl() failed: %s\n",
					strerror(errno));
				return -1;
			}
		}
	}

	return 0;
}

int
vhost_kernel_open_tap(char **p_ifname, int hdr_size, int req_mq,
			 const char *mac, uint64_t features)
{
	unsigned int tap_features;
	int sndbuf = INT_MAX;
	struct ifreq ifr;
	int tapfd;

	/* TODO:
	 * 1. verify we can get/set vnet_hdr_len, tap_probe_vnet_hdr_len
	 * 2. get number of memory regions from vhost module parameter
	 * max_mem_regions, supported in newer version linux kernel
	 */
	tapfd = open(PATH_NET_TUN, O_RDWR);
	if (tapfd < 0) {
		PMD_DRV_LOG(ERR, "fail to open %s: %s",
			    PATH_NET_TUN, strerror(errno));
		return -1;
	}

	/* Construct ifr */
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

	if (ioctl(tapfd, TUNGETFEATURES, &tap_features) == -1) {
		PMD_DRV_LOG(ERR, "TUNGETFEATURES failed: %s", strerror(errno));
		goto error;
	}
	if (tap_features & IFF_ONE_QUEUE)
		ifr.ifr_flags |= IFF_ONE_QUEUE;

	/* Let tap instead of vhost-net handle vnet header, as the latter does
	 * not support offloading. And in this case, we should not set feature
	 * bit VHOST_NET_F_VIRTIO_NET_HDR.
	 */
	if (tap_features & IFF_VNET_HDR) {
		ifr.ifr_flags |= IFF_VNET_HDR;
	} else {
		PMD_DRV_LOG(ERR, "TAP does not support IFF_VNET_HDR");
		goto error;
	}

	if (req_mq)
		ifr.ifr_flags |= IFF_MULTI_QUEUE;

	if (*p_ifname)
		strncpy(ifr.ifr_name, *p_ifname, IFNAMSIZ - 1);
	else
		strncpy(ifr.ifr_name, "tap%d", IFNAMSIZ - 1);
	if (ioctl(tapfd, TUNSETIFF, (void *)&ifr) == -1) {
		PMD_DRV_LOG(ERR, "TUNSETIFF failed: %s", strerror(errno));
		goto error;
	}

	fcntl(tapfd, F_SETFL, O_NONBLOCK);

	if (ioctl(tapfd, TUNSETVNETHDRSZ, &hdr_size) < 0) {
		PMD_DRV_LOG(ERR, "TUNSETVNETHDRSZ failed: %s", strerror(errno));
		goto error;
	}

	if (ioctl(tapfd, TUNSETSNDBUF, &sndbuf) < 0) {
		PMD_DRV_LOG(ERR, "TUNSETSNDBUF failed: %s", strerror(errno));
		goto error;
	}

	vhost_kernel_tap_set_offload(tapfd, features);

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
	memcpy(ifr.ifr_hwaddr.sa_data, mac, ETHER_ADDR_LEN);
	if (ioctl(tapfd, SIOCSIFHWADDR, (void *)&ifr) == -1) {
		PMD_DRV_LOG(ERR, "SIOCSIFHWADDR failed: %s", strerror(errno));
		goto error;
	}

	if (!(*p_ifname))
		*p_ifname = strdup(ifr.ifr_name);

	return tapfd;
error:
	close(tapfd);
	return -1;
}
