// Deterministic decoding attack on the canonical reader.
//
// The generator is seeded from a constant that every failure message repeats,
// so a failing run is reproducible byte for byte. The decoder below reads a
// fixed sequence of fields and iterates only over an element count it has
// already bounded, so no loop in this file is driven by untrusted input: that
// is what makes "never loops forever" a property of the test rather than a hope
// about the payload.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dce/canonical.hpp"
#include "dce/digest.hpp"
#include "harness.hpp"

namespace {

// The one constant that decides the whole run. Reproduce a failure by dropping
// it back into the generator.
constexpr std::uint64_t kSeed = 0x5eed1234c0ffee11ull;

constexpr std::size_t kRandomCases = 7000;
constexpr std::size_t kTruncationCases = 6500;
constexpr std::size_t kMutationCases = 6500;
constexpr std::size_t kFuzzCases = kRandomCases + kTruncationCases + kMutationCases;

constexpr std::size_t kMaxRandomBytes = 96;
constexpr std::size_t kMaxDecodedString = 256;
constexpr std::size_t kMaxDecodedBlob = 512;
constexpr std::size_t kMaxDecodedElements = 64;
constexpr std::size_t kMaxCorpusBlob = 128;
constexpr std::size_t kMaxCorpusElements = 8;

// The one message layout the fuzzer attacks. Every primitive of the codec
// appears exactly once, so a mutation anywhere lands in a different decoder.
struct Message {
  std::uint8_t byte{0};
  bool flag{false};
  std::uint16_t short_value{0};
  std::uint32_t word{0};
  std::uint64_t wide{0};
  std::int64_t signed_wide{0};
  dce::Digest256 digest{};
  std::string text;
  std::vector<std::byte> blob;
  std::vector<std::uint32_t> elements;
};

std::string hex_of(std::span<const std::byte> data) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2u);
  for (std::byte value : data) {
    const auto byte = static_cast<unsigned>(value);
    out.push_back(kDigits[(byte >> 4) & 0x0fu]);
    out.push_back(kDigits[byte & 0x0fu]);
  }
  return out;
}

// Encodes a generated message. Every field is inside the documented bounds, so
// a failed write here is a defect in the codec rather than a bad input.
std::vector<std::byte> encode(const Message& message) {
  dce::CanonicalWriter writer;
  DCE_CHECK_OK(writer.put_u8(message.byte));
  DCE_CHECK_OK(writer.put_bool(message.flag));
  DCE_CHECK_OK(writer.put_u16(message.short_value));
  DCE_CHECK_OK(writer.put_u32(message.word));
  DCE_CHECK_OK(writer.put_u64(message.wide));
  DCE_CHECK_OK(writer.put_i64(message.signed_wide));
  DCE_CHECK_OK(writer.put_digest(message.digest));
  DCE_CHECK_OK(writer.put_string(message.text));
  DCE_CHECK_OK(writer.put_bytes(message.blob));
  DCE_CHECK_OK(writer.put_count(message.elements.size()));
  for (std::uint32_t element : message.elements) {
    DCE_CHECK_OK(writer.put_u32(element));
  }
  return writer.bytes();
}

struct DecodeResult {
  std::optional<Message> message;
  bool in_bounds{true};
  dce::ErrorCode code{dce::ErrorCode::ok};
};

DecodeResult decode(std::span<const std::byte> payload) {
  DecodeResult result;
  bool& in_bounds = result.in_bounds;
  Message message;
  dce::CanonicalReader reader(payload);
  std::size_t previous = 0;
  // Called after every read: the position must only ever move forward, and it
  // must never leave the payload it was given.
  const auto step = [&reader, &previous, &in_bounds, payload]() {
    const std::size_t current = reader.offset();
    if (current < previous || current > payload.size()) {
      in_bounds = false;
    }
    previous = current;
  };

  const auto byte = reader.u8();
  step();
  if (!byte.ok()) {
    result.code = byte.code();
    return result;
  }
  message.byte = *byte;

  const auto flag = reader.boolean();
  step();
  if (!flag.ok()) {
    result.code = flag.code();
    return result;
  }
  message.flag = *flag;

  const auto short_value = reader.u16();
  step();
  if (!short_value.ok()) {
    result.code = short_value.code();
    return result;
  }
  message.short_value = *short_value;

  const auto word = reader.u32();
  step();
  if (!word.ok()) {
    result.code = word.code();
    return result;
  }
  message.word = *word;

  const auto wide = reader.u64();
  step();
  if (!wide.ok()) {
    result.code = wide.code();
    return result;
  }
  message.wide = *wide;

  const auto signed_wide = reader.i64();
  step();
  if (!signed_wide.ok()) {
    result.code = signed_wide.code();
    return result;
  }
  message.signed_wide = *signed_wide;

  const auto digest = reader.digest();
  step();
  if (!digest.ok()) {
    result.code = digest.code();
    return result;
  }
  message.digest = *digest;

  const auto text = reader.string(kMaxDecodedString);
  step();
  if (!text.ok()) {
    result.code = text.code();
    return result;
  }
  message.text = *text;

  const auto blob = reader.bytes(kMaxDecodedBlob);
  step();
  if (!blob.ok()) {
    result.code = blob.code();
    return result;
  }
  message.blob.assign(blob->begin(), blob->end());

  const auto count = reader.count(kMaxDecodedElements);
  step();
  if (!count.ok()) {
    result.code = count.code();
    return result;
  }
  // The loop is bounded by the value the reader already accepted, never by the
  // number the payload claims.
  for (std::size_t index = 0; index < *count; ++index) {
    const auto element = reader.u32();
    step();
    if (!element.ok()) {
      result.code = element.code();
      return result;
    }
    message.elements.push_back(*element);
  }

  const dce::Status ended = reader.expect_end();
  if (!ended.ok()) {
    result.code = ended.code();
    return result;
  }
  result.message = std::move(message);
  return result;
}

// Verifies every invariant the decoder promises. An empty result means the
// payload behaved; otherwise the text names the invariant that was broken.
std::string check_payload(const std::vector<std::byte>& payload, bool& accepted) {
  accepted = false;
  const DecodeResult decoded = decode(payload);
  if (!decoded.in_bounds) {
    return "the reader moved backwards or read past the end of the payload";
  }
  if (!decoded.message.has_value()) {
    if (decoded.code == dce::ErrorCode::ok) {
      return "a rejected payload reported success";
    }
    return {};
  }
  const Message& message = *decoded.message;
  if (decoded.code != dce::ErrorCode::ok) {
    return "an accepted payload carried a failure code";
  }
  if (message.text.size() > kMaxDecodedString) {
    return "a decoded string exceeded the requested maximum";
  }
  if (message.blob.size() > kMaxDecodedBlob) {
    return "a decoded blob exceeded the requested maximum";
  }
  if (message.elements.size() > kMaxDecodedElements) {
    return "a decoded element count exceeded the accepted maximum";
  }
  // A payload that decoded completely must re-encode to exactly the same
  // bytes: the encoding is canonical, so one value has exactly one form.
  if (encode(message) != payload) {
    return "a fully decoded payload did not re-encode to the same bytes";
  }
  accepted = true;
  return {};
}

std::string describe(std::string_view strategy, std::size_t index, std::span<const std::byte> payload,
                     std::string_view problem) {
  std::string out = "seed=";
  out += std::to_string(kSeed);
  out += " case=";
  out += std::to_string(index);
  out += " strategy=";
  out += strategy;
  out += " payload=";
  out += hex_of(payload);
  out += " invariant=\"";
  out += problem;
  out += '"';
  return out;
}

std::vector<std::byte> random_payload(std::mt19937_64& generator) {
  const std::size_t length = std::uniform_int_distribution<std::size_t>(0u, kMaxRandomBytes)(generator);
  std::uniform_int_distribution<unsigned> byte_distribution(0u, 255u);
  std::vector<std::byte> out(length);
  for (std::size_t index = 0; index < length; ++index) {
    out[index] = static_cast<std::byte>(byte_distribution(generator));
  }
  return out;
}

std::vector<std::byte> truncated(std::mt19937_64& generator, const std::vector<std::byte>& source) {
  const std::size_t length = std::uniform_int_distribution<std::size_t>(0u, source.size())(generator);
  std::vector<std::byte> out = source;
  out.resize(length);
  return out;
}

std::vector<std::byte> mutated(std::mt19937_64& generator, const std::vector<std::byte>& source) {
  std::vector<std::byte> out = source;
  std::uniform_int_distribution<unsigned> byte_distribution(0u, 255u);
  if (out.empty()) {
    out.push_back(static_cast<std::byte>(byte_distribution(generator)));
    return out;
  }
  const std::size_t position =
      std::uniform_int_distribution<std::size_t>(0u, out.size() - 1u)(generator);
  // A non-zero mask guarantees the mutated byte differs from the original.
  const unsigned mask = std::uniform_int_distribution<unsigned>(1u, 255u)(generator);
  out[position] = static_cast<std::byte>(static_cast<unsigned>(out[position]) ^ mask);
  return out;
}

std::size_t pick_index(std::mt19937_64& generator, std::size_t size) {
  return std::uniform_int_distribution<std::size_t>(0u, size - 1u)(generator);
}

// Valid encodings to mutate and truncate: the empty payload, the smallest
// complete message, one message at every length boundary, and a few random
// ones. Every entry is generated from the seeded generator, so the corpus is
// part of the reproducible run.
std::vector<std::vector<std::byte>> build_corpus(std::mt19937_64& generator) {
  std::vector<std::vector<std::byte>> corpus;
  corpus.push_back(std::vector<std::byte>{});

  const Message smallest;
  corpus.push_back(encode(smallest));

  Message longest;
  longest.byte = static_cast<std::uint8_t>(0xffu);
  longest.flag = true;
  longest.short_value = static_cast<std::uint16_t>(0xffffu);
  longest.word = 0xffffffffu;
  longest.wide = 0xffffffffffffffffull;
  longest.signed_wide = -1;
  longest.digest = dce::Digest256::of(std::string_view("adversarial corpus"));
  longest.text = std::string(kMaxDecodedString, 'w');
  longest.blob = std::vector<std::byte>(kMaxCorpusBlob, std::byte{0xff});
  longest.elements.assign(kMaxCorpusElements, 0xffffffffu);
  corpus.push_back(encode(longest));

  std::uniform_int_distribution<unsigned> byte_distribution(0u, 255u);
  for (int round = 0; round < 6; ++round) {
    Message message;
    message.byte = static_cast<std::uint8_t>(byte_distribution(generator));
    message.flag = byte_distribution(generator) % 2u == 0u;
    message.short_value = static_cast<std::uint16_t>(byte_distribution(generator));
    message.word = static_cast<std::uint32_t>(byte_distribution(generator));
    message.wide = static_cast<std::uint64_t>(byte_distribution(generator));
    message.signed_wide = -static_cast<std::int64_t>(byte_distribution(generator));
    message.text = std::string(byte_distribution(generator) % 32u, 't');
    message.blob = std::vector<std::byte>(byte_distribution(generator) % 32u, std::byte{0x5a});
    message.elements.assign(byte_distribution(generator) % 8u, 7u);
    corpus.push_back(encode(message));
  }
  return corpus;
}

}  // namespace

DCE_TEST(adversarial, generator_is_reproducible) {
  std::mt19937_64 first(kSeed);
  std::mt19937_64 second(kSeed);
  for (int round = 0; round < 64; ++round) {
    DCE_CHECK_EQ(hex_of(random_payload(first)), hex_of(random_payload(second)));
    DCE_CHECK_EQ(pick_index(first, 1000u), pick_index(second, 1000u));
  }
}

DCE_TEST(adversarial, canonical_decoder_survives_hostile_bytes) {
  std::mt19937_64 generator(kSeed);
  const std::vector<std::vector<std::byte>> corpus = build_corpus(generator);
  DCE_REQUIRE(!corpus.empty());

  std::size_t accepted = 0;
  std::size_t rejected = 0;
  std::size_t executed = 0;

  const auto run = [&](std::string_view strategy, std::size_t index, const std::vector<std::byte>& payload) {
    ++executed;
    dce::test::count_assertion();
    bool decoded = false;
    const std::string problem = check_payload(payload, decoded);
    if (problem.empty()) {
      if (decoded) {
        ++accepted;
      } else {
        ++rejected;
      }
      return true;
    }
    DCE_FAIL(describe(strategy, index, payload, problem));
    return false;
  };

  // The pristine corpus first: it proves the success path is exercised, not
  // only the rejection path.
  std::size_t case_index = 0;
  for (const std::vector<std::byte>& source : corpus) {
    if (!run("canonical", case_index, source)) {
      return;
    }
    ++case_index;
  }

  for (std::size_t index = 0; index < kRandomCases; ++index) {
    if (!run("random", index, random_payload(generator))) {
      return;
    }
  }
  for (std::size_t index = 0; index < kTruncationCases; ++index) {
    if (!run("truncation", index, truncated(generator, corpus[pick_index(generator, corpus.size())]))) {
      return;
    }
  }
  for (std::size_t index = 0; index < kMutationCases; ++index) {
    if (!run("mutation", index, mutated(generator, corpus[pick_index(generator, corpus.size())]))) {
      return;
    }
  }

  DCE_CHECK_EQ(executed, kFuzzCases + corpus.size());
  // Both halves of the contract must have been observed: payloads that decode
  // and payloads that are refused.
  DCE_CHECK_TRUE(accepted > 0u);
  DCE_CHECK_TRUE(rejected > 0u);
  DCE_CHECK_EQ(accepted + rejected, executed);
}
