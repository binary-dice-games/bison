// MIT License © 2025 Binary Dice Games
// Byte-exact and round-trip tests for the Perfetto track-event encoder.

#include "src/bison/bison_perfetto.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace bdg::bison;
using namespace bdg::bison::perfetto;

// ─────────────────────────────────────────────────────────────────────────────
// Minimal protobuf wire decoder, independent of the encoder under test.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct decoded_field {
  uint64_t field_number = 0;
  uint64_t wire_type = 0;
  uint64_t varint_value = 0;
  uint64_t fixed64_value = 0;
  std::vector<uint8_t> bytes;
};

double fixed64_as_double(uint64_t bits) {
  double value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

uint64_t read_varint(const std::vector<uint8_t>& buf, size_t& pos) {
  uint64_t value = 0;
  int shift = 0;
  while (pos < buf.size()) {
    const uint8_t byte = buf[pos++];
    value |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0)
      break;
    shift += 7;
  }
  return value;
}

std::vector<decoded_field> decode_fields(const std::vector<uint8_t>& buf) {
  std::vector<decoded_field> fields;
  size_t pos = 0;
  while (pos < buf.size()) {
    const uint64_t tag = read_varint(buf, pos);
    decoded_field f;
    f.field_number = tag >> 3;
    f.wire_type = tag & 0x7;
    if (f.wire_type == 0) {
      f.varint_value = read_varint(buf, pos);
    } else if (f.wire_type == 1) {
      // Fixed64, little-endian on the wire.
      for (int i = 0; i < 8; ++i)
        f.fixed64_value |= static_cast<uint64_t>(buf[pos++]) << (8 * i);
    } else if (f.wire_type == 2) {
      const uint64_t len = read_varint(buf, pos);
      f.bytes.assign(buf.begin() + static_cast<long>(pos), buf.begin() + static_cast<long>(pos + len));
      pos += len;
    } else {
      ADD_FAILURE() << "unsupported wire type " << f.wire_type;
      break;
    }
    fields.push_back(std::move(f));
  }
  return fields;
}

/** @brief Unwrap one `Trace.packet` (field 1, length-delimited) envelope. */
std::vector<uint8_t> unwrap_trace_packet(const buffer& framed) {
  const auto fields = decode_fields(framed);
  EXPECT_EQ(fields.size(), 1u);
  EXPECT_EQ(fields[0].field_number, 1u);
  EXPECT_EQ(fields[0].wire_type, 2u);
  return fields[0].bytes;
}

const decoded_field* find_field(const std::vector<decoded_field>& fields, uint64_t number) {
  for (const auto& f : fields) {
    if (f.field_number == number)
      return &f;
  }
  return nullptr;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// TrackEvent packets
// ═════════════════════════════════════════════════════════════════════════════

TEST(BisonPerfetto, TrackEventSliceBeginRoundTrips) {
  const auto framed = encode_track_event_packet(1234, 7, 0xABCDu, event_type::slice_begin, "my_slice");
  const auto packet_body = unwrap_trace_packet(framed);
  const auto packet_fields = decode_fields(packet_body);

  const auto* ts = find_field(packet_fields, /*TracePacket.timestamp=*/8);
  ASSERT_NE(ts, nullptr);
  EXPECT_EQ(ts->wire_type, 0u);
  EXPECT_EQ(ts->varint_value, 1234u);

  const auto* seq = find_field(packet_fields, /*TracePacket.trusted_packet_sequence_id=*/10);
  ASSERT_NE(seq, nullptr);
  EXPECT_EQ(seq->varint_value, 7u);

  const auto* track_event = find_field(packet_fields, /*TracePacket.track_event=*/11);
  ASSERT_NE(track_event, nullptr);
  EXPECT_EQ(track_event->wire_type, 2u);

  const auto event_fields = decode_fields(track_event->bytes);

  const auto* type = find_field(event_fields, /*TrackEvent.type=*/9);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(type->varint_value, static_cast<uint64_t>(event_type::slice_begin));

  const auto* uuid = find_field(event_fields, /*TrackEvent.track_uuid=*/11);
  ASSERT_NE(uuid, nullptr);
  EXPECT_EQ(uuid->varint_value, 0xABCDu);

  const auto* name = find_field(event_fields, /*TrackEvent.name=*/23);
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(std::string(name->bytes.begin(), name->bytes.end()), "my_slice");
}

TEST(BisonPerfetto, TrackEventSliceEndOmitsName) {
  const auto framed = encode_track_event_packet(1, 1, 1, event_type::slice_end, "ignored");
  const auto packet_body = unwrap_trace_packet(framed);
  const auto packet_fields = decode_fields(packet_body);

  const auto* track_event = find_field(packet_fields, 11);
  ASSERT_NE(track_event, nullptr);
  const auto event_fields = decode_fields(track_event->bytes);

  EXPECT_EQ(find_field(event_fields, /*TrackEvent.name=*/23), nullptr);

  const auto* type = find_field(event_fields, 9);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(type->varint_value, static_cast<uint64_t>(event_type::slice_end));
}

TEST(BisonPerfetto, TrackEventInstantCarriesName) {
  const auto framed = encode_track_event_packet(1, 1, 1, event_type::instant, "tick");
  const auto packet_body = unwrap_trace_packet(framed);
  const auto packet_fields = decode_fields(packet_body);
  const auto event_fields = decode_fields(find_field(packet_fields, 11)->bytes);

  const auto* name = find_field(event_fields, 23);
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(std::string(name->bytes.begin(), name->bytes.end()), "tick");
}

TEST(BisonPerfetto, TrackEventByteExactForFixedInput) {
  // timestamp=1 (varint), sequence_id=1 (varint), then a nested TrackEvent
  // submessage: type=SLICE_BEGIN(1), track_uuid=1, name="a".
  const auto framed = encode_track_event_packet(1, 1, 1, event_type::slice_begin, "a");

  // TrackEvent body: tag(9,varint)=0x48,1 ; tag(11,varint)=0x58,1 ;
  // tag(23,len)=0xBA,0x01, len=1, 'a'.
  const std::vector<uint8_t> expected_event_body = {
      0x48, 0x01,             // type = 1
      0x58, 0x01,             // track_uuid = 1
      0xBA, 0x01, 0x01, 'a',  // name = "a"
  };
  // TracePacket body: tag(8,varint)=0x40,1 ; tag(10,varint)=0x50,1 ;
  // tag(11,len)=0x5A, len, event body.
  std::vector<uint8_t> expected_packet_body = {0x40, 0x01, 0x50, 0x01, 0x5A,
                                                static_cast<uint8_t>(expected_event_body.size())};
  expected_packet_body.insert(expected_packet_body.end(), expected_event_body.begin(), expected_event_body.end());

  // Trace.packet framing: tag(1,len)=0x0A, len, packet body.
  std::vector<uint8_t> expected = {0x0A, static_cast<uint8_t>(expected_packet_body.size())};
  expected.insert(expected.end(), expected_packet_body.begin(), expected_packet_body.end());

  EXPECT_EQ(framed, expected);
}

// ═════════════════════════════════════════════════════════════════════════════
// TrackDescriptor packets
// ═════════════════════════════════════════════════════════════════════════════

TEST(BisonPerfetto, TrackDescriptorRoundTrips) {
  const auto framed = encode_track_descriptor_packet(42, 3, 0x1122334455u, "worker-thread");
  const auto packet_body = unwrap_trace_packet(framed);
  const auto packet_fields = decode_fields(packet_body);

  EXPECT_EQ(find_field(packet_fields, 8)->varint_value, 42u);
  EXPECT_EQ(find_field(packet_fields, 10)->varint_value, 3u);

  const auto* descriptor = find_field(packet_fields, /*TracePacket.track_descriptor=*/60);
  ASSERT_NE(descriptor, nullptr);
  const auto descriptor_fields = decode_fields(descriptor->bytes);

  const auto* uuid = find_field(descriptor_fields, /*TrackDescriptor.uuid=*/1);
  ASSERT_NE(uuid, nullptr);
  EXPECT_EQ(uuid->varint_value, 0x1122334455u);

  const auto* name = find_field(descriptor_fields, /*TrackDescriptor.name=*/2);
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(std::string(name->bytes.begin(), name->bytes.end()), "worker-thread");
}

// ═════════════════════════════════════════════════════════════════════════════
// Counter track/event packets
// ═════════════════════════════════════════════════════════════════════════════

TEST(BisonPerfetto, CounterTrackDescriptorOmitsUnitByDefault) {
  const auto framed = encode_counter_track_descriptor_packet(0, 1, 9, "my-counter");
  const auto packet_body = unwrap_trace_packet(framed);
  const auto packet_fields = decode_fields(packet_body);

  const auto* descriptor = find_field(packet_fields, /*TracePacket.track_descriptor=*/60);
  ASSERT_NE(descriptor, nullptr);
  const auto descriptor_fields = decode_fields(descriptor->bytes);

  const auto* uuid = find_field(descriptor_fields, /*TrackDescriptor.uuid=*/1);
  ASSERT_NE(uuid, nullptr);
  EXPECT_EQ(uuid->varint_value, 9u);

  const auto* name = find_field(descriptor_fields, /*TrackDescriptor.name=*/2);
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(std::string(name->bytes.begin(), name->bytes.end()), "my-counter");

  // Unit unspecified -> CounterDescriptor sub-message omitted entirely.
  EXPECT_EQ(find_field(descriptor_fields, /*TrackDescriptor.counter=*/8), nullptr);
}

TEST(BisonPerfetto, CounterTrackDescriptorCarriesUnit) {
  const auto framed = encode_counter_track_descriptor_packet(0, 1, 9, "bytes-sent", counter_unit::size_bytes);
  const auto packet_body = unwrap_trace_packet(framed);
  const auto packet_fields = decode_fields(packet_body);
  const auto descriptor_fields = decode_fields(find_field(packet_fields, 60)->bytes);

  const auto* counter = find_field(descriptor_fields, /*TrackDescriptor.counter=*/8);
  ASSERT_NE(counter, nullptr);
  const auto counter_fields = decode_fields(counter->bytes);

  const auto* unit = find_field(counter_fields, /*CounterDescriptor.unit=*/3);
  ASSERT_NE(unit, nullptr);
  EXPECT_EQ(unit->varint_value, static_cast<uint64_t>(counter_unit::size_bytes));
}

TEST(BisonPerfetto, CounterEventCarriesIntValue) {
  const auto framed = encode_counter_event_packet(100, 1, 9, -42);
  const auto packet_body = unwrap_trace_packet(framed);
  const auto packet_fields = decode_fields(packet_body);
  const auto event_fields = decode_fields(find_field(packet_fields, 11)->bytes);

  const auto* type = find_field(event_fields, /*TrackEvent.type=*/9);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(type->varint_value, static_cast<uint64_t>(event_type::counter));

  const auto* uuid = find_field(event_fields, /*TrackEvent.track_uuid=*/11);
  ASSERT_NE(uuid, nullptr);
  EXPECT_EQ(uuid->varint_value, 9u);

  const auto* value = find_field(event_fields, /*TrackEvent.counter_value=*/30);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(value->wire_type, 0u);
  EXPECT_EQ(static_cast<int64_t>(value->varint_value), -42);
}

TEST(BisonPerfetto, DoubleCounterEventCarriesFixed64Value) {
  const auto framed = encode_double_counter_event_packet(100, 1, 9, 3.5);
  const auto packet_body = unwrap_trace_packet(framed);
  const auto packet_fields = decode_fields(packet_body);
  const auto event_fields = decode_fields(find_field(packet_fields, 11)->bytes);

  const auto* type = find_field(event_fields, 9);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(type->varint_value, static_cast<uint64_t>(event_type::counter));

  const auto* value = find_field(event_fields, /*TrackEvent.double_counter_value=*/44);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(value->wire_type, 1u);
  EXPECT_EQ(fixed64_as_double(value->fixed64_value), 3.5);
}

// ═════════════════════════════════════════════════════════════════════════════
// Multi-packet batches concatenate cleanly.
// ═════════════════════════════════════════════════════════════════════════════

TEST(BisonPerfetto, ConcatenatedPacketsDecodeIndependently) {
  buffer batch;
  const auto descriptor = encode_track_descriptor_packet(0, 1, 5, "t");
  const auto begin = encode_track_event_packet(1, 1, 5, event_type::slice_begin, "s");
  const auto end = encode_track_event_packet(2, 1, 5, event_type::slice_end, "");
  batch.insert(batch.end(), descriptor.begin(), descriptor.end());
  batch.insert(batch.end(), begin.begin(), begin.end());
  batch.insert(batch.end(), end.begin(), end.end());

  // Walk the concatenated buffer as a sequence of independently length-framed
  // Trace.packet fields -- exactly how a real trace file is structured.
  size_t pos = 0;
  int packet_count = 0;
  while (pos < batch.size()) {
    const uint64_t tag = read_varint(batch, pos);
    ASSERT_EQ(tag >> 3, 1u);
    ASSERT_EQ(tag & 0x7, 2u);
    const uint64_t len = read_varint(batch, pos);
    pos += len;
    ++packet_count;
  }
  EXPECT_EQ(packet_count, 3);
  EXPECT_EQ(pos, batch.size());
}
