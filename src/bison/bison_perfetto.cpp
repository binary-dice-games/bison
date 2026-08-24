// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

#include "src/bison/bison_perfetto.hpp"

#include "src/bison/bison_serialization.hpp"

namespace bdg::bison::perfetto {

namespace {

// Protobuf wire types.
constexpr uint64_t kWireVarint = 0;
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

// TrackDescriptor field numbers.
constexpr uint64_t kTrackDescriptorUuid = 1;
constexpr uint64_t kTrackDescriptorName = 2;

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

} // namespace bdg::bison::perfetto
