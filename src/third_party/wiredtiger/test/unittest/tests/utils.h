/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <string>

#define DB_HOME "test_db"

#define BREAK utils::break_here(__FILE__, __func__, __LINE__)

namespace utils {
void break_here(const char *file, const char *func, int line);
void throwIfNonZero(int result);
void wiredtigerCleanup(const std::string &db_home);
} // namespace utils.
