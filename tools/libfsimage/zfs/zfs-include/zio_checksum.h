/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 1999,2000,2001,2002,2003,2004  Free Software Foundation, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; If not, see <http://www.gnu.org/licenses/>.
 */
/*
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef _SYS_ZIO_CHECKSUM_H
#define	_SYS_ZIO_CHECKSUM_H

/*
 * Signature for checksum functions.
 */
typedef void zio_checksum_t(const void *data, uint64_t size, zio_cksum_t *zcp);

/*
 * Information about each checksum function.
 */
typedef struct zio_checksum_info {
	zio_checksum_t	*ci_func[2]; /* checksum function for each byteorder */
	int		ci_correctable;	/* number of correctable bits	*/
	int		ci_eck;		/* uses zio embedded checksum? */
	char		*ci_name;	/* descriptive name */
} zio_checksum_info_t;

#endif	/* _SYS_ZIO_CHECKSUM_H */
