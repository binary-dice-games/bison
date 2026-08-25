// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

/**
 * @file bison_perfetto.hpp
 * @brief Minimal hand-rolled encoder for Perfetto's track-event protobuf
 *        wire format (https://perfetto.dev/docs/instrumentation/track-events).
 *
 * This is *not* a general protobuf library and does not depend on the
 * Perfetto SDK or `libprotobuf` -- it encodes exactly the handful of
 * `TracePacket` shapes bison's profiler needs, using the varint/
 * length-delimited primitives already provided by `buffer_serializer`. A
 * valid Perfetto trace file is simply the raw concatenation of these
 * encoded packets (each one is independently length-framed as if it were
 * field 1 of the top-level `Trace` message, so no outer wrapper is ever
 * built). See FORMAT.md for the field-number reference this file encodes
 * against.
 */

#pragma once

#include <cstdint>
#include <string_view>

#include "src/bison/bison_common.hpp"

namespace bdg::bison::perfetto {

/** @brief Mirrors a subset of Perfetto's `TrackEvent.Type` enum. */
enum class event_type : uint32_t {
  unspecified = 0,
  slice_begin = 1,
  slice_end = 2,
  instant = 3,
  counter = 4,
};

/**
 * @brief Mirrors a subset of Perfetto's `CounterDescriptor.Unit` enum, used
 *        to label a counter track's `TrackDescriptor` for display purposes.
 */
enum class counter_unit : uint32_t {
  unspecified = 0,
  time_ns = 1,
  count = 2,
  size_bytes = 3,
};

/**
 * @brief Encode one length-framed `TracePacket` carrying a `TrackEvent`.
 *
 * @param timestamp_ns                 Absolute timestamp in nanoseconds.
 * @param trusted_packet_sequence_id   Identifies the producer sequence this
 *                                     packet belongs to (must be unique per
 *                                     writer and stable across its packets).
 * @param track_uuid                   Track this event is emitted on.
 * @param type                         Slice begin/end or instant.
 * @param name                         Event name; ignored for `slice_end`.
 * @return A complete, self-framed `TracePacket` ready to append to a trace
 *         file or an RMI trace block.
 */
bdg::bison::buffer encode_track_event_packet(uint64_t timestamp_ns, uint32_t trusted_packet_sequence_id,
                                              uint64_t track_uuid, event_type type, std::string_view name);

/**
 * @brief Encode one length-framed `TracePacket` carrying a `TrackDescriptor`.
 *
 * Must be emitted at least once for a given `track_uuid` before any
 * `TrackEvent` referencing it is guaranteed to render correctly in the
 * Perfetto UI.
 *
 * @param timestamp_ns                 Absolute timestamp in nanoseconds.
 * @param trusted_packet_sequence_id   Producer sequence id (see above).
 * @param track_uuid                   Track being described.
 * @param track_name                   Human-readable track name.
 * @return A complete, self-framed `TracePacket`.
 */
bdg::bison::buffer encode_track_descriptor_packet(uint64_t timestamp_ns, uint32_t trusted_packet_sequence_id,
                                                    uint64_t track_uuid, std::string_view track_name);

/**
 * @brief Encode one length-framed `TracePacket` carrying a `TrackDescriptor`
 *        for a counter track (its `CounterDescriptor` sub-message set).
 *
 * A counter track only ever carries `event_type::counter` `TrackEvent`s
 * (see `encode_counter_event_packet`/`encode_double_counter_event_packet`),
 * never slices or instants. As with `encode_track_descriptor_packet`, this
 * must be emitted at least once for a given `track_uuid` before any
 * `TrackEvent` referencing it is guaranteed to render correctly.
 *
 * @param timestamp_ns                 Absolute timestamp in nanoseconds.
 * @param trusted_packet_sequence_id   Producer sequence id (see above).
 * @param track_uuid                   Track being described.
 * @param track_name                   Human-readable track name.
 * @param unit                         Unit the counter's values are in;
 *                                     `counter_unit::unspecified` omits the
 *                                     field entirely.
 * @return A complete, self-framed `TracePacket`.
 */
bdg::bison::buffer encode_counter_track_descriptor_packet(uint64_t timestamp_ns, uint32_t trusted_packet_sequence_id,
                                                            uint64_t track_uuid, std::string_view track_name,
                                                            counter_unit unit = counter_unit::unspecified);

/**
 * @brief Encode one length-framed `TracePacket` carrying an integer-valued
 *        `TYPE_COUNTER` `TrackEvent`.
 *
 * @param timestamp_ns                 Absolute timestamp in nanoseconds.
 * @param trusted_packet_sequence_id   Producer sequence id (see above).
 * @param track_uuid                   Counter track this sample belongs to;
 *                                     should have a matching
 *                                     `encode_counter_track_descriptor_packet`
 *                                     emitted earlier in the trace.
 * @param value                        New value of the counter.
 * @return A complete, self-framed `TracePacket`.
 */
bdg::bison::buffer encode_counter_event_packet(uint64_t timestamp_ns, uint32_t trusted_packet_sequence_id,
                                                uint64_t track_uuid, int64_t value);

/**
 * @brief Encode one length-framed `TracePacket` carrying a double-valued
 *        `TYPE_COUNTER` `TrackEvent`.
 *
 * @param timestamp_ns                 Absolute timestamp in nanoseconds.
 * @param trusted_packet_sequence_id   Producer sequence id (see above).
 * @param track_uuid                   Counter track this sample belongs to;
 *                                     should have a matching
 *                                     `encode_counter_track_descriptor_packet`
 *                                     emitted earlier in the trace.
 * @param value                        New value of the counter.
 * @return A complete, self-framed `TracePacket`.
 */
bdg::bison::buffer encode_double_counter_event_packet(uint64_t timestamp_ns, uint32_t trusted_packet_sequence_id,
                                                        uint64_t track_uuid, double value);

} // namespace bdg::bison::perfetto
