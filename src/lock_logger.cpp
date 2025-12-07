#include "lock_logger.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/fstream.hpp"
#include <cstdint>

// Source - https://stackoverflow.com/a
// Posted by iFreilicht, modified by community. See post 'Timeline' for change history
// Retrieved 2025-12-07, License - CC BY-SA 4.0

#include <memory>
#include <string>
#include <stdexcept>

template <typename... Args>
std::string string_format(const std::string &format, Args... args) {
	int size_s = std::snprintf(nullptr, 0, format.c_str(), args...) + 1; // Extra space for '\0'
	if (size_s <= 0) {
		throw std::runtime_error("Error during formatting.");
	}
	auto size = static_cast<size_t>(size_s);
	std::unique_ptr<char[]> buf(new char[size]);
	std::snprintf(buf.get(), size, format.c_str(), args...);
	return std::string(buf.get(), buf.get() + size - 1); // We don't want the '\0' inside
}

namespace duckdb {
void LockLogger::AccessLock(const string function_name, LockType lock_type) {
	string lock_name = "";
	switch (lock_type) {
	case LockType::SHARED_TEMP_LOCK:
		lock_name = "temp_mutex shared";
		break;
	case LockType::UNIQUE_TEMP_LOCK:
		lock_name = "temp_mutex unique";
		break;
	case LockType::SHARED_FILE_LOCK:
		lock_name = "file_mutex shared";
		break;
	case LockType::UNIQUE_FILE_LOCK:
		lock_name = "file_mutex unique";
		break;
	}
	const string function_lock_name = function_name + ": " + lock_name;
	boost::unique_lock<boost::shared_mutex> lock(logger_mutex);
	if (!lock_name_to_lock_access.count(lock_name)) {
		lock_name_to_lock_access.emplace(lock_name, 0);
	}
	if (!function_lock_name_to_lock_access.count(function_lock_name)) {
		function_lock_name_to_lock_access.emplace(function_lock_name, 0);
	}
	uint32_t lock_name_access_count = lock_name_to_lock_access[lock_name];
	lock_name_to_lock_access.emplace(lock_name, lock_name_access_count + 1);
	uint32_t function_lock_name_access_count = function_lock_name_to_lock_access[function_lock_name];
	function_lock_name_to_lock_access.emplace(function_lock_name, function_lock_name_access_count + 1);
}

void LockLogger::WriteToCSV(const string file_path) {
	ofstream file(file_path);
	if (!file.is_open()) {
		throw IOException("Unable to open CSV file for writing");
	}
	boost::unique_lock<boost::shared_mutex> lock(logger_mutex);
	for (const auto &[key, value] : lock_name_to_lock_access) {
		file << string_format("%s,%d", key, value);
	}
	for (const auto &[key, value] : function_lock_name_to_lock_access) {
		file << string_format("%s,%d", key, value);
	}
	file.close();
}
} // namespace duckdb
