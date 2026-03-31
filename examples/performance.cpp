// MIT License (c) 2025 Binary Dice Games
// examples/performance.cpp
//
// Micro-benchmark comparing equivalent operations implemented with:
// 1. A plain C++ class
// 2. bdg::bison::dynamic
// 3. nlohmann::json

/**
 * @file performance.cpp
 * @brief Micro-benchmark comparing object-model overhead across three
 * representations.
 *
 * The benchmark measures equivalent operations implemented with:
 * 1. A plain C++ struct (`NativeRecord`)
 * 2. `bdg::bison::dynamic`
 * 3. `nlohmann::json`
 *
 * Benchmarked operations:
 * - Create / destroy
 * - Field set / get
 * - Method-style call
 * - Serialize        (stream-based and buffer-based variants for dynamic)
 * - Deserialize      (stream-based and buffer-based variants for dynamic)
 *
 * The harness supports warmup + repeated measured samples and reports
 * min/median timings. Results can be printed as table, CSV, or Markdown.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/core/bison.hpp"

using namespace bdg::bison;
using json = nlohmann::json;

namespace {

/** @brief Pre-hashed keys used by the `bison::dynamic` benchmark path. */
constexpr hash_t KEY_ID = "id"_key;
constexpr hash_t KEY_AGE = "age"_key;
constexpr hash_t KEY_SCORE = "score"_key;
constexpr hash_t KEY_RATIO = "ratio"_key;
constexpr hash_t KEY_ACTIVE = "active"_key;
constexpr hash_t KEY_LEVEL = "level"_key;
constexpr hash_t KEY_NAME = "name"_key;
constexpr hash_t KEY_A = "a"_key;
constexpr hash_t KEY_B = "b"_key;
constexpr hash_t KEY_STEP = "step"_key;
constexpr hash_t KEY_DELTA = "delta"_key;
constexpr hash_t KEY_VALUE = "value"_key;

/**
 * @brief Number of prebuilt objects/payloads used by serialization benchmarks.
 *
 * Serialization rows intentionally reuse prebuilt inputs so measured time is
 * dominated by serialization/deserialization work rather than object creation.
 */
constexpr std::size_t SERIALIZATION_POOL_SIZE = 1024u;

/** @brief Volatile sink to prevent benchmark code paths from being optimized
 * away. */
volatile std::uint64_t g_sink = 0;

/** @brief Consume an integer value into the optimization guard sink. */
void consume_u64(std::uint64_t value) {
  g_sink += value;
}

void consume_i32(std::int32_t value) {
  consume_u64(static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)));
}

void consume_f32(float value) {
  consume_u64(static_cast<std::uint64_t>(value * 1000.0f));
}

void consume_bool(bool value) {
  consume_u64(value ? 1u : 0u);
}

void consume_string(std::string_view value) {
  consume_u64(static_cast<std::uint64_t>(value.size()));
  if (!value.empty()) {
    consume_u64(
        static_cast<std::uint64_t>(static_cast<unsigned char>(value.front())));
  }
}

const char* benchmark_name(std::size_t iteration) {
  return (iteration & 1u) == 0u ? "alpha" : "omega";
}

/** @brief Parameter object used by native method-style benchmark calls. */
struct NativeParams {
  std::int32_t a;
  std::int32_t b;
  std::int32_t step;
  float delta;
};

/**
 * @brief Plain C++ record used as the low-overhead baseline representation.
 */
struct NativeRecord {
  std::int32_t id;
  std::int32_t age;
  float score;
  float ratio;
  bool active;
  std::int32_t level;
  std::string name;

  /**
   * @brief Native method-like operation used for method-call benchmarking.
   *
   * Mirrors the same update + compute pattern used in dynamic/json paths.
   */
  std::int32_t compute(const NativeParams& params) {
    score += params.delta;
    level += params.step;
    active = !active;
    return id + age + level + params.a + params.b +
        static_cast<std::int32_t>(score + ratio) +
        static_cast<std::int32_t>(name.size()) + (active ? 1 : 0);
  }
};

NativeRecord make_native_record(std::size_t iteration) {
  return NativeRecord{
      static_cast<std::int32_t>(iteration),
      20 + static_cast<std::int32_t>(iteration % 50u),
      100.0f + static_cast<float>(iteration % 100u) * 0.25f,
      0.5f + static_cast<float>(iteration % 10u) * 0.05f,
      (iteration & 1u) == 0u,
      static_cast<std::int32_t>(iteration % 8u),
      benchmark_name(iteration)};
}

dynamic make_dynamic_record(std::size_t iteration) {
  dynamic obj{"BenchmarkRecord"_key};
  obj[KEY_ID] = static_cast<std::int32_t>(iteration);
  obj[KEY_AGE] = 20 + static_cast<std::int32_t>(iteration % 50u);
  obj[KEY_SCORE] = 100.0f + static_cast<float>(iteration % 100u) * 0.25f;
  obj[KEY_RATIO] = 0.5f + static_cast<float>(iteration % 10u) * 0.05f;
  obj[KEY_ACTIVE] = (iteration & 1u) == 0u;
  obj[KEY_LEVEL] = static_cast<std::int32_t>(iteration % 8u);
  obj[KEY_NAME] = std::string{benchmark_name(iteration)};
  return obj;
}

json make_json_record(std::size_t iteration) {
  return json{
      {"id", static_cast<std::int32_t>(iteration)},
      {"age", 20 + static_cast<std::int32_t>(iteration % 50u)},
      {"score", 100.0 + static_cast<double>(iteration % 100u) * 0.25},
      {"ratio", 0.5 + static_cast<double>(iteration % 10u) * 0.05},
      {"active", (iteration & 1u) == 0u},
      {"level", static_cast<std::int32_t>(iteration % 8u)},
      {"name", benchmark_name(iteration)}};
}

template <typename T>
void append_bytes(std::string& buffer, const T& value) {
  buffer.append(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
T read_bytes(const std::string& buffer, std::size_t& offset) {
  T value{};
  std::memcpy(&value, buffer.data() + offset, sizeof(T));
  offset += sizeof(T);
  return value;
}

/**
 * @brief Serialize `NativeRecord` to a compact binary blob.
 *
 * This is intentionally simple and deterministic. It provides a native binary
 * baseline for comparison against Bison binary serialization and JSON text
 * dump.
 */
std::string serialize_native_record(const NativeRecord& obj) {
  std::string buffer;
  buffer.reserve(
      sizeof(obj.id) + sizeof(obj.age) + sizeof(obj.score) + sizeof(obj.ratio) +
      sizeof(std::uint8_t) + sizeof(obj.level) + sizeof(std::uint32_t) +
      obj.name.size());
  append_bytes(buffer, obj.id);
  append_bytes(buffer, obj.age);
  append_bytes(buffer, obj.score);
  append_bytes(buffer, obj.ratio);
  append_bytes(buffer, static_cast<std::uint8_t>(obj.active ? 1u : 0u));
  append_bytes(buffer, obj.level);
  append_bytes(buffer, static_cast<std::uint32_t>(obj.name.size()));
  buffer.append(obj.name.data(), obj.name.size());
  return buffer;
}

/** @brief Deserialize `NativeRecord` from its compact binary blob
 * representation. */
NativeRecord deserialize_native_record(const std::string& buffer) {
  NativeRecord obj{};
  std::size_t offset = 0;
  obj.id = read_bytes<std::int32_t>(buffer, offset);
  obj.age = read_bytes<std::int32_t>(buffer, offset);
  obj.score = read_bytes<float>(buffer, offset);
  obj.ratio = read_bytes<float>(buffer, offset);
  obj.active = read_bytes<std::uint8_t>(buffer, offset) != 0u;
  obj.level = read_bytes<std::int32_t>(buffer, offset);
  const std::uint32_t name_size = read_bytes<std::uint32_t>(buffer, offset);
  obj.name.assign(buffer.data() + offset, name_size);
  return obj;
}

/** @brief Serialize a `bison::dynamic` object to Bison binary format (stream).
 */
std::string serialize_dynamic_record(const dynamic& obj) {
  std::ostringstream out(std::ios::binary);
  stream_serializer ser{out};
  obj.serialize(ser);
  return out.str();
}

/** @brief Deserialize a `bison::dynamic` object from Bison binary format
 * (stream). */
dynamic deserialize_dynamic_record(const std::string& buffer) {
  std::istringstream in(buffer, std::ios::binary);
  stream_deserializer des{in};
  return dynamic::deserialize(des);
}

/**
 * @brief Serialize a `bison::dynamic` object to Bison binary format using
 *        the buffer_serializer (no stream overhead).
 */
buffer serialize_dynamic_record_buf(const dynamic& obj) {
  buffer_serializer out;
  obj.serialize(out);
  return out.release();
}

/**
 * @brief Deserialize a `bison::dynamic` object from Bison binary format
 *        using the buffer_deserializer (no stream overhead).
 */
dynamic deserialize_dynamic_record_buf(const buffer& buf) {
  buffer_deserializer in(buf);
  return dynamic::deserialize(in);
}

/** @brief Serialize a JSON object using text encoding (`dump`). */
std::string serialize_json_record(const json& obj) {
  return obj.dump();
}

/** @brief Deserialize a JSON object from text encoding (`parse`). */
json deserialize_json_record(const std::string& buffer) {
  return json::parse(buffer);
}

/**
 * @brief Prebuilt benchmark data used by serialization-related measurements.
 */
struct benchmark_pool {
  std::vector<NativeRecord> native_records;
  std::vector<dynamic> dynamic_records;
  std::vector<json> json_records;
  std::vector<std::string> native_payloads;
  std::vector<std::string> dynamic_payloads;
  std::vector<buffer> dynamic_buf_payloads; // buffer_serializer payloads
  std::vector<std::string> json_payloads;
};

/**
 * @brief Build reusable objects and payloads for serialization benchmarks.
 *
 * @param pool_size Number of distinct fixtures to precompute.
 */
benchmark_pool build_serialization_pool(std::size_t pool_size) {
  benchmark_pool pool;
  pool.native_records.reserve(pool_size);
  pool.dynamic_records.reserve(pool_size);
  pool.json_records.reserve(pool_size);
  pool.native_payloads.reserve(pool_size);
  pool.dynamic_payloads.reserve(pool_size);
  pool.dynamic_buf_payloads.reserve(pool_size);
  pool.json_payloads.reserve(pool_size);

  // Build object fixtures first.
  for (std::size_t iteration = 0; iteration < pool_size; ++iteration) {
    pool.native_records.push_back(make_native_record(iteration));
    pool.dynamic_records.push_back(make_dynamic_record(iteration));
    pool.json_records.push_back(make_json_record(iteration));
  }

  // Then build payload fixtures derived from those objects.
  for (std::size_t iteration = 0; iteration < pool_size; ++iteration) {
    pool.native_payloads.push_back(
        serialize_native_record(pool.native_records[iteration]));
    pool.dynamic_payloads.push_back(
        serialize_dynamic_record(pool.dynamic_records[iteration]));
    pool.dynamic_buf_payloads.push_back(
        serialize_dynamic_record_buf(pool.dynamic_records[iteration]));
    pool.json_payloads.push_back(
        serialize_json_record(pool.json_records[iteration]));
  }

  return pool;
}

NativeParams make_native_params(std::size_t iteration) {
  return NativeParams{
      static_cast<std::int32_t>(iteration % 31u),
      static_cast<std::int32_t>((iteration * 3u) % 29u),
      1 + static_cast<std::int32_t>(iteration % 3u),
      0.125f + static_cast<float>(iteration % 5u) * 0.025f};
}

dynamic make_dynamic_params(std::size_t iteration) {
  dynamic params;
  params[KEY_A] = static_cast<std::int32_t>(iteration % 31u);
  params[KEY_B] = static_cast<std::int32_t>((iteration * 3u) % 29u);
  params[KEY_STEP] = 1 + static_cast<std::int32_t>(iteration % 3u);
  params[KEY_DELTA] = 0.125f + static_cast<float>(iteration % 5u) * 0.025f;
  return params;
}

json make_json_params(std::size_t iteration) {
  return json{
      {"a", static_cast<std::int32_t>(iteration % 31u)},
      {"b", static_cast<std::int32_t>((iteration * 3u) % 29u)},
      {"step", 1 + static_cast<std::int32_t>(iteration % 3u)},
      {"delta", 0.125 + static_cast<double>(iteration % 5u) * 0.025}};
}

/**
 * @brief JSON equivalent of the method-style compute operation.
 *
 * Updates mutable fields on @p obj and returns a computed integer result.
 */
std::int32_t json_compute(json& obj, const json& params) {
  double score = obj["score"].get<double>() + params["delta"].get<double>();
  std::int32_t level =
      obj["level"].get<std::int32_t>() + params["step"].get<std::int32_t>();
  bool active = !obj["active"].get<bool>();
  obj["score"] = score;
  obj["level"] = level;
  obj["active"] = active;
  return obj["id"].get<std::int32_t>() + obj["age"].get<std::int32_t>() +
      level + params["a"].get<std::int32_t>() +
      params["b"].get<std::int32_t>() +
      static_cast<std::int32_t>(score + obj["ratio"].get<double>()) +
      static_cast<std::int32_t>(
             obj["name"].get_ref<const std::string&>().size()) +
      (active ? 1 : 0);
}

/**
 * @brief Create a `dynamic` object with an attached `compute` method.
 */
dynamic make_dynamic_method_record(std::size_t iteration) {
  dynamic obj = make_dynamic_record(iteration);
  obj.addMethod(
      "compute"_key, [](dynamic& self, const dynamic& params) -> dynamic {
        const float score =
            self[KEY_SCORE].as<float>() + params[KEY_DELTA].as<float>();
        const std::int32_t level = self[KEY_LEVEL].as<std::int32_t>() +
            params[KEY_STEP].as<std::int32_t>();
        const bool active = !self[KEY_ACTIVE].as<bool>();
        self[KEY_SCORE] = score;
        self[KEY_LEVEL] = level;
        self[KEY_ACTIVE] = active;

        dynamic result;
        result[KEY_VALUE] = self[KEY_ID].as<std::int32_t>() +
            self[KEY_AGE].as<std::int32_t>() + level +
            params[KEY_A].as<std::int32_t>() +
            params[KEY_B].as<std::int32_t>() +
            static_cast<std::int32_t>(score + self[KEY_RATIO].as<float>()) +
            static_cast<std::int32_t>(self[KEY_NAME].as<std::string>().size()) +
            (active ? 1 : 0);
        return result;
      });
  return obj;
}

template <typename Fn>
double measure_ms(std::size_t iterations, Fn&& fn) {
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    fn(iteration);
  }
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count();
}

/** @brief Summary statistics for repeated benchmark samples. */
struct sample_stats {
  double min_ms;
  double median_ms;
};

/** @brief Supported output modes for benchmark result rendering. */
enum class output_format {
  table,
  csv,
  markdown,
};

/**
 * @brief Runtime benchmark options parsed from CLI arguments.
 */
struct benchmark_config {
  std::size_t iterations;
  std::size_t warmup_samples;
  std::size_t samples;
  output_format format;
};

/** @brief One operation row containing statistics for all three
 * representations. */
struct benchmark_row {
  std::string operation;
  sample_stats native;
  sample_stats dynamic;
  sample_stats json;
};

double ratio(double baseline, double sample) {
  return baseline > 0.0 ? sample / baseline : 0.0;
}

std::size_t default_iterations() {
#ifdef NDEBUG
  return 200000u;
#else
  return 20000u;
#endif
}

/**
 * @brief Parse a positive integer CLI argument.
 *
 * Exits the process with an error message if parsing fails.
 */
std::size_t parse_size_arg(const char* label, const std::string& value) {
  try {
    return static_cast<std::size_t>(std::stoull(value));
  } catch (const std::exception&) {
    std::cerr << "Invalid " << label << ": " << value << "\n";
    std::exit(1);
  }
}

/** @brief Parse output format string from CLI. */
output_format parse_format(const std::string& value) {
  if (value == "table") {
    return output_format::table;
  }
  if (value == "csv") {
    return output_format::csv;
  }
  if (value == "markdown") {
    return output_format::markdown;
  }
  std::cerr << "Invalid format: " << value << "\n";
  std::exit(1);
}

/** @brief Print benchmark CLI usage help text. */
void print_usage(const char* program) {
  std::cout << "Usage: " << program
            << " [iterations] [--iterations=N] [--samples=N]"
            << " [--warmup=N] [--format=table|csv|markdown]\n";
}

/**
 * @brief Parse benchmark configuration from command-line arguments.
 *
 * Supports either positional `iterations` or `--iterations=...`, along with
 * optional warmup/sample/output-format options.
 */
benchmark_config parse_config(int argc, char** argv) {
  benchmark_config config{default_iterations(), 1u, 5u, output_format::table};
  bool positional_iterations_set = false;

  for (int arg_index = 1; arg_index < argc; ++arg_index) {
    const std::string arg = argv[arg_index];

    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    }
    if (arg.rfind("--iterations=", 0) == 0) {
      config.iterations = parse_size_arg("iteration count", arg.substr(13));
      positional_iterations_set = true;
      continue;
    }
    if (arg.rfind("--samples=", 0) == 0) {
      config.samples = parse_size_arg("sample count", arg.substr(10));
      continue;
    }
    if (arg.rfind("--warmup=", 0) == 0) {
      config.warmup_samples = parse_size_arg("warmup count", arg.substr(9));
      continue;
    }
    if (arg.rfind("--format=", 0) == 0) {
      config.format = parse_format(arg.substr(9));
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "Unknown option: " << arg << "\n";
      print_usage(argv[0]);
      std::exit(1);
    }
    if (positional_iterations_set) {
      std::cerr << "Unexpected positional argument: " << arg << "\n";
      print_usage(argv[0]);
      std::exit(1);
    }

    config.iterations = parse_size_arg("iteration count", arg);
    positional_iterations_set = true;
  }

  if (config.samples == 0u) {
    std::cerr << "Sample count must be greater than zero\n";
    std::exit(1);
  }

  return config;
}

/** @brief Compute min/median statistics from measured sample times. */
sample_stats summarize_samples(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  const double min_ms = samples.front();
  const std::size_t middle = samples.size() / 2u;
  const double median_ms = (samples.size() & 1u) != 0u
      ? samples[middle]
      : (samples[middle - 1u] + samples[middle]) * 0.5;
  return sample_stats{min_ms, median_ms};
}

template <typename Fn>
sample_stats measure_stats(const benchmark_config& config, Fn&& fn) {
  for (std::size_t warmup = 0; warmup < config.warmup_samples; ++warmup) {
    (void)measure_ms(config.iterations, fn);
  }

  std::vector<double> samples;
  samples.reserve(config.samples);
  for (std::size_t sample = 0; sample < config.samples; ++sample) {
    samples.push_back(measure_ms(config.iterations, fn));
  }
  return summarize_samples(std::move(samples));
}

/**
 * @brief Measure a benchmark with per-sample mutable state reset.
 *
 * `create_state()` is called once per warmup/measurement sample so mutation
 * inside `body(state, iteration)` does not leak across samples.
 */
template <typename StateFactory, typename Body>
sample_stats measure_stats_stateful(
    const benchmark_config& config,
    StateFactory&& create_state,
    Body&& body) {
  for (std::size_t warmup = 0; warmup < config.warmup_samples; ++warmup) {
    auto state = create_state();
    (void)measure_ms(config.iterations, [&](std::size_t iteration) {
      body(state, iteration);
    });
  }

  std::vector<double> samples;
  samples.reserve(config.samples);
  for (std::size_t sample = 0; sample < config.samples; ++sample) {
    auto state = create_state();
    samples.push_back(measure_ms(config.iterations, [&](std::size_t iteration) {
      body(state, iteration);
    }));
  }
  return summarize_samples(std::move(samples));
}

std::string format_stats(sample_stats stats) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << stats.min_ms << "/"
      << stats.median_ms;
  return out.str();
}

/** @brief Print benchmark results in human-readable fixed-width table format.
 */
void print_table(
    const benchmark_config& config,
    const std::vector<benchmark_row>& rows) {
  struct display_row {
    std::string operation;
    std::string cpp;
    std::string dyn;
    std::string js;
    std::string dyn_ratio;
    std::string json_ratio;
  };

  std::vector<display_row> display_rows;
  display_rows.reserve(rows.size());

  std::size_t operation_width = std::string("op").size();
  std::size_t cpp_width = std::string("cpp").size();
  std::size_t dyn_width = std::string("dyn").size();
  std::size_t json_width = std::string("json").size();
  std::size_t dyn_ratio_width = std::string("d/c").size();
  std::size_t json_ratio_width = std::string("j/c").size();

  for (const auto& row : rows) {
    std::ostringstream dyn_ratio_stream;
    std::ostringstream json_ratio_stream;
    dyn_ratio_stream << std::fixed << std::setprecision(2)
                     << ratio(row.native.median_ms, row.dynamic.median_ms);
    json_ratio_stream << std::fixed << std::setprecision(2)
                      << ratio(row.native.median_ms, row.json.median_ms);

    display_row rendered{
        row.operation,
        format_stats(row.native),
        format_stats(row.dynamic),
        format_stats(row.json),
        dyn_ratio_stream.str(),
        json_ratio_stream.str()};

    operation_width = std::max(operation_width, rendered.operation.size());
    cpp_width = std::max(cpp_width, rendered.cpp.size());
    dyn_width = std::max(dyn_width, rendered.dyn.size());
    json_width = std::max(json_width, rendered.js.size());
    dyn_ratio_width = std::max(dyn_ratio_width, rendered.dyn_ratio.size());
    json_ratio_width = std::max(json_ratio_width, rendered.json_ratio.size());

    display_rows.push_back(std::move(rendered));
  }

  const std::size_t border_width = 1 + operation_width + 1 + cpp_width + 1 +
      dyn_width + 1 + json_width + 1 + dyn_ratio_width + 1 + json_ratio_width +
      1;

  std::cout << "Bison performance comparison\n";
  std::cout << "Iterations per sample: " << config.iterations << "\n";
  std::cout << "Warmup samples: " << config.warmup_samples << "\n";
  std::cout << "Measured samples: " << config.samples << "\n";
#ifdef NDEBUG
  std::cout << "Build mode: Release\n";
#else
  std::cout << "Build mode: Debug (use Release for meaningful comparisons)\n";
#endif
  std::cout
      << "Stat cells show min/median milliseconds. Ratios use median times.\n\n";

  std::cout << std::string(border_width, '-') << "\n";
  std::cout << "|" << std::left << std::setw(static_cast<int>(operation_width))
            << "op"
            << "|" << std::right << std::setw(static_cast<int>(cpp_width))
            << "cpp"
            << "|" << std::setw(static_cast<int>(dyn_width)) << "dyn"
            << "|" << std::setw(static_cast<int>(json_width)) << "json"
            << "|" << std::setw(static_cast<int>(dyn_ratio_width)) << "d/c"
            << "|" << std::setw(static_cast<int>(json_ratio_width)) << "j/c"
            << "|\n";
  std::cout << std::string(border_width, '-') << "\n";

  for (const auto& row : display_rows) {
    std::cout << "|" << std::left
              << std::setw(static_cast<int>(operation_width)) << row.operation
              << "|" << std::right << std::setw(static_cast<int>(cpp_width))
              << row.cpp << "|" << std::setw(static_cast<int>(dyn_width))
              << row.dyn << "|" << std::setw(static_cast<int>(json_width))
              << row.js << "|" << std::setw(static_cast<int>(dyn_ratio_width))
              << row.dyn_ratio << "|"
              << std::setw(static_cast<int>(json_ratio_width)) << row.json_ratio
              << "|\n";
  }
  std::cout << std::string(border_width, '-') << "\n";

  std::cout << "\nOptimization guard: " << g_sink << "\n";
}

/** @brief Print benchmark results as CSV for tooling/automation. */
void print_csv(
    const benchmark_config& config,
    const std::vector<benchmark_row>& rows) {
  std::cout << "iterations,warmup_samples,measured_samples,operation,"
            << "cpp_min_ms,cpp_median_ms,dynamic_min_ms,dynamic_median_ms,"
            << "json_min_ms,json_median_ms,dynamic_vs_cpp,json_vs_cpp\n";
  for (const auto& row : rows) {
    std::cout << config.iterations << "," << config.warmup_samples << ","
              << config.samples << "," << row.operation << "," << std::fixed
              << std::setprecision(6) << row.native.min_ms << ","
              << row.native.median_ms << "," << row.dynamic.min_ms << ","
              << row.dynamic.median_ms << "," << row.json.min_ms << ","
              << row.json.median_ms << ","
              << ratio(row.native.median_ms, row.dynamic.median_ms) << ","
              << ratio(row.native.median_ms, row.json.median_ms) << "\n";
  }
}

/** @brief Print benchmark results as Markdown for docs/issues/PR comments. */
void print_markdown(
    const benchmark_config& config,
    const std::vector<benchmark_row>& rows) {
  std::cout << "Bison performance comparison\n\n";
  std::cout << "Iterations per sample: " << config.iterations << "  \n";
  std::cout << "Warmup samples: " << config.warmup_samples << "  \n";
  std::cout << "Measured samples: " << config.samples << "\n\n";
  std::cout
      << "| Operation | C++ min/med | dynamic min/med | json min/med | dyn x | json x |\n";
  std::cout << "|---|---:|---:|---:|---:|---:|\n";
  for (const auto& row : rows) {
    std::cout << "| " << row.operation << " | " << format_stats(row.native)
              << " | " << format_stats(row.dynamic) << " | "
              << format_stats(row.json) << " | " << std::fixed
              << std::setprecision(2)
              << ratio(row.native.median_ms, row.dynamic.median_ms) << " | "
              << ratio(row.native.median_ms, row.json.median_ms) << " |\n";
  }
  std::cout << "\nOptimization guard: " << g_sink << "\n";
}

} // namespace

int main(int argc, char** argv) {
  // Parse options and prebuild reusable fixtures used by serialization rows.
  const benchmark_config config = parse_config(argc, argv);
  const benchmark_pool pool = build_serialization_pool(SERIALIZATION_POOL_SIZE);
  std::vector<benchmark_row> rows;
  rows.reserve(7);

  // 1) Cost of constructing and tearing down objects each iteration.
  rows.push_back(
      {"Create / destroy",
       measure_stats(
           config,
           [](std::size_t iteration) {
             auto obj = make_native_record(iteration);
             consume_i32(obj.id + obj.level);
             consume_string(obj.name);
           }),
       measure_stats(
           config,
           [](std::size_t iteration) {
             auto obj = make_dynamic_record(iteration);
             consume_i32(
                 obj[KEY_ID].as<std::int32_t>() +
                 obj[KEY_LEVEL].as<std::int32_t>());
             consume_string(obj[KEY_NAME].as<std::string>());
           }),
       measure_stats(config, [](std::size_t iteration) {
         auto obj = make_json_record(iteration);
         consume_i32(
             obj["id"].get<std::int32_t>() + obj["level"].get<std::int32_t>());
         consume_string(obj["name"].get_ref<const std::string&>());
       })});

  // 2) Cost of repeatedly writing and reading fields on long-lived objects.
  rows.push_back(
      {"Field set / get",
       measure_stats_stateful(
           config,
           []() { return make_native_record(0); },
           [](NativeRecord& obj, std::size_t iteration) {
             obj.id = static_cast<std::int32_t>(iteration);
             obj.age = 18 + static_cast<std::int32_t>(iteration % 70u);
             obj.score = 10.0f + static_cast<float>(iteration % 100u) * 0.5f;
             obj.ratio = 0.1f + static_cast<float>(iteration % 25u) * 0.02f;
             obj.active = (iteration & 1u) == 0u;
             obj.level = static_cast<std::int32_t>(iteration % 12u);
             obj.name = benchmark_name(iteration);
             consume_i32(obj.id + obj.age + obj.level);
             consume_f32(obj.score + obj.ratio);
             consume_bool(obj.active);
             consume_string(obj.name);
           }),
       measure_stats_stateful(
           config,
           []() { return make_dynamic_record(0); },
           [](dynamic& obj, std::size_t iteration) {
             obj[KEY_ID] = static_cast<std::int32_t>(iteration);
             obj[KEY_AGE] = 18 + static_cast<std::int32_t>(iteration % 70u);
             obj[KEY_SCORE] =
                 10.0f + static_cast<float>(iteration % 100u) * 0.5f;
             obj[KEY_RATIO] =
                 0.1f + static_cast<float>(iteration % 25u) * 0.02f;
             obj[KEY_ACTIVE] = (iteration & 1u) == 0u;
             obj[KEY_LEVEL] = static_cast<std::int32_t>(iteration % 12u);
             obj[KEY_NAME] = std::string{benchmark_name(iteration)};
             consume_i32(
                 obj[KEY_ID].as<std::int32_t>() +
                 obj[KEY_AGE].as<std::int32_t>() +
                 obj[KEY_LEVEL].as<std::int32_t>());
             consume_f32(
                 obj[KEY_SCORE].as<float>() + obj[KEY_RATIO].as<float>());
             consume_bool(obj[KEY_ACTIVE].as<bool>());
             consume_string(obj[KEY_NAME].as<std::string>());
           }),
       measure_stats_stateful(
           config,
           []() { return make_json_record(0); },
           [](json& obj, std::size_t iteration) {
             obj["id"] = static_cast<std::int32_t>(iteration);
             obj["age"] = 18 + static_cast<std::int32_t>(iteration % 70u);
             obj["score"] = 10.0 + static_cast<double>(iteration % 100u) * 0.5;
             obj["ratio"] = 0.1 + static_cast<double>(iteration % 25u) * 0.02;
             obj["active"] = (iteration & 1u) == 0u;
             obj["level"] = static_cast<std::int32_t>(iteration % 12u);
             obj["name"] = benchmark_name(iteration);
             consume_i32(
                 obj["id"].get<std::int32_t>() +
                 obj["age"].get<std::int32_t>() +
                 obj["level"].get<std::int32_t>());
             consume_f32(
                 static_cast<float>(
                     obj["score"].get<double>() + obj["ratio"].get<double>()));
             consume_bool(obj["active"].get<bool>());
             consume_string(obj["name"].get_ref<const std::string&>());
           })});

  // 3) Cost of method-like execution on mutable object state.
  rows.push_back(
      {"Method-style call",
       measure_stats_stateful(
           config,
           []() { return make_native_record(0); },
           [](NativeRecord& obj, std::size_t iteration) {
             const NativeParams params = make_native_params(iteration);
             consume_i32(obj.compute(params));
           }),
       measure_stats_stateful(
           config,
           []() { return make_dynamic_method_record(0); },
           [](dynamic& obj, std::size_t iteration) {
             dynamic params = make_dynamic_params(iteration);
             dynamic result = obj.call("compute"_key, params);
             consume_i32(result[KEY_VALUE].as<std::int32_t>());
           }),
       measure_stats_stateful(
           config,
           []() { return make_json_record(0); },
           [](json& obj, std::size_t iteration) {
             json params = make_json_params(iteration);
             json result{{"value", json_compute(obj, params)}};
             consume_i32(result["value"].get<std::int32_t>());
           })});

  // 4) Pure serialization cost using prebuilt object fixtures.
  rows.push_back(
      {"Serialize",
       measure_stats(
           config,
           [&pool](std::size_t iteration) {
             const NativeRecord& obj =
                 pool.native_records[iteration % pool.native_records.size()];
             const std::string payload = serialize_native_record(obj);
             consume_u64(static_cast<std::uint64_t>(payload.size()));
           }),
       measure_stats(
           config,
           [&pool](std::size_t iteration) {
             const dynamic& obj =
                 pool.dynamic_records[iteration % pool.dynamic_records.size()];
             const std::string payload = serialize_dynamic_record(obj);
             consume_u64(static_cast<std::uint64_t>(payload.size()));
           }),
       measure_stats(config, [&pool](std::size_t iteration) {
         const json& obj =
             pool.json_records[iteration % pool.json_records.size()];
         const std::string payload = serialize_json_record(obj);
         consume_u64(static_cast<std::uint64_t>(payload.size()));
       })});

  // 4b) Serialization using buffer_serializer (no stream overhead).
  rows.push_back(
      {"Serialize (buf)",
       measure_stats(
           config,
           [&pool](std::size_t iteration) {
             const NativeRecord& obj =
                 pool.native_records[iteration % pool.native_records.size()];
             const std::string payload = serialize_native_record(obj);
             consume_u64(static_cast<std::uint64_t>(payload.size()));
           }),
       measure_stats(
           config,
           [&pool](std::size_t iteration) {
             const dynamic& obj =
                 pool.dynamic_records[iteration % pool.dynamic_records.size()];
             const auto payload = serialize_dynamic_record_buf(obj);
             consume_u64(static_cast<std::uint64_t>(payload.size()));
           }),
       measure_stats(config, [&pool](std::size_t iteration) {
         const json& obj =
             pool.json_records[iteration % pool.json_records.size()];
         const std::string payload = serialize_json_record(obj);
         consume_u64(static_cast<std::uint64_t>(payload.size()));
       })});

  // 5) Pure deserialization cost using prebuilt payload fixtures.
  rows.push_back(
      {"Deserialize",
       measure_stats(
           config,
           [&pool](std::size_t iteration) {
             const std::string& payload =
                 pool.native_payloads[iteration % pool.native_payloads.size()];
             const NativeRecord obj = deserialize_native_record(payload);
             consume_i32(obj.id + obj.level);
             consume_string(obj.name);
           }),
       measure_stats(
           config,
           [&pool](std::size_t iteration) {
             const std::string& payload =
                 pool.dynamic_payloads
                     [iteration % pool.dynamic_payloads.size()];
             const auto obj = deserialize_dynamic_record(payload);
             consume_i32(
               obj[KEY_ID].as<std::int32_t>() +
               obj[KEY_LEVEL].as<std::int32_t>());
             consume_string(obj[KEY_NAME].as<std::string>());
           }),
       measure_stats(config, [&pool](std::size_t iteration) {
         const std::string& payload =
             pool.json_payloads[iteration % pool.json_payloads.size()];
         const json obj = deserialize_json_record(payload);
         consume_i32(
             obj["id"].get<std::int32_t>() + obj["level"].get<std::int32_t>());
         consume_string(obj["name"].get_ref<const std::string&>());
       })});

  // 5b) Deserialization using buffer_deserializer (no stream overhead).
  rows.push_back(
      {"Deserialize (buf)",
       measure_stats(
           config,
           [&pool](std::size_t iteration) {
             const std::string& payload =
                 pool.native_payloads[iteration % pool.native_payloads.size()];
             const NativeRecord obj = deserialize_native_record(payload);
             consume_i32(obj.id + obj.level);
             consume_string(obj.name);
           }),
       measure_stats(
           config,
           [&pool](std::size_t iteration) {
             const buffer& payload =
                 pool.dynamic_buf_payloads
                     [iteration % pool.dynamic_buf_payloads.size()];
             const auto obj = deserialize_dynamic_record_buf(payload);
             consume_i32(
               obj[KEY_ID].as<std::int32_t>() +
               obj[KEY_LEVEL].as<std::int32_t>());
             consume_string(obj[KEY_NAME].as<std::string>());
           }),
       measure_stats(config, [&pool](std::size_t iteration) {
         const std::string& payload =
             pool.json_payloads[iteration % pool.json_payloads.size()];
         const json obj = deserialize_json_record(payload);
         consume_i32(
             obj["id"].get<std::int32_t>() + obj["level"].get<std::int32_t>());
         consume_string(obj["name"].get_ref<const std::string&>());
       })});

  // Render results in the user-selected output format.
  switch (config.format) {
    case output_format::table:
      print_table(config, rows);
      break;
    case output_format::csv:
      print_csv(config, rows);
      break;
    case output_format::markdown:
      print_markdown(config, rows);
      break;
  }

  return 0;
}