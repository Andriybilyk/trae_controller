#pragma once

#include <string>

std::string kiln_fs_read_text(const char *path, bool lock_fs = true);
bool kiln_fs_write_text(const char *path, const std::string &data, bool sync = true, bool lock_fs = true);
bool kiln_fs_write_text_atomic(const char *path, const std::string &data, bool sync = true, bool lock_fs = true);
size_t kiln_fs_file_size(const char *path, bool lock_fs = true);
