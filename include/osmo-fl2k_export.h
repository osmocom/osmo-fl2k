/*
 * osmo-fl2k, turns FL2000-based USB 3.0 to VGA adapters into
 * low cost DACs
 *
 * Copyright (C) 2016-2018 by Steve Markgraf <steve@steve-m.de>
 *
 * SPDX-License-Identifier: GPL-2.0+
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef FL2K_EXPORT_H
#define FL2K_EXPORT_H

#if defined __GNUC__
#  if __GNUC__ >= 4
#    define __FL2K_EXPORT   __attribute__((visibility("default")))
#    define __FL2K_IMPORT   __attribute__((visibility("default")))
#  else
#    define __FL2K_EXPORT
#    define __FL2K_IMPORT
#  endif
#elif _MSC_VER
#  define __FL2K_EXPORT     __declspec(dllexport)
#  define __FL2K_IMPORT     __declspec(dllimport)
#else
#  define __FL2K_EXPORT
#  define __FL2K_IMPORT
#endif

#ifndef libosmofl2k_STATIC
#	ifdef fl2k_EXPORTS
#	define FL2K_API __FL2K_EXPORT
#	else
#	define FL2K_API __FL2K_IMPORT
#	endif
#else
#define FL2K_API
#endif
#endif /* FL2K_EXPORT_H */
