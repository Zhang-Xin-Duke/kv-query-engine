#include <charconv>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "kvquery/kvquery.hpp"

using namespace kvquery;
namespace {
uint64_t number(std::string_view s) {
  uint64_t value = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  if (ec != std::errc{} || p != s.data() + s.size())
    throw std::invalid_argument("expected unsigned decimal integer: " + std::string(s));
  return value;
}
Strategy strategy(std::string_view s) {
  if (s == "scalar") return Strategy::scalar;
  if (s == "prefetch") return Strategy::prefetch;
  if (s == "amac") return Strategy::amac;
  if (s == "simd") return Strategy::simd;
  throw std::invalid_argument("unknown strategy: " + std::string(s));
}
void usage() {
  std::cerr
      << "KVQueryEngine: High-Performance KV Query Engine for Read-Heavy Workloads\n"
         "  kvquery build INPUT.tsv OUTPUT.kwv\n"
         "  kvquery query SNAPSHOT.kwv [--strategy scalar|prefetch|amac|simd] [--window N] KEY...\n"
         "  kvquery inspect SNAPSHOT.kwv\n"
         "TSV: unsigned decimal key<TAB>text value; query values are printed as hex.\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      usage();
      return 2;
    }
    const std::string command = argv[1];
    if (command == "build") {
      if (argc != 4) {
        usage();
        return 2;
      }
      if (std::filesystem::file_size(argv[2]) > (512ULL << 20))
        throw std::length_error("TSV exceeds 512 MiB limit");
      std::ifstream in(argv[2]);
      if (!in) throw std::runtime_error("cannot open TSV");
      std::vector<Record> records;
      std::string line;
      size_t row = 0;
      while (std::getline(in, line)) {
        ++row;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto tab = line.find('\t');
        if (tab == std::string::npos)
          throw std::runtime_error("missing tab at row " + std::to_string(row));
        records.push_back({number(std::string_view(line).substr(0, tab)), line.substr(tab + 1)});
        if (records.size() > 10'000'000) throw std::length_error("TSV record limit exceeded");
      }
      if (!in.eof()) throw std::runtime_error("TSV read failed");
      auto s = Snapshot::build(records);
      s->save(argv[3]);
      std::cout << "saved " << s->stats().records << " records\n";
    } else if (command == "query") {
      if (argc < 4) {
        usage();
        return 2;
      }
      QueryOptions options;
      std::vector<uint64_t> keys;
      for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--strategy" || arg == "--window") {
          if (i + 1 == argc) throw std::invalid_argument("option is missing a value");
          const std::string_view value = argv[++i];
          if (arg == "--strategy")
            options.strategy = strategy(value);
          else
            options.window = static_cast<size_t>(number(value));
        } else
          keys.push_back(number(arg));
      }
      if (keys.empty()) throw std::invalid_argument("at least one key is required");
      auto s = Snapshot::load(argv[2]);
      const auto result = s->query(keys, options);
      constexpr char hex[] = "0123456789abcdef";
      for (size_t i = 0; i < keys.size(); ++i) {
        std::cout << keys[i] << '\t';
        if (!result[i])
          std::cout << "MISSING\n";
        else {
          std::cout << "FOUND\t";
          for (unsigned char c : *result[i]) std::cout << hex[c >> 4] << hex[c & 15];
          std::cout << '\n';
        }
      }
    } else if (command == "inspect") {
      if (argc != 3) {
        usage();
        return 2;
      }
      const auto s = Snapshot::load(argv[2])->stats();
      std::cout << "{\"records\":" << s.records << ",\"buckets\":" << s.buckets
                << ",\"index_bytes\":" << s.index_bytes << ",\"value_bytes\":" << s.value_bytes
                << ",\"inline_records\":" << s.inline_records
                << ",\"inline_value_bytes\":" << s.inline_value_bytes
                << ",\"collisions\":" << s.collisions << ",\"relocations\":" << s.relocations
                << ",\"build_attempts\":" << s.build_attempts << "}\n";
    } else {
      usage();
      return 2;
    }
  } catch (const std::exception& e) {
    std::cerr << "kvquery: " << e.what() << '\n';
    return 1;
  }
}
