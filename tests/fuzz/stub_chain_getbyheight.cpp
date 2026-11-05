// Test-only link stub for the ASan "TestsMini" executable.
//
// TestsMini links the ASan-built Common/Core/Crypto/PMMR libraries and compiles
// selected tests (NRD differential, kernel-index rebuild, segment, P2P compat).
// PMMR.lib(TxHashSetImpl.obj) references exactly one BlockChain symbol,
// Chain::GetByHeight, from TxHashSet::SaveOutputPositions. Linking the real
// BlockChain lib would pull Database -> the uninstrumented rocksdb.lib C++
// library and reintroduce the LNK2038 annotate_string/annotate_vector mismatch
// this build exists to eliminate.
//
// The covered tests never call TxHashSet::SaveOutputPositions, so this stub is
// never executed. It exists only to satisfy the linker. Do not use this stub in
// any other executable and do not treat TestsMini as coverage of output-position
// persistence (that path stays with the ordinary build's tests).
#include <BlockChain/Chain.h>

std::shared_ptr<const BlockIndex> Chain::GetByHeight(const uint64_t) const
{
	return nullptr;
}
