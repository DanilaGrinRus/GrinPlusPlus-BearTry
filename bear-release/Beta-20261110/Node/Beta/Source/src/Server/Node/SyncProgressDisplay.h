#pragma once
#include <P2P/SyncStatus.h>
#include <sstream>
#include <string>

inline std::string FormatTxHashSetProgress(const SyncStatus& status)
{
    std::ostringstream out;
    if (status.GetPibdStarted() > 0) {
        const uint64_t received = status.GetPibdReceived();
        const uint64_t required = status.GetPibdRequired();
        out << "Status: Syncing PIBD segments " << received << "/";
        if (required == 0) out << "pending";
        else out << required << " (" << (100.0 * static_cast<double>(received) / static_cast<double>(required)) << "%)";
    } else {
        const uint64_t downloaded = status.GetDownloaded();
        const uint64_t size = status.GetDownloadSize();
        const uint64_t percent = size > 0 ? downloaded * 100 / size : 0;
        out << "Status: Syncing TxHashSet " << downloaded << "/" << size << "(" << percent << "%)";
    }
    return out.str();
}