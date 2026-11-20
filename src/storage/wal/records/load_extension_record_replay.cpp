#include <cstdio>

#include "extension/extension_manager.h"
#include "storage/wal/wal_replayer.h"

using namespace lbug::storage;

namespace lbug {
namespace storage {

void WALReplayer::replayLoadExtensionRecord(const WALRecord& walRecord) const {
    const auto& loadExtensionRecord = walRecord.constCast<LoadExtensionRecord>();
    // gh-1612: a WAL written by an older release can reference an extension binary built
    // for that release, or an absolute path that no longer exists. Loading it here
    // segfaults on close (ABI mismatch) or fails the open entirely (missing file),
    // wedging an in-place upgrade. Skip it with a warning instead: the database opens
    // and the extension simply stays unloaded until the user LOADs a matching build.
    try {
        extension::ExtensionManager::Get(clientContext)
            ->loadExtension(loadExtensionRecord.path, &clientContext);
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "Warning: skipping WAL replay of LOAD EXTENSION '%s': %s. "
            "Load a matching build explicitly if needed.\n",
            loadExtensionRecord.path.c_str(), e.what());
    }
}

} // namespace storage
} // namespace lbug
