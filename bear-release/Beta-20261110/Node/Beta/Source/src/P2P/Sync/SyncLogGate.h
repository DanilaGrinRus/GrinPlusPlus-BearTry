#pragma once

//
// Throttling for repeated sync-state transition warnings.
//
// StateSyncer::IsStateSyncDue() is called on every sync-loop iteration (~10 ms).
// Several of its warnings describe a *transition* (sync peer lost, transfer
// failed, no progress, download timeout), but the underlying condition stays
// true while the 30-second re-request guard blocks RequestState(). Logging them
// on every iteration flooded Node.log: 15 306 "Sync peer no longer connected."
// lines / 1.66 MB in ~6.5 minutes during the task-64 live stand.
//
// Each warning is therefore emitted once per "request epoch". A request that is
// actually sent (RequestState success) starts a new epoch and re-arms all
// transition logs, so diagnostics are preserved: one line per transition, not
// one line per loop iteration. Sync/retry behaviour is not changed.
//
class SyncLogGate
{
public:
	void OnRequestSent() noexcept
	{
		m_peerLostLogged = false;
		m_syncFailedLogged = false;
		m_noProgressLogged = false;
		m_downloadTimeoutLogged = false;
	}

	bool ShouldLogPeerLost() noexcept
	{
		return MarkOnce(m_peerLostLogged);
	}

	bool ShouldLogSyncFailed() noexcept
	{
		return MarkOnce(m_syncFailedLogged);
	}

	bool ShouldLogNoProgress() noexcept
	{
		return MarkOnce(m_noProgressLogged);
	}

	bool ShouldLogDownloadTimeout() noexcept
	{
		return MarkOnce(m_downloadTimeoutLogged);
	}

private:
	static bool MarkOnce(bool& logged) noexcept
	{
		if (logged) {
			return false;
		}
		logged = true;
		return true;
	}

	bool m_peerLostLogged = false;
	bool m_syncFailedLogged = false;
	bool m_noProgressLogged = false;
	bool m_downloadTimeoutLogged = false;
};
