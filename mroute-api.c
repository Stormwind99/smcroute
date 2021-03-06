/* Generic kernel multicast routing API for Linux and *BSD
 *
 * Copyright (C) 2001-2005  Carsten Schill <carsten@cschill.de>
 * Copyright (C) 2006-2009  Julien BLACHE <jb@jblache.org>
 * Copyright (C) 2009       Todd Hayton <todd.hayton@gmail.com>
 * Copyright (C) 2009-2011  Micha Lenk <micha@debian.org>
 * Copyright (C) 2011-2017  Joachim Nilsson <troglobit@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */
#include <unistd.h>
#include <arpa/inet.h>
#include "config.h"

#include "ifvc.h"
#include "mclab.h"

#ifdef HAVE_NETINET6_IP6_MROUTE_H
#include <netinet6/ip6_mroute.h>
#endif

/* MAX_MC_VIFS from mclab.h must have same value as MAXVIFS from mroute.h */
#if MAX_MC_VIFS != MAXVIFS
#error "IPv4 constants do not match, 'mclab.h' needs to be fixed!"
#endif

#ifdef HAVE_IPV6_MULTICAST_ROUTING
/* MAX_MC_MIFS from mclab.h must have same value as MAXVIFS from mroute6.h */
#if MAX_MC_MIFS != MAXMIFS
#error "IPv6 constants do not match, 'mclab.h' needs to be fixed!"
#endif
#endif

/*
 * Need a raw IGMP socket as interface for the IPv4 mrouted API
 * Receives IGMP packets and kernel upcall messages.
 */
int mroute4_socket = -1;

/* All user added/configured (*,G) routes that are matched on-demand
 * at runtime. See the mroute4_dyn_list for the actual (S,G) routes
 * set from this "template". */
LIST_HEAD(, mroute4) mroute4_conf_list = LIST_HEAD_INITIALIZER();

/* For dynamically/on-demand set (S,G) routes that we must track
 * if the user removes the configured (*,G) route. */
LIST_HEAD(, mroute4) mroute4_dyn_list = LIST_HEAD_INITIALIZER();

#ifdef HAVE_IPV6_MULTICAST_ROUTING
/*
 * Need a raw ICMPv6 socket as interface for the IPv6 mrouted API
 * Receives MLD packets and kernel upcall messages.
 */
int mroute6_socket = -1;
#endif

/* IPv4 internal virtual interfaces (VIF) descriptor vector */
static struct {
	struct iface *iface;
} vif_list[MAXVIFS];

static int mroute4_add_vif(struct iface *iface);

#ifdef HAVE_IPV6_MULTICAST_ROUTING
/* IPv6 internal virtual interfaces (VIF) descriptor vector */
static struct mif {
	struct iface *iface;
} mif_list[MAXMIFS];

static int mroute6_add_mif(struct iface *iface);
#endif

/**
 * mroute4_enable - Initialise IPv4 multicast routing
 *
 * Setup the kernel IPv4 multicast routing API and lock the multicast
 * routing socket to this program (only!).
 *
 * Returns:
 * POSIX OK(0) on success, non-zero on error with @errno set.
 */
int mroute4_enable(void)
{
	int arg = 1;
	unsigned int i;
	struct iface *iface;

	mroute4_socket = create_socket(AF_INET, SOCK_RAW, IPPROTO_IGMP);
	if (mroute4_socket < 0) {
		if (ENOPROTOOPT == errno)
			smclog(LOG_WARNING, "Kernel does not support IPv4 multicast routing, skipping ...");

		return -1;
	}

	if (setsockopt(mroute4_socket, IPPROTO_IP, MRT_INIT, (void *)&arg, sizeof(arg))) {
		switch (errno) {
		case EADDRINUSE:
			smclog(LOG_INIT, "IPv4 multicast routing API already in use: %s", strerror(errno));
			break;

		default:
			smclog(LOG_INIT, "Failed initializing IPv4 multicast routing API: %s", strerror(errno));
			break;
		}

		close(mroute4_socket);
		mroute4_socket = -1;

		return -1;
	}

	/* Initialize virtual interface table */
	memset(&vif_list, 0, sizeof(vif_list));

	/* Create virtual interfaces (VIFs) for all non-loopback interfaces supporting multicast */
	for (i = 0; do_vifs && (iface = iface_find_by_index(i)); i++) {
		/* No point in continuing the loop when out of VIF's */
		if (mroute4_add_vif(iface))
			break;
	}

	LIST_INIT(&mroute4_conf_list);
	LIST_INIT(&mroute4_dyn_list);

	return 0;
}

/**
 * mroute4_disable - Disable IPv4 multicast routing
 *
 * Disable IPv4 multicast routing and release kernel routing socket.
 */
void mroute4_disable(void)
{
	struct mroute4 *entry;

	if (mroute4_socket < 0)
		return;

	/* Drop all kernel routes set by smcroute */
	if (setsockopt(mroute4_socket, IPPROTO_IP, MRT_DONE, NULL, 0))
		smclog(LOG_WARNING, "Failed shutting down IPv4 multicast routing socket: %s", strerror(errno));

	close(mroute4_socket);
	mroute4_socket = -1;

	/* Free list of (*,G) routes on SIGHUP */
	while (!LIST_EMPTY(&mroute4_conf_list)) {
		entry = LIST_FIRST(&mroute4_conf_list);
		LIST_REMOVE(entry, link);
		free(entry);
	}
	while (!LIST_EMPTY(&mroute4_dyn_list)) {
		entry = LIST_FIRST(&mroute4_dyn_list);
		LIST_REMOVE(entry, link);
		free(entry);
	}
}


/* Create a virtual interface from @iface so it can be used for IPv4 multicast routing. */
static int mroute4_add_vif(struct iface *iface)
{
	struct vifctl vc;
	int vif = -1;
	size_t i;

	if ((iface->flags & (IFF_LOOPBACK | IFF_MULTICAST)) != IFF_MULTICAST) {
		smclog(LOG_INFO, "Interface %s is not multicast capable, skipping VIF.", iface->name);
		iface->vif = -1;
		return 0;
	}

	/* find a free vif */
	for (i = 0; i < NELEMS(vif_list); i++) {
		if (!vif_list[i].iface) {
			vif = i;
			break;
		}
	}

	/* no more space */
	if (vif == -1) {
		errno = ENOMEM;
		smclog(LOG_WARNING, "Kernel MAXVIFS (%d) too small for number of interfaces: %s", MAXVIFS, strerror(errno));
		return 1;
	}

	memset(&vc, 0, sizeof(vc));
	vc.vifc_vifi = vif;
	vc.vifc_flags = 0;      /* no tunnel, no source routing, register ? */
	vc.vifc_threshold = iface->threshold;
	vc.vifc_rate_limit = 0;	/* hopefully no limit */
#ifdef VIFF_USE_IFINDEX		/* Register VIF using ifindex, not lcl_addr, since Linux 2.6.33 */
	vc.vifc_flags |= VIFF_USE_IFINDEX;
	vc.vifc_lcl_ifindex = iface->ifindex;
#else
	vc.vifc_lcl_addr.s_addr = iface->inaddr.s_addr;
#endif
	vc.vifc_rmt_addr.s_addr = INADDR_ANY;

	smclog(LOG_DEBUG, "Map iface %-16s => VIF %-2d ifindex %2d flags 0x%04x TTL threshold %u",
	       iface->name, vc.vifc_vifi, iface->ifindex, vc.vifc_flags, iface->threshold);

	if (setsockopt(mroute4_socket, IPPROTO_IP, MRT_ADD_VIF, (void *)&vc, sizeof(vc)))
		smclog(LOG_ERR, "Failed adding VIF for iface %s: %s", iface->name, strerror(errno));

	iface->vif = vif;
	vif_list[vif].iface = iface;

	return 0;
}

static int mroute4_del_vif(struct iface *iface)
{
	int ret;
	int16_t vif = iface->vif;

	if (-1 == vif)
		return 0;	/* No VIF setup for iface, skip */

	smclog(LOG_DEBUG, "Removing  %-16s => VIF %-2d", iface->name, vif);

#ifdef __linux__
	struct vifctl vc = { .vifc_vifi = vif };
	ret = setsockopt(mroute4_socket, IPPROTO_IP, MRT_DEL_VIF, (void *)&vc, sizeof(vc));
#else
	ret = setsockopt(mroute4_socket, IPPROTO_IP, MRT_DEL_VIF, (void *)&vif, sizeof(vif));
#endif
	if (ret)
		smclog(LOG_ERR, "Failed deleting VIF for iface %s: %s", iface->name, strerror(errno));
	else
		iface->vif = -1;

	return 0;
}

/* Actually set in kernel - called by mroute4_add() and mroute4_check_add() */
static int __mroute4_add(struct mroute4 *route)
{
	int result = 0;
	char origin[INET_ADDRSTRLEN], group[INET_ADDRSTRLEN];
	struct mfcctl mc;

	memset(&mc, 0, sizeof(mc));

	mc.mfcc_origin = route->sender;
	mc.mfcc_mcastgrp = route->group;
	mc.mfcc_parent = route->inbound;

	/* copy the TTL vector */
	if (sizeof(mc.mfcc_ttls[0]) != sizeof(route->ttl[0]) || NELEMS(mc.mfcc_ttls) != NELEMS(route->ttl)) {
		smclog(LOG_ERR, "Critical data type validation error in %s!", __FILE__);
		exit(255);
	}

	memcpy(mc.mfcc_ttls, route->ttl, NELEMS(mc.mfcc_ttls) * sizeof(mc.mfcc_ttls[0]));

	smclog(LOG_DEBUG, "Add %s -> %s from VIF %d",
	       inet_ntop(AF_INET, &mc.mfcc_origin,   origin, INET_ADDRSTRLEN),
	       inet_ntop(AF_INET, &mc.mfcc_mcastgrp, group,  INET_ADDRSTRLEN), mc.mfcc_parent);

	if (setsockopt(mroute4_socket, IPPROTO_IP, MRT_ADD_MFC, (void *)&mc, sizeof(mc))) {
		result = errno;
		smclog(LOG_WARNING, "Failed adding IPv4 multicast route: %s", strerror(errno));
	}

	return result;
}

/* Actually remove from kernel - called by mroute4_del() */
static int __mroute4_del(struct mroute4 *route)
{
	int result = 0;
	char origin[INET_ADDRSTRLEN], group[INET_ADDRSTRLEN];
	struct mfcctl mc;

	memset(&mc, 0, sizeof(mc));
	mc.mfcc_origin = route->sender;
	mc.mfcc_mcastgrp = route->group;

	smclog(LOG_DEBUG, "Del %s -> %s",
	       inet_ntop(AF_INET, &mc.mfcc_origin,  origin, INET_ADDRSTRLEN),
	       inet_ntop(AF_INET, &mc.mfcc_mcastgrp, group, INET_ADDRSTRLEN));

	if (setsockopt(mroute4_socket, IPPROTO_IP, MRT_DEL_MFC, (void *)&mc, sizeof(mc))) {
		result = errno;
		smclog(LOG_WARNING, "Failed removing IPv4 multicast route: %s", strerror(errno));
	}

	return result;
}

/*
 * Used for (*,G) matches
 *
 * The incoming candidate is compared to the configured rule, e.g.
 * does 225.1.2.3 fall inside 225.0.0.0/8?  => Yes
 * does 225.1.2.3 fall inside 225.0.0.0/15? => Yes
 * does 225.1.2.3 fall inside 225.0.0.0/16? => No
 */
static int __mroute4_match(struct mroute4 *rule, struct mroute4 *cand)
{
	uint32_t g1, g2, mask;

	if (rule->inbound != cand->inbound)
		return 0;

	/* This handles len == 0 => 255.255.255.255 */
	mask = htonl(0xFFFFFFFFu << (32 - rule->len));
	g1 = rule->group.s_addr & mask;
	g2 = cand->group.s_addr & mask;

	return g1 == g2;
}

/**
 * mroute4_dyn_add - Add route to kernel if it matches a known (*,G) route.
 * @route: Pointer to candidate struct mroute4 IPv4 multicast route
 *
 * Returns:
 * POSIX OK(0) on success, non-zero on error with @errno set.
 */
int mroute4_dyn_add(struct mroute4 *route)
{
	struct mroute4 *entry;

	LIST_FOREACH(entry, &mroute4_conf_list, link) {
		/* Find matching (*,G) ... and interface. */
		if (__mroute4_match(entry, route)) {
			/* Use configured template (*,G) outbound interfaces. */
			memcpy(route->ttl, entry->ttl, NELEMS(route->ttl) * sizeof(route->ttl[0]));

			/* Add to list of dynamically added routes. Necessary if the user
			 * removes the (*,G) using the command line interface rather than
			 * updating the conf file and SIGHUP. Note: if we fail to alloc()
			 * memory we don't do anything, just add kernel route silently. */
			entry = malloc(sizeof(struct mroute4));
			if (entry) {
				memcpy(entry, route, sizeof(struct mroute4));
				LIST_INSERT_HEAD(&mroute4_dyn_list, entry, link);
			}

			return __mroute4_add(route);
		}
	}

	errno = ENOENT;
	return -1;
}

/**
 * mroute4_dyn_flush - Flush dynamically added (*,G) routes
 *
 * This function flushes all (*,G) routes.  It is currently only called
 * on cache-timeout, a command line option, but could also be called on
 * topology changes (e.g. VRRP fail-over) or similar.
 */
void mroute4_dyn_flush(void)
{
	if (LIST_EMPTY(&mroute4_dyn_list))
		return;

	while (mroute4_dyn_list.lh_first) {
		__mroute4_del((struct mroute4 *)mroute4_dyn_list.lh_first);
		LIST_REMOVE(mroute4_dyn_list.lh_first, link);
		free(mroute4_dyn_list.lh_first);
	}
}

/**
 * mroute4_add - Add route to kernel, or save a wildcard route for later use
 * @route: Pointer to struct mroute4 IPv4 multicast route to add
 *
 * Adds the given multicast @route to the kernel multicast routing table
 * unless the source IP is %INADDR_ANY, i.e., a (*,G) route.  Those we
 * save for and check against at runtime when the kernel signals us.
 *
 * Returns:
 * POSIX OK(0) on success, non-zero on error with @errno set.
 */
int mroute4_add(struct mroute4 *route)
{
	/* For (*,G) we save to a linked list to be added on-demand
	 * when the kernel sends IGMPMSG_NOCACHE. */
	if (route->sender.s_addr == INADDR_ANY) {
		struct mroute4 *entry = malloc(sizeof(struct mroute4));

		if (!entry) {
			smclog(LOG_WARNING, "Failed adding (*,G) multicast route: %s", strerror(errno));
			return errno;
		}

		memcpy(entry, route, sizeof(struct mroute4));
		LIST_INSERT_HEAD(&mroute4_conf_list, entry, link);

		return 0;
	}

	return __mroute4_add(route);
}

/**
 * mroute4_del - Remove route from kernel, or all matching routes if wildcard
 * @route: Pointer to struct mroute4 IPv4 multicast route to remove
 *
 * Removes the given multicast @route from the kernel multicast routing
 * table, or if the @route is a wildcard, then all matching kernel
 * routes are removed, as well as the wildcard.
 *
 * Returns:
 * POSIX OK(0) on success, non-zero on error with @errno set.
 */
int mroute4_del(struct mroute4 *route)
{
	struct mroute4 *entry, *set;

	/* For (*,G) we have saved all dynamically added kernel routes
	 * to a linked list which we need to traverse again and remove
	 * all matches. From kernel dyn list before we remove the conf
	 * entry. */
	if (route->sender.s_addr != INADDR_ANY)
		return __mroute4_del(route);

	if (LIST_EMPTY(&mroute4_conf_list))
		return 0;

	entry = LIST_FIRST(&mroute4_conf_list);
	while (entry) {
		/* Find matching (*,G) ... and interface .. and prefix length. */
		if (__mroute4_match(entry, route) && entry->len == route->len) {
			if (LIST_EMPTY(&mroute4_dyn_list))
				goto empty;

			set = LIST_FIRST(&mroute4_dyn_list);
			while (set) {
				if (__mroute4_match(entry, set) && entry->len == route->len) {
					__mroute4_del(set);
					LIST_REMOVE(set, link);
					free(set);

					set = LIST_FIRST(&mroute4_dyn_list);
					continue;
				}

				set = LIST_NEXT(set, link);
			}

		empty:
			LIST_REMOVE(entry, link);
			free(entry);

			entry = LIST_FIRST(&mroute4_conf_list);
			continue;
		}

		entry = LIST_NEXT(entry, link);
	}

	return 0;
}

#ifdef HAVE_IPV6_MULTICAST_ROUTING
#ifdef __linux__
#define IPV6_ALL_MC_FORWARD "/proc/sys/net/ipv6/conf/all/mc_forwarding"

static int proc_set_val(char *file, int val)
{
	int fd, result = 0;

	fd = open(file, O_WRONLY);
	if (fd < 0)
		return 1;

	if (-1 == write(fd, "1", val))
		result = 1;

	close(fd);

	return result;
}
#endif /* Linux only */
#endif /* HAVE_IPV6_MULTICAST_ROUTING */

/**
 * mroute6_enable - Initialise IPv6 multicast routing
 *
 * Setup the kernel IPv6 multicast routing API and lock the multicast
 * routing socket to this program (only!).
 *
 * Returns:
 * POSIX OK(0) on success, non-zero on error with @errno set.
 */
int mroute6_enable(void)
{
#ifndef HAVE_IPV6_MULTICAST_ROUTING
	return -1;
#else
	int arg = 1;
	unsigned int i;
	struct iface *iface;

	if ((mroute6_socket = create_socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6)) < 0) {
		if (ENOPROTOOPT == errno)
			smclog(LOG_WARNING, "Kernel does not support IPv6 multicast routing, skipping ...");

		return -1;
	}
	if (setsockopt(mroute6_socket, IPPROTO_IPV6, MRT6_INIT, (void *)&arg, sizeof(arg))) {
		switch (errno) {
		case EADDRINUSE:
			smclog(LOG_INIT, "IPv6 multicast routing API already in use: %s", strerror(errno));
			break;

		default:
			smclog(LOG_INIT, "Failed initializing IPv6 multicast routing API: %s", strerror(errno));
			break;
		}

		close(mroute6_socket);
		mroute6_socket = -1;

		return -1;
	}

	/* Initialize virtual interface table */
	memset(&mif_list, 0, sizeof(mif_list));

#ifdef __linux__
	/* On Linux pre 2.6.29 kernels net.ipv6.conf.all.mc_forwarding
	 * is not set on MRT6_INIT so we have to do this manually */
	if (proc_set_val(IPV6_ALL_MC_FORWARD, 1)) {
		if (errno != EACCES) {
			smclog(LOG_ERR, "Failed enabling IPv6 multicast forwarding: %s", strerror(errno));
			exit(255);
		}
	}
#endif
	/* Create virtual interfaces, IPv6 MIFs, for all non-loopback interfaces */
	for (i = 0; do_vifs && (iface = iface_find_by_index(i)); i++) {
		/* No point in continuing the loop when out of MIF's */
		if (mroute6_add_mif(iface))
			break;
	}

	return 0;
#endif /* HAVE_IPV6_MULTICAST_ROUTING */
}

/**
 * mroute6_disable - Disable IPv6 multicast routing
 *
 * Disable IPv6 multicast routing and release kernel routing socket.
 */
void mroute6_disable(void)
{
#ifdef HAVE_IPV6_MULTICAST_ROUTING
	if (mroute6_socket < 0)
		return;

	if (setsockopt(mroute6_socket, IPPROTO_IPV6, MRT6_DONE, NULL, 0))
		smclog(LOG_WARNING, "Failed shutting down IPv6 multicast routing socket: %s", strerror(errno));

	close(mroute6_socket);
	mroute6_socket = -1;
#endif /* HAVE_IPV6_MULTICAST_ROUTING */
}

#ifdef HAVE_IPV6_MULTICAST_ROUTING
/* Create a virtual interface from @iface so it can be used for IPv6 multicast routing. */
static int mroute6_add_mif(struct iface *iface)
{
	struct mif6ctl mc;
	int mif = -1;
	size_t i;

	if ((iface->flags & (IFF_LOOPBACK | IFF_MULTICAST)) != IFF_MULTICAST) {
		smclog(LOG_INFO, "Interface %s is not multicast capable, skipping MIF.", iface->name);
		iface->mif = -1;
		return 0;
	}

	/* find a free mif */
	for (i = 0; i < NELEMS(mif_list); i++) {
		if (!mif_list[i].iface) {
			mif = i;
			break;
		}
	}

	/* no more space */
	if (mif == -1) {
		errno = ENOMEM;
		smclog(LOG_WARNING, "Kernel MAXMIFS (%d) too small for number of interfaces: %s", MAXMIFS, strerror(errno));
		return 1;
	}

	memset(&mc, 0, sizeof(mc));
	mc.mif6c_mifi = mif;
	mc.mif6c_flags = 0;	/* no register */
#ifdef HAVE_MIF6CTL_VIFC_THRESHOLD
	mc.vifc_threshold = iface->threshold;
#endif
	mc.mif6c_pifi = iface->ifindex;	/* physical interface index */
#ifdef HAVE_MIF6CTL_VIFC_RATE_LIMIT
	mc.vifc_rate_limit = 0;	/* hopefully no limit */
#endif

	smclog(LOG_DEBUG, "Map iface %-16s => MIF %-2d ifindex %2d flags 0x%04x TTL threshold %u",
	       iface->name, mc.mif6c_mifi, mc.mif6c_pifi, mc.mif6c_flags, iface->threshold);

	if (setsockopt(mroute6_socket, IPPROTO_IPV6, MRT6_ADD_MIF, (void *)&mc, sizeof(mc))) {
		smclog(LOG_ERR, "Failed adding MIF for iface %s: %s", iface->name, strerror(errno));
		iface->mif = -1;
	} else {
		iface->mif = mif;
		mif_list[mif].iface = iface;
	}

	return 0;
}

static int mroute6_del_mif(struct iface *iface)
{
	int16_t mif = iface->mif;

	if (-1 == mif)
		return 0;	/* No MIF setup for iface, skip */

	smclog(LOG_DEBUG, "Removing  %-16s => MIF %-2d", iface->name, mif);

	if (setsockopt(mroute6_socket, IPPROTO_IPV6, MRT6_DEL_MIF, (void *)&mif, sizeof(mif)))
		smclog(LOG_ERR, "Failed deleting MIF for iface %s: %s", iface->name, strerror(errno));
	else
		iface->mif = -1;

	return 0;
}

/**
 * mroute6_add - Add route to kernel, or save a wildcard route for later use
 * @route: Pointer to struct mroute6 IPv6 multicast route to add
 *
 * Adds the given multicast @route to the kernel multicast routing table.
 *
 * Returns:
 * POSIX OK(0) on success, non-zero on error with @errno set.
 */
int mroute6_add(struct mroute6 *route)
{
	int result = 0;
	size_t i;
	char origin[INET6_ADDRSTRLEN], group[INET6_ADDRSTRLEN];
	struct mf6cctl mc;

	memset(&mc, 0, sizeof(mc));
	mc.mf6cc_origin   = route->sender;
	mc.mf6cc_mcastgrp = route->group;
	mc.mf6cc_parent   = route->inbound;

	/* copy the outgoing MIFs */
	for (i = 0; i < NELEMS(route->ttl); i++) {
		if (route->ttl[i] > 0)
			IF_SET(i, &mc.mf6cc_ifset);
	}

	smclog(LOG_DEBUG, "Add %s -> %s from MIF %d",
	       inet_ntop(AF_INET6, &mc.mf6cc_origin.sin6_addr, origin, INET6_ADDRSTRLEN),
	       inet_ntop(AF_INET6, &mc.mf6cc_mcastgrp.sin6_addr, group, INET6_ADDRSTRLEN),
	       mc.mf6cc_parent);

	if (setsockopt(mroute6_socket, IPPROTO_IPV6, MRT6_ADD_MFC, (void *)&mc, sizeof(mc))) {
		result = errno;
		smclog(LOG_WARNING, "Failed adding IPv6 multicast route: %s", strerror(errno));
	}

	return result;
}

/**
 * mroute6_del - Remove route from kernel
 * @route: Pointer to struct mroute6 IPv6 multicast route to remove
 *
 * Removes the given multicast @route from the kernel multicast routing
 * table.
 *
 * Returns:
 * POSIX OK(0) on success, non-zero on error with @errno set.
 */
int mroute6_del(struct mroute6 *route)
{
	int result = 0;
	char origin[INET6_ADDRSTRLEN], group[INET6_ADDRSTRLEN];
	struct mf6cctl mc;

	memset(&mc, 0, sizeof(mc));
	mc.mf6cc_origin = route->sender;
	mc.mf6cc_mcastgrp = route->group;

	smclog(LOG_DEBUG, "Del %s -> %s",
	       inet_ntop(AF_INET6, &mc.mf6cc_origin.sin6_addr, origin, INET6_ADDRSTRLEN),
	       inet_ntop(AF_INET6, &mc.mf6cc_mcastgrp.sin6_addr, group, INET6_ADDRSTRLEN));

	if (setsockopt(mroute6_socket, IPPROTO_IPV6, MRT6_DEL_MFC, (void *)&mc, sizeof(mc))) {
		result = errno;
		smclog(LOG_WARNING, "Failed removing IPv6 multicast route: %s", strerror(errno));
	}

	return result;
}
#endif /* HAVE_IPV6_MULTICAST_ROUTING */

/* Used by file parser to add VIFs/MIFs after setup */
int mroute_add_vif(char *ifname, uint8_t threshold)
{
	int ret;
	struct iface *iface;

	smclog(LOG_DEBUG, "Adding %s to list of multicast routing interfaces", ifname);
	iface = iface_find_by_name(ifname);
	if (!iface)
		return 1;

	iface->threshold = threshold;
	ret = mroute4_add_vif(iface);
#ifdef HAVE_IPV6_MULTICAST_ROUTING
	ret += mroute6_add_mif(iface);
#endif

	return ret;
}

/* Used by file parser to remove VIFs/MIFs after setup */
int mroute_del_vif(char *ifname)
{
	int ret;
	struct iface *iface;

	smclog(LOG_DEBUG, "Pruning %s from list of multicast routing interfaces", ifname);
	iface = iface_find_by_name(ifname);
	if (!iface)
		return 1;

	ret = mroute4_del_vif(iface);
#ifdef HAVE_IPV6_MULTICAST_ROUTING
	ret += mroute6_del_mif(iface);
#endif

	return ret;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
