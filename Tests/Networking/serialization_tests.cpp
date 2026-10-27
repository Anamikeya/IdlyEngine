#include "engine/net/byte_stream.h"
#include "engine/test/test.h"

#include <array>
#include <cmath>
#include <limits>

using namespace engine::net;

namespace {

std::vector<std::byte> bytes(std::initializer_list<int> values)
{
	std::vector<std::byte> out;
	for (int v : values) {
		out.push_back(static_cast<std::byte>(v));
	}
	return out;
}

} // namespace

ENGINE_TEST(roundtrip_all_scalar_types)
{
	ByteWriter w;
	w.write_u8(0xab);
	w.write_u16(0xbeef);
	w.write_u32(0xdeadbeefu);
	w.write_u64(0x0123456789abcdefull);
	w.write_i8(-5);
	w.write_i16(-30000);
	w.write_i32(std::numeric_limits<std::int32_t>::min());
	w.write_i64(-1);
	w.write_f32(3.5f);
	w.write_f64(-0.0);
	w.write_bool(true);
	w.write_bool(false);
	ENGINE_REQUIRE(w.ok());

	ByteReader r(w.bytes());
	ENGINE_CHECK_EQ(r.read_u8(), 0xab);
	ENGINE_CHECK_EQ(r.read_u16(), 0xbeef);
	ENGINE_CHECK_EQ(r.read_u32(), 0xdeadbeefu);
	ENGINE_CHECK_EQ(r.read_u64(), 0x0123456789abcdefull);
	ENGINE_CHECK_EQ(r.read_i8(), -5);
	ENGINE_CHECK_EQ(r.read_i16(), -30000);
	ENGINE_CHECK_EQ(r.read_i32(), std::numeric_limits<std::int32_t>::min());
	ENGINE_CHECK_EQ(r.read_i64(), -1);
	ENGINE_CHECK_EQ(r.read_f32(), 3.5f);
	const double negative_zero = r.read_f64();
	ENGINE_CHECK(negative_zero == 0.0 && std::signbit(negative_zero));
	ENGINE_CHECK(r.read_bool());
	ENGINE_CHECK(!r.read_bool());
	ENGINE_CHECK(r.fully_consumed());
}

ENGINE_TEST(encoding_is_little_endian)
{
	ByteWriter w;
	w.write_u32(0x11223344u);
	ENGINE_CHECK(std::vector<std::byte>(w.bytes().begin(), w.bytes().end()) == bytes({0x44, 0x33, 0x22, 0x11}));
}

ENGINE_TEST(nan_payload_survives_roundtrip)
{
	ByteWriter w;
	w.write_f32(std::numeric_limits<float>::quiet_NaN());
	ByteReader r(w.bytes());
	ENGINE_CHECK(std::isnan(r.read_f32()));
}

ENGINE_TEST(varuint_roundtrip_and_sizes)
{
	const std::array<std::pair<std::uint64_t, std::size_t>, 6> cases{{
		{0, 1}, {127, 1}, {128, 2}, {16383, 2}, {16384, 3}, {std::numeric_limits<std::uint64_t>::max(), 10},
	}};
	for (const auto& [value, size] : cases) {
		ByteWriter w;
		w.write_varuint(value);
		ENGINE_CHECK_EQ(w.size(), size);
		ByteReader r(w.bytes());
		ENGINE_CHECK_EQ(r.read_varuint(), value);
		ENGINE_CHECK(r.fully_consumed());
	}
}

ENGINE_TEST(varuint_rejects_overlong_and_overflow)
{
	const auto overlong = bytes({0x80, 0x00}); // zero encoded in two bytes
	ByteReader a(overlong);
	a.read_varuint();
	ENGINE_CHECK(!a.ok());

	const auto overflow = bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02});
	ByteReader b(overflow);
	b.read_varuint();
	ENGINE_CHECK(!b.ok());

	const auto unterminated = bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01});
	ByteReader c(unterminated);
	c.read_varuint();
	ENGINE_CHECK(!c.ok());

	const auto truncated = bytes({0x80});
	ByteReader d(truncated);
	d.read_varuint();
	ENGINE_CHECK(!d.ok());
}

ENGINE_TEST(string_roundtrip_and_length_limit)
{
	ByteWriter w;
	w.write_string("hello, engine");
	w.write_string("");
	ByteReader r(w.bytes());
	ENGINE_CHECK_EQ(r.read_string(64), std::string("hello, engine"));
	ENGINE_CHECK_EQ(r.read_string(64), std::string());
	ENGINE_CHECK(r.fully_consumed());

	ByteReader limited(w.bytes());
	ENGINE_CHECK(limited.read_string(4).empty());
	ENGINE_CHECK(!limited.ok());
}

ENGINE_TEST(string_length_beyond_buffer_does_not_allocate)
{
	ByteWriter w;
	w.write_varuint(std::uint64_t{1} << 40); // claims a terabyte
	ByteReader r(w.bytes());
	ENGINE_CHECK(r.read_string(std::numeric_limits<std::size_t>::max()).empty());
	ENGINE_CHECK(!r.ok());
}

ENGINE_TEST(reader_truncation_is_sticky)
{
	const auto data = bytes({1, 2, 3});
	ByteReader r(data);
	ENGINE_CHECK_EQ(r.read_u16(), 0x0201);
	ENGINE_CHECK_EQ(r.read_u32(), 0u); // only one byte left
	ENGINE_CHECK(!r.ok());
	ENGINE_CHECK_EQ(r.read_u8(), 0); // stays failed even though a byte remains
	ENGINE_CHECK_EQ(r.remaining(), 0u);
	ENGINE_CHECK(!r.fully_consumed());
}

ENGINE_TEST(reader_rejects_invalid_bool)
{
	const auto data = bytes({2});
	ByteReader r(data);
	r.read_bool();
	ENGINE_CHECK(!r.ok());
}

ENGINE_TEST(reader_detects_trailing_bytes)
{
	const auto data = bytes({1, 0, 9});
	ByteReader r(data);
	r.read_u16();
	ENGINE_CHECK(r.ok());
	ENGINE_CHECK(!r.fully_consumed());
}

ENGINE_TEST(writer_enforces_max_size)
{
	ByteWriter w(5);
	w.write_u32(1);
	ENGINE_CHECK(w.ok());
	w.write_u16(2); // would make 6 bytes
	ENGINE_CHECK(!w.ok());
	ENGINE_CHECK_EQ(w.size(), 4u); // nothing partial was written
	w.write_u8(3);
	ENGINE_CHECK(!w.ok()); // sticky
	w.clear();
	ENGINE_CHECK(w.ok());
	ENGINE_CHECK_EQ(w.size(), 0u);
}

ENGINE_TEST(read_bytes_returns_view)
{
	const auto data = bytes({7, 8, 9});
	ByteReader r(data);
	const auto view = r.read_bytes(2);
	ENGINE_CHECK_EQ(view.size(), 2u);
	ENGINE_CHECK(view.data() == data.data());
	ENGINE_CHECK(r.read_bytes(5).empty());
	ENGINE_CHECK(!r.ok());
}
