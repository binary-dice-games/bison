// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

#include "src/bison/bison_perfetto.hpp"

#include "src/bison/bison_serialization.hpp"

#include <cstring>

namespace bdg::bison::perfetto {

namespace {

// Protobuf wire types.
constexpr uint64_t kWireVarint = 0;
constexpr uint64_t kWireFixed64 = 1;
constexpr uint64_t kWireLengthDelimited = 2;

// TracePacket field numbers.
constexpr uint64_t kTracePacketTimestamp = 8;
constexpr uint64_t kTracePacketTrustedSequenceId = 10;
constexpr uint64_t kTracePacketTrackEvent = 11;
constexpr uint64_t kTracePacketTrackDescriptor = 60;

// TrackEvent field numbers.
constexpr uint64_t kTrackEventType = 9;
constexpr uint64_t kTrackEventTrackUuid = 11;
constexpr uint64_t kTrackEventName = 23;
constexpr uint64_t kTrackEventCounterValue = 30;
constexpr uint64_t kTrackEventDoubleCounterValue = 44;

// TrackDescriptor field numbers.
constexpr uint64_t kTrackDescriptorUuid = 1;
constexpr uint64_t kTrackDescriptorName = 2;
constexpr uint64_t kTrackDescriptorCounter = 8;

// CounterDescriptor field numbers.
constexpr uint64_t kCounterDescriptorUnit = 3;

uint64_t tag(uint64_t field_number, uint64_t wire_type) {
  return (field_number << 3) | wire_type;
}

/**
 * @brief Wrap @p payload as the content of a length-delimited field.
 *
 * `buffer_serializer::write(string_view)` already writes exactly
 * varint(size) + bytes, which is the wire content of a protobuf
 * length-delimited field once the caller has written the field's tag.
 */
void write_length_delimited(bison::buffer_serializer& out, uint64_t field_number, const bison::buffer& payload) {
  out.write_varint(tag(field_number, kWireLengthDelimited));
  out.write(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
}

void write_length_delimited(bison::buffer_serializer& out, uint64_t field_number, std::string_view payload) {
  out.write_varint(tag(field_number, kWireLengthDelimited));
  out.write(payload);
}

void write_varint_field(bison::buffer_serializer& out, uint64_t field_number, uint64_t value) {
  out.write_varint(tag(field_number, kWireVarint));
  out.write_varint(value);
}

/**
 * @brief Write a protobuf `double` field (wire type 1, fixed64,
 *        little-endian on the wire regardless of host byte order).
 *
 * `buffer_serializer::write<T>()` is unsuitable here: it always writes
 * big-endian (see bison_serialization.hpp), while protobuf's fixed64 wire
 * type is always little-endian.
 */
void write_fixed64_field(bison::buffer_serializer& out, uint64_t field_number, double value) {
  out.write_varint(tag(field_number, kWireFixed64));
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  uint8_t le_bytes[8];
  for (size_t i = 0; i < 8; ++i)
    le_bytes[i] = static_cast<uint8_t>(bits >> (8 * i));
  // `write(string_view)` prepends a length varint (it's the length-delimited
  // primitive); the raw `write(const char*, count)` overload writes exactly
  // the given bytes with no prefix, which is what a fixed64 field needs.
  out.write(reinterpret_cast<const char*>(le_bytes), sizeof(le_bytes));
}

/** @brief Frame @p packet_body as a complete `TracePacket` submessage. */
bdg::bison::buffer frame_trace_packet(bison::buffer_serializer&& packet_body) {
  const bison::buffer body = packet_body.release();
  bison::buffer_serializer framed(body.size() + 16);
  write_length_delimited(framed, /*Trace.packet=*/1, body);
  return framed.release();
}

} // namespace

bdg::bison::buffer encode_track_event_packet(uint64_t timestamp_ns, uint32_t trusted_packet_sequence_id,
                                              uint64_t track_uuid, event_type type, std::string_view name) {
  bison::buffer_serializer event;
  write_varint_field(event, kTrackEventType, static_cast<uint64_t>(type));
  write_varint_field(event, kTrackEventTrackUuid, track_uuid);
  if (type != event_type::slice_end && !name.empty()) {
    write_length_delimited(event, kTrackEventName, name);
  }

  bison::buffer_serializer packet;
  write_varint_field(packet, kTracePacketTimestamp, timestamp_ns);
  write_varint_field(packet, kTracePacketTrustedSequenceId, trusted_packet_sequence_id);
  write_length_delimited(packet, kTracePacketTrackEvent, event.release());

  return frame_trace_packet(std::move(packet));
}

bdg::bison::buffer encode_track_descriptor_packet(uint64_t timestamp_ns, uint32_t trusted_packet_sequence_id,
                                                    uint64_t track_uuid, std::string_view track_name) {
  bison::buffer_serializer descriptor;
  write_varint_field(descriptor, kTrackDescriptorUuid, track_uuid);
  write_length_delimited(descriptor, kTrackDescriptorName, track_name);

  bison::buffer_serializer packet;
  write_varint_field(packet, kTracePacketTimestamp, timestamp_ns);
  write_varint_field(packet, kTracePacketTrustedSequenceId, trusted_packet_sequence_id);
  write_length_delimited(packet, kTracePacketTrackDescriptor, descriptor.release());

  return frame_trace_packet(std::move(packet));
}

bdg::bison::buffer encode_counter_track_descriptor_packet(uint64_t timestamp_ns, uint32_t trusted_packet_sequence_id,
                                                            uint64_t track_uuid, std::string_view track_name,
                                                            counter_unit unit) {
  bison::buffer_serializer descriptor;
  write_varint_field(descriptor, kTrackDescriptorUuid, track_uuid);
  write_length_delimited(descriptor, kTrackDescriptorName, track_name);
  if (unit != counter_unit::unspecified) {
    bison::buffer_serializer counter_descriptor;
    write_varint_field(counter_descriptor, kCounterDescriptorUnit, static_cast<uint64_t>(unit));
    write_length_delimited(descriptor, kTrackDescriptorCounter, counter_descriptor.release());
  }

  bison::buffer_serializer packet;
  write_varint_field(packet, kTracePacketTimestamp, timestamp_ns);
  write_varint_field(packet, kTracePacketTrustedSequenceId, trusted_packet_sequence_id);
  write_length_delimited(packet, kTracePacketTrackDescriptor, descriptor.release());

  return frame_trace_packet(std::move(packet));
}

bdg::bison::buffer encode_counter_event_packet(uint64_t timestamp_ns, uint32_t trusted_packet_sequence_id,
                                                uint64_t track_uuid, int64_t value) {
  bison::buffer_serializer event;
  write_varint_field(event, kTrackEventType, static_cast<uint64_t>(event_type::counter));
  write_varint_field(event, kTrackEventTrackUuid, track_uuid);
  write_varint_field(event, kTrackEventCounterValue, static_cast<uint64_t>(value));

  bison::buffer_serializer packet;
  write_varint_field(packet, kTracePacketTimestamp, timestamp_ns);
  write_varint_field(packet, kTracePacketTrustedSequenceId, trusted_packet_sequence_id);
  write_length_delimited(packet, kTracePacketTrackEvent, event.release());

  return frame_trace_packet(std::move(packet));
}

bdg::bison::buffer encode_double_counter_event_packet(uint64_t timestamp_ns, uint32_t trusted_packet_sequence_id,
                                                        uint64_t track_uuid, double value) {
  bison::buffer_serializer event;
  write_varint_field(event, kTrackEventType, static_cast<uint64_t>(event_type::counter));
  write_varint_field(event, kTrackEventTrackUuid, track_uuid);
  write_fixed64_field(event, kTrackEventDoubleCounterValue, value);

  bison::buffer_serializer packet;
  write_varint_field(packet, kTracePacketTimestamp, timestamp_ns);
  write_varint_field(packet, kTracePacketTrustedSequenceId, trusted_packet_sequence_id);
  write_length_delimited(packet, kTracePacketTrackEvent, event.release());

  return frame_trace_packet(std::move(packet));
}

} // namespace bdg::bison::perfetto
