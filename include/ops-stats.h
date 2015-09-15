/*
 * Copyright (C) 2015 Hewlett-Packard Development Company, L.P.
 * All Rights Reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License"); you may
 *   not use this file except in compliance with the License. You may obtain
 *   a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *   WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 *   License for the specific language governing permissions and limitations
 *   under the License.
 *
 * File: ops-stats.h
 *
 * Purpose: This file provides public definitions for Interface statistics API.
 */

#ifndef __OPS_STAT_H__
#define __OPS_STAT_H__ 1

extern int bcmsdk_get_port_stats(int hw_unit, int hw_port, struct netdev_stats *stats);

#endif /* __OPS_STAT_H__ */