/*
 * ipv6-tunnel.cpp - IPv6 interface class definition
 * $Id: ipv6-tunnel.cpp,v 1.4 2004/06/17 22:52:28 rdenisc Exp $
 */

/***********************************************************************
 *  Copyright (C) 2004 Remi Denis-Courmont.                            *
 *  This program is free software; you can redistribute and/or modify  *
 *  it under the terms of the GNU General Public License as published  *
 *  by the Free Software Foundation; version 2 of the license.         *
 *                                                                     *
 *  This program is distributed in the hope that it will be useful,    *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of     *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.               *
 *  See the GNU General Public License for more details.               *
 *                                                                     *
 *  You should have received a copy of the GNU General Public License  *
 *  along with this program; if not, you can get it from:              *
 *  http://www.gnu.org/copyleft/gpl.html                               *
 ***********************************************************************/

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <syslog.h>
#include "ipv6-tunnel.h"

#if HAVE_LINUX_IF_TUN_H
# include <linux/if_tun.h> // TUNSETIFF
#endif
#include <net/if.h> // struct ifreq
#include <sys/socket.h> // socket(PF_INET6, SOCK_DGRAM, 0)
#if HAVE_LINUX_IPV6_H
# include <linux/ipv6.h> // strict in6_ifreq
#endif

/* 
 * NOTE:
 * This has to be done by hand rather than through htons(),
 * not for optimisation, but because <netinet/in.h> conflicts with
 * <linux/ipv6.h> on my system.
 */
#ifdef WORDS_BIGENDIAN
# define L2_PROTO_IPV6 0x86dd
#else
# define L2_PROTO_IPV6 0xdd86
#endif

static int
socket_udp6 (void)
{
	int fd = socket (PF_INET6, SOCK_DGRAM, 0);
	if (fd == -1)
                syslog (LOG_ERR, _("IPv6 stack not available: %m\n"));
	return fd;
}


inline void
secure_strncpy (char *tgt, const char *src, size_t len)
{
	strncpy (tgt, src, len);
	tgt[len - 1] = '\0';
}



IPv6Tunnel::IPv6Tunnel (const char *req_name, const char *tundev)
{
	if (tundev == NULL)
		tundev = "/dev/net/tun";

	fd = open (tundev, O_RDWR);
	if (fd == -1)
	{
		syslog (LOG_ERR, _("Tunneling driver error (%s): %m\n"),
			tundev);
		return;
	}

	// Allocates the tunneling virtual network interface
	struct ifreq req;
	memset (&req, 0, sizeof (req));
	if (req_name != NULL)
		secure_strncpy (req.ifr_name, req_name, IFNAMSIZ);
	req.ifr_flags = IFF_TUN;

	if (ioctl (fd, TUNSETIFF, (void *)&req))
	{
		syslog (LOG_ERR, _("Tunnel error (TUNSETIFF): %m\n"));
		close (fd);
		fd = -1;
	}

	secure_strncpy (ifname, req.ifr_name, IFNAMSIZ);
	syslog (LOG_INFO, _("Tunneling interface %s created.\n"), ifname);
}


IPv6Tunnel::~IPv6Tunnel ()
{
	if (fd != -1)
	{
		syslog (LOG_INFO, _("Tunneling interface %s removed.\n"),
			ifname);
		close (fd);
	}
}


int
IPv6Tunnel::SetState (bool up) const
{
	if (fd == -1)
		return -1;

	int reqfd = socket_udp6 ();
	if (reqfd == -1)
		return -1;

	// Sets up the interface
	struct ifreq req;
	memset (&req, 0, sizeof (req));	
	secure_strncpy (req.ifr_name, ifname, IFNAMSIZ);
	if (ioctl (reqfd, SIOCGIFFLAGS, &req))
	{
		syslog (LOG_ERR, _("Tunnel error (SIOCGIFFLAGS): %m\n"));
		close (reqfd);
		return -1;
	}

	secure_strncpy (req.ifr_name, ifname, IFNAMSIZ);
	// settings we want/don't want:
	req.ifr_flags |= IFF_POINTOPOINT | IFF_NOARP;
	if (up)
		req.ifr_flags |= IFF_UP | IFF_RUNNING;
	else
		req.ifr_flags &= ~IFF_UP;
	req.ifr_flags &= ~(IFF_MULTICAST | IFF_BROADCAST);

	if (ioctl (reqfd, SIOCSIFFLAGS, &req))
	{
		syslog (LOG_ERR, _("%s tunnel error (SIOCSIFFLAGS): %m\n"),
			ifname);
		close (reqfd);
		return -1;
	}

	close (reqfd);
	return 0;
}


int
IPv6Tunnel::SetAddress (const struct in6_addr *addr, int prefix_len) const
{
	if ((fd == -1) || (prefix_len < 0))
		return -1;

	if (prefix_len > 128)
	{
		syslog (LOG_ERR, _("IPv6 prefix length too long: %d\n"),
			prefix_len);
		return -1;
	}

	int reqfd = socket_udp6 ();
	if (reqfd == -1)
		return -1;

	// Gets kernel's interface index
	struct ifreq req;
	memset (&req, 0, sizeof (req));
	secure_strncpy (req.ifr_name, ifname, IFNAMSIZ);
	if (ioctl (reqfd, SIOCGIFINDEX, &req) == 0)
	{
		struct in6_ifreq req6;

		memset (&req6, 0, sizeof (req6));
		req6.ifr6_ifindex = req.ifr_ifindex;
		memcpy (&req6.ifr6_addr, addr, sizeof (struct in6_addr));
		req6.ifr6_prefixlen = prefix_len;

		if (ioctl (reqfd, SIOCSIFADDR, &req6) == 0)
		{
			// FIXME: display address/prefix
			syslog (LOG_DEBUG, _("%s tunnel address set\n"),
				ifname);
			close (reqfd);
			return 0;
		}
	}

	close (reqfd);
	return -1;
}


int
IPv6Tunnel::SetMTU (int mtu) const
{
	if (fd == -1)
		return -1;
	if (mtu < 1280)
	{
		syslog (LOG_ERR, _("IPv6 MTU too small (<1280): %d\n"), mtu);
		return -1;
	}
	if (mtu > 65535)
	{
		syslog (LOG_ERR, _("IPv6 MTU too big (>65535): %d\n"), mtu);
		return -1;
	}

	int reqfd = socket_udp6 ();
	if (reqfd == -1)
		return -1;

	struct ifreq req;
	memset (&req, 0, sizeof (req));
	secure_strncpy (req.ifr_name, ifname, IFNAMSIZ);
	req.ifr_mtu = mtu;

	if (ioctl (reqfd, SIOCSIFMTU, &req))
	{
		syslog (LOG_ERR, _("%s tunnel MTU error (SIOCSIFMTU): %m\n"),
			ifname);
		close (reqfd);
		return -1;
	}

	syslog (LOG_DEBUG, _("%s tunnel MTU set to %d.\n"), ifname, mtu);
	return 0;
}


    

int
IPv6Tunnel::RegisterReadSet (fd_set *readset) const
{
	if (fd != -1)
		FD_SET (fd, readset);
	return fd;
}


int
IPv6Tunnel::ReceivePacket (const fd_set *readset)
{
	if ((fd == -1) || !FD_ISSET (fd, readset))
		return -1;

	int len = read (fd, pbuf, sizeof (pbuf));
	if (len == -1)
		return -1;

	plen = len;
	uint16_t flags, proto;
	memcpy (&flags, pbuf, 2);
	memcpy (&proto, pbuf + 2, 2);
	if (proto != L2_PROTO_IPV6)
		return -1; // only accept IPv6 packets

	return 0;
}


int
IPv6Tunnel::SendPacket (const void *packet, size_t len) const
{
	if ((fd != -1) && (len <= 65535))
	{
		uint8_t buf[65535 + 4];
		const uint16_t flags = 0, proto = L2_PROTO_IPV6;

		memcpy (buf, &flags, 2);
		memcpy (buf + 2, &proto, 2);
		memcpy (buf + 4, packet, len);

		len += 4;

		if (write (fd, buf, len) == (int)len)
			return 0;
		if ((int)len == -1)
			syslog (LOG_ERR,
				_("Cannot send packet to tunnel: %m"));
		else
			syslog (LOG_ERR,
				_("Packet truncated to %u byte(s)\n"), len);
	}
	return -1;
}

