#include "temporary_file_metadata_manager.hpp"
#include <iostream>

namespace duckdb {

inline idx_t GetBufferSize(const string buffer_size_string) {
	if (!buffer_size_string.compare("S32K")) {
		return 32768;
	} else if (!buffer_size_string.compare("S64K")) {
		return 65536;
	} else if (!buffer_size_string.compare("S96K")) {
		return 98304;
	} else if (!buffer_size_string.compare("S128K")) {
		return 131072;
	} else if (!buffer_size_string.compare("S160K")) {
		return 163840;
	} else if (!buffer_size_string.compare("S192K")) {
		return 196608;
	} else if (!buffer_size_string.compare("S224K")) {
		return 229376;
	} else if (!buffer_size_string.compare("DEFAULT")) {
		return 262144;
	} else {
		throw InvalidInputException("Unknown buffer size %s", buffer_size_string.c_str());
	}
}

inline unique_ptr<TempFileMetadata> CreateTempFileMetadata(const string &filename) {
	//Expected filename format: /tmp/duckdb_temp_<buffer_size>-<file_index>.tmp
	unique_ptr<TempFileMetadata> tfmeta = make_uniq<TempFileMetadata>();
	tfmeta->is_active.store(true);

	// Find the position of the first number
	size_t first_number_start = filename.find_last_of('_') + 1;       // Start after the last '_'
	size_t first_number_end = filename.find('-', first_number_start); // Find the '-' after the first number

	// Extract the first number
	std::string block_size_str = filename.substr(first_number_start, first_number_end - first_number_start);
	idx_t block_size = GetBufferSize(block_size_str);

	
		// Find the position of the second number
	size_t file_index_start = first_number_end + 1;               // Start after the '-'
	size_t file_index_end = filename.find('.', file_index_start); // Find the '.' after the second number

	// Extract the second number
	std::string file_index_str = filename.substr(file_index_start, file_index_end - file_index_start);
	int file_index = std::stoi(file_index_str);
	
	tfmeta->block_size = block_size;
	tfmeta->file_index = file_index;
	tfmeta->nr_blocks = (1 << file_index) * 4000;
	tfmeta->lba_location.store(0);
	// tfmeta->block_range = nullptr;

	return std::move(tfmeta);
}

// boost::shared_mutex TemporaryFileMetadataManager::temp_mutex;

const TempFileMetadata *TemporaryFileMetadataManager::GetOrCreateFile(const string &filename) {
	// Lock the shared mutex for writing
	{
		boost::shared_lock<boost::shared_mutex> alloc_lock(temp_mutex);

		// Check if the file already exists
		// Use find() for safety
		auto it = file_to_temp_meta.find(filename);
		if (it != file_to_temp_meta.end()) {
			return it->second.get();
		}
	}

	boost::unique_lock<boost::shared_mutex> lock(temp_mutex);

	//Double check if another thread created the file while trying acquire unique lock
	auto it = file_to_temp_meta.find(filename);
    if (it != file_to_temp_meta.end()) {
        return it->second.get();
    }

	// Create a new TempFileMetadata object
	unique_ptr<TempFileMetadata> tfmeta = CreateTempFileMetadata(filename);
	auto [entry, is_new] = file_to_temp_meta.emplace(filename, std::move(tfmeta));

	// Use entry instead of fail_to_temp_meta, as it is an unnecessary extra lookup.
	return entry->second.get();
}

void TemporaryFileMetadataManager::CreateFile(const string &filename) {
	GetOrCreateFile(filename);
}

idx_t TemporaryFileMetadataManager::GetLBA(const string &filename, idx_t location, idx_t nr_lbas) {
	// We only read file_to_temp_meta to find the file's tfmeta
	// Hold the lock for the duration of function to ensure 'tfmeta' does not get changed
	boost::shared_lock<boost::shared_mutex> global_lock(temp_mutex);

	//Should we use find() instead of operator[]?
	//TempFileMetadata *tfmeta = file_to_temp_meta[filename].get();
	// operator[] can be unsafe with read lock only. as it tries to insert if file does not exist
	auto entry = file_to_temp_meta.find(filename);
	if (entry == file_to_temp_meta.end()) {
		throw IOException("Temporary file not found: " + filename);
	}
	TempFileMetadata *tfmeta = entry->second.get();
	

	// Start by assuming the block exists
	{
		boost::shared_lock<boost::shared_mutex> file_read_lock(tfmeta->file_mutex);
		idx_t block_index = location / tfmeta->block_size;

		if (nr_lbas != (tfmeta->block_size / lba_size)) {
			throw IOException("Temporary file block size mismatch");
		}

		//Changed to use find() instead of operator[] due to being unsafe with read lock
		auto it = tfmeta->block_map.find(block_index);
		if (it != tfmeta->block_map.end()) {
			// Release both locks if block exists, by returning Start LBA
			return it->second->GetStartLBA();
		}
	}

	// The block does not exist
	boost::unique_lock<boost::shared_mutex> file_write_lock(tfmeta->file_mutex);
	idx_t block_index = location / tfmeta->block_size;

	if (!tfmeta->block_map.count(block_index)) {
		TemporaryBlock *block = block_manager->AllocateBlock(nr_lbas);
		tfmeta->block_map[block_index] = block;
		total_allocated_blocks.fetch_add(nr_lbas, std::memory_order_relaxed);
	}

	return tfmeta->block_map[block_index]->GetStartLBA();
}

void TemporaryFileMetadataManager::MoveLBALocation(const string &filename, idx_t lba_location) {
	// boost::shared_lock<boost::shared_mutex> lock(temp_mutex);

	// if (!file_to_temp_meta.count(filename)) {
	// 	return;
	// }

	// TempFileMetadata *tfmeta = file_to_temp_meta[filename].get();
	// boost::shared_lock<boost::shared_mutex> file_lock(tfmeta->file_mutex);

	// // Use atomic compare-and-swap to update lba_location if the new location is larger
	// idx_t current_lba = tfmeta->lba_location.load();
	// do {
	// 	// Location does not need to be updated from this thread anymore
	// 	// Another thread have surpassed it
	// 	if (lba_location < current_lba) {
	// 		printf("MoveLBALocation %s, location %d\n", filename.c_str(), current_lba);
	// 		break;
	// 	}
	// } while (!tfmeta->lba_location.compare_exchange_weak(current_lba, lba_location));

	// printf("MoveLBALocation %s, location %d\n", filename.c_str(), lba_location);
}

void TemporaryFileMetadataManager::TruncateFile(const string &filename, idx_t new_size) {
	// As we do not modify file_to_temp_meta but only read the file
	// Allow others to read other files
	boost::shared_lock<boost::shared_mutex> global_lock(temp_mutex);

	auto entry = file_to_temp_meta.find(filename);
	if (entry == file_to_temp_meta.end()) {
		return; //File does not exist; perhaps we should throw ioexception
	}
	TempFileMetadata *tfmeta = entry->second.get();

	// Only lock this file
	boost::unique_lock<boost::shared_mutex> file_lock(tfmeta->file_mutex);

	idx_t to_block_index = new_size / tfmeta->block_size;
	idx_t from_block_index = tfmeta->block_map.size();

	for (idx_t i = from_block_index; i > to_block_index; i--) {
		idx_t block_index = i - 1;
		// Use find() for safety
		auto it = tfmeta->block_map.find(block_index);
		if (it != tfmeta->block_map.end()) {
			TemporaryBlock* block = it->second;
			total_allocated_blocks.fetch_sub((tfmeta->block_size / lba_size), std::memory_order_relaxed);

			block_manager->FreeBlock(block);
			tfmeta->block_map.erase(it);
		}
	}
}

void TemporaryFileMetadataManager::DeleteFile(const string &filename) {
	boost::unique_lock<boost::shared_mutex> lock(temp_mutex);

	//Use find() 
	auto entry = file_to_temp_meta.find(filename);
    if (entry == file_to_temp_meta.end()) {
        return; // File already deleted, nothing to do
    }
	// Since we delete file we are allowed extract ownership to local scope, and remove the entry
	// Which allows us to release the unique lock earlier
	unique_ptr<TempFileMetadata> local_tfmeta = std::move(entry->second);
	file_to_temp_meta.erase(entry);
	lock.unlock();

	//No file_mutex is needed, as tfmeta is local now
	for (const auto &kv : local_tfmeta->block_map) {
		total_allocated_blocks.fetch_sub((local_tfmeta->block_size / lba_size), std::memory_order_relaxed);
		block_manager->FreeBlock(kv.second);
	}
	
}

bool TemporaryFileMetadataManager::FileExists(const string &filename) {
	boost::shared_lock<boost::shared_mutex> lock(temp_mutex);

	if (file_to_temp_meta.count(filename)) {
		return true;
	}

	return false;
}

idx_t TemporaryFileMetadataManager::GetFileSizeLBA(const string &filename) {
	boost::shared_lock<boost::shared_mutex> lock(temp_mutex);

	//Use find() for thread safety
	auto entry = file_to_temp_meta.find(filename);
	if (entry == file_to_temp_meta.end()) {
		return 0; // Potentially thow IOException("File not found") instead
	}
	TempFileMetadata *tfmeta = entry->second.get();

	boost::shared_lock<boost::shared_mutex> file_lock(tfmeta->file_mutex);

	idx_t nr_lbas = (tfmeta->block_size * tfmeta->block_map.size()) / lba_size;
	return nr_lbas;

}

void TemporaryFileMetadataManager::Clear() {
	boost::unique_lock<boost::shared_mutex> alloc_lock(temp_mutex);

	//Extract global map into a local variable
	map<string, unique_ptr<TempFileMetadata>> local_map;
	file_to_temp_meta.swap(local_map);

	total_allocated_blocks = 0;

	alloc_lock.unlock();

	// own local_Map exlusively so no locks needed
	for (const auto &kv : local_map) {
		TempFileMetadata *tfmeta = kv.second.get();

		for (const auto &block : tfmeta->block_map) {
			block_manager->FreeBlock(block.second);
		}
	}
	//Do I need to clear() local_map?
}

idx_t TemporaryFileMetadataManager::GetSeekBound(const string &filename) {
	boost::shared_lock<boost::shared_mutex> lock(temp_mutex);

	//Use find() for thread safety
	auto entry = file_to_temp_meta.find(filename);
	if (entry == file_to_temp_meta.end()) {
		return 0;
	}
	TempFileMetadata *tfmeta = entry->second.get();

	boost::shared_lock<boost::shared_mutex> file_lock(tfmeta->file_mutex);

	return tfmeta->block_size * tfmeta->block_map.size();
}

idx_t TemporaryFileMetadataManager::GetAvailableSpace(idx_t lba_count, idx_t lba_start) {
	//Use shared lock instead of unique lock, allowing others thread to create/delete/write files
	boost::shared_lock<boost::shared_mutex> temp_lock(temp_mutex);

	idx_t temp_max_bytes = ((lba_count - 1) - lba_start) * lba_size;
	//Atomic read, instead of going through entire file_to_temp_meta
	idx_t used_bytes = total_allocated_blocks.load() * lba_size;
	
	if (used_bytes > temp_max_bytes) return 0;
	return (temp_max_bytes - used_bytes);
}

void TemporaryFileMetadataManager::ListFiles(const string &directory,
                                             const std::function<void(const string &, bool)> &callback) {
	//USe shared lock instead of uniqe lock
	boost::shared_lock<boost::shared_mutex> lock(temp_mutex);

	for (const auto &kv : file_to_temp_meta) {
		callback(StringUtil::GetFileName(kv.first), false);
	}
}

} // namespace duckdb
