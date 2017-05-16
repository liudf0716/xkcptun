/* vim: set et sw=4 ts=4 sts=4 : */
/********************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
 *                                                                  *
\********************************************************************/

/* $Id$ */
/** @file commandline.h
    @brief Command line argument handling
    @author Copyright (C) 2004 Philippe April <papril777@yahoo.com>
*/

#ifndef _COMMANDLINE_H_
#define _COMMANDLINE_H_

enum {
	GETOPT_VAL_NC = 257,
	GETOPT_VAL_KEY,
	GETOPT_VAL_CRYPT,
	GETOPT_VAL_MODE,
	GETOPT_VAL_MTU,
	GETOPT_VAL_SNDWND,
	GETOPT_VAL_RCVWND,
	GETOPT_VAL_DATASHARD,
	GETOPT_VAL_PARITYSHARD,
	GETOPT_VAL_DSCP,
	GETOPT_VAL_NOCOMP,
	GETOPT_VAL_ACKNODELAY,
	GETOPT_VAL_NODELAY,
	GETOPT_VAL_INTERVAL,
	GETOPT_VAL_RESEND,
	GETOPT_VAL_SOCKBUF,
	GETOPT_VAL_KEEPALIVE,
	GETOPT_VAL_HELP,
	GETOPT_VAL_VERSION
};

/** @brief Parses the command line and set the config accordingly */
void parse_commandline(int, char **);

void usage();

#endif                          /* _COMMANDLINE_H_ */
