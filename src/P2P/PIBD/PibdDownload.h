#pragma once

#include "BitmapSegmentAssembler.h"
#include "PibdParams.h"
#include "PibdTreeAssembler.h"

#include "../../PMMR/Models/BitmapSegment.h"
#include "../../PMMR/Models/Segment.h"
#include "../../PMMR/Models/SegmentException.h"
#include "../../PMMR/Models/SegmentIdentifier.h"

#include <Core/Models/OutputIdentifier.h>
#include <Core/Models/TransactionKernel.h>
#include <Crypto/Models/Hash.h>
#include <Crypto/Models/RangeProof.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

// PIBD download orchestration: bitmap phase first, then outputs, rangeproofs and
// kernels at the same horizon.
//
// The class is transport-agnostic: NextRequests() produces the segment requests
// to send (ids 21/23/25/27) and the Accept* methods consume validated responses.
// Timers, peer selection and retries belong to the network layer.
//
// Payload types are template parameters so the orchestration can be exercised
// with lightweight test elements; the node uses the alias at the bottom.
template<class TOutput, class TRangeProof, class TKernel>
class PibdDownloadT
{
public:
	enum class ETree
	{
		Bitmap = 0,
		Output = 1,
		RangeProof = 2,
		Kernel = 3
	};

	enum class EPhase
	{
		Bitmap,
		Trees,
		Complete
	};

	struct SegmentRequest
	{
		ETree Tree;
		SegmentIdentifier Identifier;

		bool operator==(const SegmentRequest& rhs) const noexcept
		{
			return Tree == rhs.Tree && Identifier == rhs.Identifier;
		}
	};

	struct Horizon
	{
		Hash BlockHash;
		uint64_t Height = 0;
		uint64_t OutputMmrSize = 0;   // also the rangeproof MMR size (one proof per output)
		uint64_t KernelMmrSize = 0;
		// Header output_root: H(output_mmr_size || output_pmmr_root || bitmap_root).
		Hash OutputRoot;
		Hash RangeProofRoot;
		Hash KernelRoot;
	};

	explicit PibdDownloadT(Horizon horizon)
		: m_horizon(std::move(horizon)),
		m_bitmap(m_horizon.OutputMmrSize, m_horizon.OutputRoot),
		m_phase(EPhase::Bitmap)
	{
	}

	const Horizon& GetHorizon() const noexcept { return m_horizon; }
	EPhase GetPhase() const noexcept { return m_phase; }
	bool IsComplete() const noexcept { return m_phase == EPhase::Complete; }

	const BitmapSegmentAssembler& GetBitmapAssembler() const noexcept { return m_bitmap; }

	const PibdTreeAssembler<TOutput>& GetOutputs() const
	{
		RequireTrees();
		return *m_outputs;
	}

	const PibdTreeAssembler<TRangeProof>& GetRangeProofs() const
	{
		RequireTrees();
		return *m_rangeProofs;
	}

	const PibdTreeAssembler<TKernel>& GetKernels() const
	{
		RequireTrees();
		return *m_kernels;
	}

	// Bitmap assembled during phase 1; only available once the bitmap is complete.
	const VectorSegmentBitmap& GetOutputBitmap() const
	{
		if (!m_bitmapObject.has_value()) {
			throw SegmentException(ESegmentError::NonExistent, "Output bitmap is not assembled yet");
		}

		return m_bitmapObject.value();
	}

	const Hash& GetOutputMmrRoot() const { return m_outputMmrRoot; }

	size_t GetReceivedCount() const
	{
		size_t received = m_bitmap.GetReceivedSegments();
		if (m_outputs != nullptr) {
			received += m_outputs->GetReceivedSegments();
			received += m_rangeProofs->GetReceivedSegments();
			received += m_kernels->GetReceivedSegments();
		}

		return received;
	}

	size_t GetRequiredCount() const
	{
		size_t required = m_bitmap.GetRequiredSegments();
		if (m_outputs != nullptr) {
			required += m_outputs->GetRequiredSegments();
			required += m_rangeProofs->GetRequiredSegments();
			required += m_kernels->GetRequiredSegments();
		}

		return required;
	}

	// Next requests to issue, honouring the bitmap-first rule and interleaving the
	// three trees afterwards.
	std::vector<SegmentRequest> NextRequests(const size_t maxCount) const
	{
		std::vector<SegmentRequest> requests;
		if (maxCount == 0 || m_phase == EPhase::Complete) {
			return requests;
		}

		if (m_phase == EPhase::Bitmap) {
			for (const SegmentIdentifier& identifier : m_bitmap.GetMissing()) {
				if (requests.size() >= maxCount) {
					break;
				}

				requests.push_back({ ETree::Bitmap, identifier });
			}

			return requests;
		}

		const std::vector<SegmentIdentifier> missing[3] = {
			m_outputs->GetMissing(),
			m_rangeProofs->GetMissing(),
			m_kernels->GetMissing()
		};

		size_t round = 0;
		while (requests.size() < maxCount) {
			bool added = false;
			for (size_t tree = 0; tree < 3 && requests.size() < maxCount; ++tree) {
				if (round < missing[tree].size()) {
					requests.push_back({
						(ETree)((int)ETree::Output + (int)tree),
						missing[tree][round]
					});
					added = true;
				}
			}

			if (!added) {
				break;
			}

			++round;
		}

		return requests;
	}

	// Returns false for a late/duplicate bitmap segment, true when newly accepted.
	// `outputMmrRoot` is carried by the bitmap response and authenticated here
	// against the header output root together with the bitmap accumulator root.
	bool AcceptBitmapSegment(const BitmapSegment& segment, const Hash& outputMmrRoot)
	{
		if (m_phase != EPhase::Bitmap) {
			return false;
		}

		const bool accepted = m_bitmap.Accept(segment, outputMmrRoot);
		if (m_bitmap.IsComplete()) {
			m_bitmapObject = m_bitmap.BuildBitmap();
			m_outputMmrRoot = m_bitmap.GetOutputMmrRoot();
			const Hash bitmapMmrRoot = m_bitmap.BuildBitmapMmrRoot();
			m_outputs = std::make_unique<PibdTreeAssembler<TOutput>>(
				m_horizon.OutputMmrSize,
				m_horizon.OutputRoot,
				PibdParams::OUTPUT_SEGMENT_HEIGHT,
				&m_bitmapObject.value(),
				bitmapMmrRoot,
				m_horizon.OutputMmrSize,
				false);
			m_rangeProofs = std::make_unique<PibdTreeAssembler<TRangeProof>>(
				m_horizon.OutputMmrSize,
				m_horizon.RangeProofRoot,
				PibdParams::RANGEPROOF_SEGMENT_HEIGHT,
				&m_bitmapObject.value());
			m_kernels = std::make_unique<PibdTreeAssembler<TKernel>>(
				m_horizon.KernelMmrSize,
				m_horizon.KernelRoot,
				PibdParams::KERNEL_SEGMENT_HEIGHT,
				nullptr);
			m_phase = EPhase::Trees;
		}

		return accepted;
	}

	bool AcceptOutputSegment(const Segment<TOutput>& segment)
	{
		RequireTrees();
		const bool accepted = m_outputs->Accept(segment);
		UpdateComplete();
		return accepted;
	}

	bool AcceptRangeProofSegment(const Segment<TRangeProof>& segment)
	{
		RequireTrees();
		const bool accepted = m_rangeProofs->Accept(segment);
		UpdateComplete();
		return accepted;
	}

	bool AcceptKernelSegment(const Segment<TKernel>& segment)
	{
		RequireTrees();
		const bool accepted = m_kernels->Accept(segment);
		UpdateComplete();
		return accepted;
	}

private:
	void RequireTrees() const
	{
		if (m_outputs == nullptr) {
			throw SegmentException(ESegmentError::NonExistent, "Bitmap phase is not complete yet");
		}
	}

	void UpdateComplete()
	{
		if (m_outputs->IsComplete() && m_rangeProofs->IsComplete() && m_kernels->IsComplete()) {
			m_phase = EPhase::Complete;
		}
	}

	Horizon m_horizon;
	BitmapSegmentAssembler m_bitmap;
	Hash m_outputMmrRoot;
	std::optional<VectorSegmentBitmap> m_bitmapObject;
	std::unique_ptr<PibdTreeAssembler<TOutput>> m_outputs;
	std::unique_ptr<PibdTreeAssembler<TRangeProof>> m_rangeProofs;
	std::unique_ptr<PibdTreeAssembler<TKernel>> m_kernels;
	EPhase m_phase;
};

// The concrete PIBD download used by the node.
using PibdDownload = PibdDownloadT<OutputIdentifier, RangeProof, TransactionKernel>;
