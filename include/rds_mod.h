/*
 * RDS Modulator from:
 * PiFmRds - FM/RDS transmitter for the Raspberry Pi
 * https://github.com/ChristopheJacquet/PiFmRds
 * 
 * Copyright (C) 2014 by Christophe Jacquet, F8FTK
 *
 * adapted for use with fl2k_fm:
 * Copyright (C) 2018 by Steve Markgraf <steve@steve-m.de>
 *
 * SPDX-License-Identifier: GPL-3.0+
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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

#ifndef RDS_H
#define RDS_H

#define RDS_MODULATOR_RATE	(57000 * 4)

void get_rds_samples(double *buffer, int count);
void set_rds_pi(uint16_t pi_code);
void set_rds_rt(char *rt);
void set_rds_ps(char *ps);
void set_rds_ta(int ta);

#endif /* RDS_H */
