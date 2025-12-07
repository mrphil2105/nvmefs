#pragma once

#include "duckdb.hpp"
#include <cstdint>
#include <boost/thread/shared_mutex.hpp>

namespace duckdb {

enum LockType { SHARED_TEMP_LOCK, UNIQUE_TEMP_LOCK, SHARED_FILE_LOCK, UNIQUE_FILE_LOCK };

class LockLogger {
public:
	void AccessLock(const string function_name, LockType lock_type);

	void WriteToCSV(const string file_path);

private:
	map<string, uint32_t> lock_name_to_lock_access;
	map<string, uint32_t> function_lock_name_to_lock_access;
	boost::shared_mutex logger_mutex;
};
} // namespace duckdb
