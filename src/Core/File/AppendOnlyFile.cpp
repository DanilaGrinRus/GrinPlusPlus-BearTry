#include <Core/File/AppendOnlyFile.h>
#include <Core/Exceptions/FileException.h>
#include <Common/Util/FileUtil.h>

void AppendOnlyFile::Load()
{
	m_fileSize = FileUtil::GetFileSize(m_path);
	m_bufferIndex = m_fileSize;
	m_pMappedFile = IMappedFile::Load(m_path);
}

bool AppendOnlyFile::Flush()
{
	if (m_fileSize == m_bufferIndex && m_buffer.empty())
	{
		return true;
	}

	if (m_fileSize < m_bufferIndex)
	{
		return false;
	}

	if (!m_pMappedFile->Write(m_bufferIndex, m_buffer))
	{
		return false;
	}

	m_fileSize = m_bufferIndex + m_buffer.size();
	m_bufferIndex = m_fileSize;
	m_buffer.clear();

	return true;
}

void AppendOnlyFile::Append(const std::vector<unsigned char>& data)
{
	m_buffer.insert(m_buffer.end(), data.cbegin(), data.cend());
}

bool AppendOnlyFile::Rewind(const uint64_t nextPosition)
{
	// TODO: Shouldn't flush here - need to support multiple rewinds
	//if (!Flush())
	//{
	//	return false;
	//}

	if (nextPosition > (m_bufferIndex + m_buffer.size()))
	{
		return false;
	}

	if (nextPosition <= m_bufferIndex)
	{
		m_buffer.clear();
		m_bufferIndex = nextPosition;
	}
	else
	{
		m_buffer.erase(m_buffer.begin() + (nextPosition - m_bufferIndex), m_buffer.end());
	}

	return true;
}

void AppendOnlyFile::Discard() noexcept
{
	m_bufferIndex = m_fileSize;
	m_buffer.clear();
}

uint64_t AppendOnlyFile::GetSize() const noexcept
{
	return m_bufferIndex + m_buffer.size();
}

bool AppendOnlyFile::Read(const uint64_t position, const uint64_t numBytes, std::vector<unsigned char>& data) const
{
	const uint64_t size = GetSize();
	if (numBytes > size || position > size - numBytes) {
		return false;
	}

	// TODO: Handle partial read from m_mmap and m_buffer
	if (position < m_bufferIndex) {
		// This implementation only reads the mapped part or the buffered part;
		// a read spanning the boundary is rejected instead of over-reading.
		if (position + numBytes > m_bufferIndex) {
			return false;
		}

		m_pMappedFile->Read(position, numBytes, data);
		return true;
	}

	const uint64_t firstBufferIndex = position - m_bufferIndex;
	if (firstBufferIndex > m_buffer.size() || numBytes > m_buffer.size() - firstBufferIndex) {
		return false;
	}

	data = std::vector<unsigned char>(
		m_buffer.cbegin() + firstBufferIndex,
		m_buffer.cbegin() + firstBufferIndex + numBytes
	);
	return true;
}