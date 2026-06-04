/*  
    VTun - Virtual Tunnel over TCP/IP network.

    Copyright (C) 1998-2016  Maxim Krasnyansky <max_mk@yahoo.com>

    VTun has been derived from VPPP package by Maxim Krasnyansky. 

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
 */

/*
 * $Id: compat.h,v 1.3.2.3 2016/10/01 21:29:28 mtbishop Exp $
 */ 
#ifndef _VTUN_COMPAT_H
#define _VTUN_COMPAT_H

#ifndef LOG_PERROR
#define LOG_PERROR 0
#endif

#ifndef SA_NOCLDWAIT
#define SA_NOCLDWAIT 0
#endif

#endif /* _VTUN_COMPAT_H */

/* vtun's autoconf fork test mis-detects on some modern toolchains; fork()
   genuinely works on Linux/glibc.  Without HAVE_WORKING_FORK the standalone
   server (-s) mode and the up/down-script fork path are compiled out. */
#if !defined(HAVE_WORKING_FORK) && (defined(__linux__) || defined(__GLIBC__))
#define HAVE_WORKING_FORK 1
#endif
#if !defined(HAVE_WORKING_VFORK) && (defined(__linux__) || defined(__GLIBC__))
#define HAVE_WORKING_VFORK 1
#endif
