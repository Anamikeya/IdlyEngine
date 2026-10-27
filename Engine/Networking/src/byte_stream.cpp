#include "engine/net/byte_stream.h"

#include <bit>
#include <cstring>

namespace engine::net {

ByteWriter::ByteWriter(std::size_t max_size) : max_size_(max_size) {}

bool ByteWriter::reserve(std::size_t count)
{
	if (!ok_ || count > max_size_ - buffer_.size()) {
		ok_ = false;
		return false;
	}
	return true;
}

void ByteWriter::clear() noexcept
{
	buffer_.clear();
	ok_ = true;
}

void ByteWriter::write_u8(std::uint8_t v)
{
	if (reserve(1)) {
		buffer_.push_back(static_cast<std::byte>(v));
	}
}

void ByteWriter::write_u16(std::uint16_t v)
{
	if (reserve(2)) {
		for (int i = 0; i < 2; ++i) {
			buffer_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xffu));
		}
	}
}

void ByteWriter::write_u32(std::uint32_t v)
{
	if (reserve(4)) {
		for (int i = 0; i < 4; ++i) {
			buffer_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xffu));
		}
	}
}

void ByteWriter::write_u64(std::uint64_t v)
{
	if (reserve(8)) {
		for (int i = 0; i < 8; ++i) {
			buffer_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xffu));
		}
	}
}

void ByteWriter::write_i8(std::int8_t v)
{
	write_u8(static_cast<std::uint8_t>(v));
}

void ByteWriter::write_i16(std::int16_t v)
{
	write_u16(static_cast<std::uint16_t>(v));
}

void ByteWriter::write_i32(std::int32_t v)
{
	write_u32(static_cast<std::uint32_t>(v));
}

void ByteWriter::write_i64(std::int64_t v)
{
	write_u64(static_cast<std::uint64_t>(v));
}

void ByteWriter::write_f32(float v)
{
	write_u32(std::bit_cast<std::uint32_t>(v));
}

void ByteWriter::write_f64(double v)
{
	write_u64(std::bit_cast<std::uint64_t>(v));
}

void ByteWriter::write_bool(bool v)
{
	write_u8(v ? 1 : 0);
}

void ByteWriter::write_varuint(std::uint64_t v)
{
	do {
		std::uint8_t byte = static_cast<std::uint8_t>(v & 0x7fu);
		v >>= 7;
		if (v != 0) {
			byte |= 0x80u;
		}
		write_u8(byte);
	} while (v != 0 && ok_);
}

void ByteWriter::write_bytes(std::span<const std::byte> bytes)
{
	if (reserve(bytes.size())) {
		buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
	}
}

void ByteWriter::write_string(std::string_view s)
{
	write_varuint(s.size());
	write_bytes(std::as_bytes(std::span<const char>(s.data(), s.size())));
}

bool ByteReader::take(std::size_t count, const std::byte*& out) noexcept
{
	if (!ok_ || count > data_.size() - offset_) {
		ok_ = false;
		return false;
	}
	out = data_.data() + offset_;
	offset_ += count;
	return true;
}

std::uint64_t ByteReader::read_le(std::size_t count) noexcept
{
	const std::byte* p = nullptr;
	if (!take(count, p)) {
		return 0;
	}
	std::uint64_t v = 0;
	for (std::size_t i = 0; i < count; ++i) {
		v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
	}
	return v;
}

std::uint8_t ByteReader::read_u8() noexcept
{
	return static_cast<std::uint8_t>(read_le(1));
}

std::uint16_t ByteReader::read_u16() noexcept
{
	return static_cast<std::uint16_t>(read_le(2));
}

std::uint32_t ByteReader::read_u32() noexcept
{
	return static_cast<std::uint32_t>(read_le(4));
}

std::uint64_t ByteReader::read_u64() noexcept
{
	return read_le(8);
}

std::int8_t ByteReader::read_i8() noexcept
{
	return static_cast<std::int8_t>(read_u8());
}

std::int16_t ByteReader::read_i16() noexcept
{
	return static_cast<std::int16_t>(read_u16());
}

std::int32_t ByteReader::read_i32() noexcept
{
	return static_cast<std::int32_t>(read_u32());
}

std::int64_t ByteReader::read_i64() noexcept
{
	return static_cast<std::int64_t>(read_u64());
}

float ByteReader::read_f32() noexcept
{
	return std::bit_cast<float>(read_u32());
}

double ByteReader::read_f64() noexcept
{
	return std::bit_cast<double>(read_u64());
}

bool ByteReader::read_bool() noexcept
{
	const std::uint8_t v = read_u8();
	if (v > 1) {
		ok_ = false;
		return false;
	}
	return v == 1;
}

std::uint64_t ByteReader::read_varuint() noexcept
{
	std::uint64_t value = 0;
	for (int shift = 0; shift < 64; shift += 7) {
		const std::uint8_t byte = read_u8();
		if (!ok_) {
			return 0;
		}
		const std::uint64_t bits = byte & 0x7fu;
		// The 10th byte may only carry the single remaining bit.
		if (shift == 63 && bits > 1) {
			break;
		}
		value |= bits << shift;
		if ((byte & 0x80u) == 0) {
			// A zero final byte after the first is an over-long encoding.
			if (shift > 0 && byte == 0) {
				break;
			}
			return value;
		}
	}
	ok_ = false;
	return 0;
}

std::span<const std::byte> ByteReader::read_bytes(std::size_t count) noexcept
{
	const std::byte* p = nullptr;
	if (!take(count, p)) {
		return {};
	}
	return {p, count};
}

std::string ByteReader::read_string(std::size_t max_length)
{
	const std::uint64_t length = read_varuint();
	if (!ok_ || length > max_length) {
		ok_ = false;
		return {};
	}
	const auto bytes = read_bytes(static_cast<std::size_t>(length));
	if (!ok_) {
		return {};
	}
	std::string s(bytes.size(), '\0');
	std::memcpy(s.data(), bytes.data(), bytes.size());
	return s;
}

} // namespace engine::net
