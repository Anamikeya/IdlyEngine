#pragma once

// Bounded little-endian serialization. This is the only boundary through which network bytes
// become engine values: readers never allocate more than the caller's limits and never throw.
// Any overflow, truncation or limit violation sets a sticky failure flag; check ok() once at the end.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::net {

class ByteWriter {
public:
	explicit ByteWriter(std::size_t max_size = 64 * 1024);

	void write_u8(std::uint8_t v);
	void write_u16(std::uint16_t v);
	void write_u32(std::uint32_t v);
	void write_u64(std::uint64_t v);
	void write_i8(std::int8_t v);
	void write_i16(std::int16_t v);
	void write_i32(std::int32_t v);
	void write_i64(std::int64_t v);
	void write_f32(float v);
	void write_f64(double v);
	void write_bool(bool v);
	// LEB128, 1 to 10 bytes.
	void write_varuint(std::uint64_t v);
	void write_bytes(std::span<const std::byte> bytes);
	// varuint length prefix followed by UTF-8 bytes (not validated).
	void write_string(std::string_view s);

	[[nodiscard]] bool ok() const noexcept { return ok_; }
	[[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
	[[nodiscard]] std::span<const std::byte> bytes() const noexcept { return buffer_; }
	[[nodiscard]] std::vector<std::byte> take() && noexcept { return std::move(buffer_); }
	void clear() noexcept;

private:
	bool reserve(std::size_t count);

	std::vector<std::byte> buffer_;
	std::size_t max_size_;
	bool ok_ = true;
};

class ByteReader {
public:
	explicit ByteReader(std::span<const std::byte> data) noexcept : data_(data) {}

	// On failure each read returns a zero value and leaves ok() false.
	std::uint8_t read_u8() noexcept;
	std::uint16_t read_u16() noexcept;
	std::uint32_t read_u32() noexcept;
	std::uint64_t read_u64() noexcept;
	std::int8_t read_i8() noexcept;
	std::int16_t read_i16() noexcept;
	std::int32_t read_i32() noexcept;
	std::int64_t read_i64() noexcept;
	float read_f32() noexcept;
	double read_f64() noexcept;
	// Only 0 and 1 are accepted.
	bool read_bool() noexcept;
	// Rejects non-canonical over-long encodings and values above 64 bits.
	std::uint64_t read_varuint() noexcept;
	// Returns a view into the source buffer.
	std::span<const std::byte> read_bytes(std::size_t count) noexcept;
	// Fails if the encoded length exceeds max_length.
	std::string read_string(std::size_t max_length);

	[[nodiscard]] bool ok() const noexcept { return ok_; }
	[[nodiscard]] std::size_t remaining() const noexcept { return ok_ ? data_.size() - offset_ : 0; }
	[[nodiscard]] std::size_t offset() const noexcept { return offset_; }
	// True when every byte was consumed without error; use to reject trailing garbage.
	[[nodiscard]] bool fully_consumed() const noexcept { return ok_ && offset_ == data_.size(); }
	void fail() noexcept { ok_ = false; }

private:
	bool take(std::size_t count, const std::byte*& out) noexcept;
	std::uint64_t read_le(std::size_t count) noexcept;

	std::span<const std::byte> data_;
	std::size_t offset_ = 0;
	bool ok_ = true;
};

} // namespace engine::net
