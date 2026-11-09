#include <catch.hpp>

#include <Core/Traits/Lockable.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace
{
	class MockBatchable : public Traits::IBatchable
	{
	public:
		void OnInitWrite(const bool /*batch*/) override
		{
			SetDirty(true);
			if (m_failInit.load()) {
				throw std::runtime_error("init failed");
			}
		}

		void Commit() override
		{
			if (m_failCommit.load()) {
				throw std::runtime_error("commit failed");
			}
		}

		void Rollback() noexcept override
		{
		}

		void OnEndWrite() override
		{
			m_endWriteCalled = true;
		}

		std::atomic<bool> m_failInit{ false };
		std::atomic<bool> m_failCommit{ false };
		std::atomic<bool> m_endWriteCalled{ false };
	};
}

TEST_CASE("Locked<Writer> releases the mutex when OnInitWrite throws")
{
	auto pMock = std::make_shared<MockBatchable>();
	auto pLocked = std::make_shared<Locked<MockBatchable>>(pMock);

	pMock->m_failInit = true;
	REQUIRE_THROWS(pLocked->Write());

	// Before the fix the failed constructor above left the mutex locked and
	// this second acquisition deadlocked forever.
	pMock->m_failInit = false;
	std::atomic<bool> acquired{ false };
	std::thread secondWriter([pLocked, &acquired]() {
		auto writer = pLocked->Write();
		(void)writer;
		acquired = true;
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!acquired && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	if (acquired) {
		secondWriter.join();
	} else {
		// Keep the locked object (and its mutex) alive in the detached thread.
		// The test has already failed; never destroy a mutex while it is locked.
		secondWriter.detach();
	}

	REQUIRE(acquired.load());
}

TEST_CASE("Writer destructor logs commit failure and still calls OnEndWrite")
{
	auto pMock = std::make_shared<MockBatchable>();
	Locked<MockBatchable> locked(pMock);
	pMock->m_failCommit = true;

	{
		// Non-batched writer: destructor calls Commit(), which throws.
		auto writer = locked.Write();
		(void)writer;
	}

	// Before the fix the destructor swallowed the exception together with
	// OnEndWrite(); after the fix the error is logged and OnEndWrite() runs.
	REQUIRE(pMock->m_endWriteCalled.load());
}
