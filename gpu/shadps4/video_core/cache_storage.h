// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    void Open();
    void Close();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();
    /// gow3: removes every cached blob (an incompatible cache is rebuilt, not ignored).
    void Clear();

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    /// gow3: `stop` returning true ends the walk early (background warm-up at exit).
    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func,
                     const std::function<bool()>& stop = {});
    /// gow3: ForEachBlob that also passes each blob's name (no extension).
    void ForEachNamedBlob(
        BlobType type,
        const std::function<void(std::string_view name, std::vector<u8>&& data)>& func,
        const std::function<bool()>& stop = {});
    /// gow3: deletes one blob (loose files only; the zip archive is left as it is).
    void Remove(BlobType type, std::string_view name);
    /// gow3: blobs are plain files written beside and renamed into place, so other threads
    /// may read them while the IO thread saves (the zip archive cannot).
    [[nodiscard]] bool ConcurrentReads() const;
    /// gow3: number of blobs ForEachBlob visits (the loading screen's total).
    size_t CountBlobs(BlobType type);
    /// gow3: the name (no extension) of every blob of a type, without reading it.
    void ForEachBlobName(BlobType type, const std::function<void(std::string_view name)>& func);
    /// gow3: reads every pipeline key, then every shader blob, on several threads ahead of the
    /// warm-up, so the system file cache holds them when the warm-up's single reader asks (on a
    /// hard drive or with an antivirus scanning each open, one file at a time took 30 ms: #84).
    void StartPrefetch();
    void StopPrefetch();

private:
    std::vector<std::jthread> prefetch_threads{};
    std::jthread io_worker{};
    std::filesystem::path cache_path{};
    bool opened{};
};

} // namespace Storage
