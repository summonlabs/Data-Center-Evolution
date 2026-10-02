// Durability, integrity and recovery.
//
// The claims under test are specific: a committed record survives, a torn tail
// is distinguished from interior corruption, corruption is never silently
// repaired, and a real process that dies without closing leaves a store that
// still opens.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dce/platform.hpp"
#include "dce/store.hpp"
#include "dce/text.hpp"
#include "harness.hpp"

namespace {

using dce::persist::OpenMode;
using dce::persist::RecordType;
using dce::persist::Store;
using dce::persist::StoreOptions;

std::string temporary_root(const std::string& name) {
  const char* base = std::getenv("TEMP");
  if (base == nullptr) {
    base = std::getenv("TMPDIR");
  }
  if (base == nullptr) {
    base = ".";
  }
  return dce::platform::join_path(
      base, "dce-store-" + name + "-" + std::to_string(dce::platform::process_id()));
}

std::vector<std::byte> payload_for(std::uint64_t index) {
  const std::string text = "record-" + dce::text::u64_to_string(index);
  std::vector<std::byte> bytes(text.size());
  std::transform(text.begin(), text.end(), bytes.begin(),
                 [](char c) { return static_cast<std::byte>(static_cast<unsigned char>(c)); });
  return bytes;
}

std::string payload_text(const std::vector<std::byte>& payload) {
  return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

// The segment files a store currently holds, in ascending name order.
std::vector<std::string> segments(const std::string& directory) {
  std::vector<std::string> found;
  dce::Result<std::vector<std::string>> entries = dce::platform::list_directory(directory);
  if (!entries.ok()) {
    return found;
  }
  for (const std::string& entry : *entries) {
    if (entry.rfind("LOG-", 0) == 0) {
      found.push_back(entry);
    }
  }
  std::sort(found.begin(), found.end());
  return found;
}

void overwrite_byte(const std::string& path, std::uint64_t offset, unsigned char value) {
  dce::Result<dce::platform::File> file =
      dce::platform::File::open(path, dce::platform::OpenMode::read_write);
  DCE_REQUIRE_OK(file);
  const std::byte raw = static_cast<std::byte>(value);
  DCE_REQUIRE(file->write_at(offset, std::span<const std::byte>(&raw, 1)).ok());
  DCE_REQUIRE(file->sync().ok());
}

}  // namespace

DCE_TEST(persistence, commits_and_recovers_in_order) {
  const std::string directory = temporary_root("recover");
  DCE_REQUIRE(dce::platform::make_directories(directory).ok());

  StoreOptions options;
  {
    dce::Result<Store> store = Store::open(directory, options, OpenMode::read_write);
    DCE_REQUIRE_OK(store);
    for (std::uint64_t index = 0; index < 32; ++index) {
      const std::vector<std::byte> payload = payload_for(index);
      dce::Result<dce::persist::CommitReceipt> receipt =
          store->append(RecordType::migration_receipt, payload);
      DCE_REQUIRE_OK(receipt);
      DCE_CHECK_TRUE(receipt->durable);
      DCE_CHECK_EQ(receipt->sequence.value(), index + 1);
    }
    DCE_REQUIRE(store->close().ok());
  }
  {
    dce::Result<Store> store = Store::open(directory, options, OpenMode::read_only);
    DCE_REQUIRE_OK(store);
    dce::Result<dce::persist::RecoveredState> state = store->recover();
    DCE_REQUIRE_OK(state);
    DCE_CHECK_EQ(state->records.size(), static_cast<std::size_t>(32));
    for (std::size_t index = 0; index < state->records.size(); ++index) {
      DCE_CHECK_EQ(payload_text(state->records[index].payload),
                   std::string("record-") + std::to_string(index));
    }
    DCE_CHECK_TRUE(!store->recovery().torn_tail_recovered);
  }
  DCE_CHECK_TRUE(dce::platform::remove_tree(directory).ok());
}

DCE_TEST(persistence, a_torn_tail_is_discarded_and_reported) {
  const std::string directory = temporary_root("torn");
  DCE_REQUIRE(dce::platform::make_directories(directory).ok());
  StoreOptions options;

  {
    dce::Result<Store> store = Store::open(directory, options, OpenMode::read_write);
    DCE_REQUIRE_OK(store);
    for (std::uint64_t index = 0; index < 8; ++index) {
      DCE_REQUIRE_OK(store->append(RecordType::migration_receipt, payload_for(index)));
    }
    DCE_REQUIRE(store->close().ok());
  }

  const std::vector<std::string> before = segments(directory);
  DCE_REQUIRE(!before.empty());
  const std::string active = dce::platform::join_path(directory, before.back());
  dce::Result<dce::platform::FileStat> stat = dce::platform::stat_path(active);
  DCE_REQUIRE_OK(stat);

  // A crash in the middle of writing a record leaves a partial record past the
  // last committed one: bytes that begin a header and then stop. Thirty bytes
  // is shorter than a header, which is precisely the shape the scanner must
  // classify as a torn tail rather than as damage to committed state.
  const std::uint64_t junk = 30;
  {
    dce::Result<dce::platform::File> file =
        dce::platform::File::open(active, dce::platform::OpenMode::read_write);
    DCE_REQUIRE_OK(file);
    std::vector<std::byte> partial(static_cast<std::size_t>(junk), std::byte{0x5a});
    DCE_REQUIRE(file->write_at(stat->size, partial).ok());
    DCE_REQUIRE(file->sync().ok());
  }

  {
    dce::Result<Store> store = Store::open(directory, options, OpenMode::read_write);
    DCE_REQUIRE_OK(store);
    DCE_CHECK_TRUE(store->recovery().torn_tail_recovered);
    DCE_CHECK_EQ(store->recovery().torn_tail_bytes_discarded, junk);
    dce::Result<dce::persist::RecoveredState> state = store->recover();
    DCE_REQUIRE_OK(state);
    DCE_CHECK_EQ(state->records.size(), static_cast<std::size_t>(8));
    for (std::size_t index = 0; index < state->records.size(); ++index) {
      DCE_CHECK_EQ(payload_text(state->records[index].payload),
                   std::string("record-") + std::to_string(index));
    }
  }
  DCE_CHECK_TRUE(dce::platform::remove_tree(directory).ok());
}

DCE_TEST(persistence, interior_corruption_is_refused_and_never_truncated) {
  const std::string directory = temporary_root("interior");
  DCE_REQUIRE(dce::platform::make_directories(directory).ok());
  StoreOptions options;

  {
    dce::Result<Store> store = Store::open(directory, options, OpenMode::read_write);
    DCE_REQUIRE_OK(store);
    for (std::uint64_t index = 0; index < 8; ++index) {
      DCE_REQUIRE_OK(store->append(RecordType::migration_receipt, payload_for(index)));
    }
    DCE_REQUIRE(store->close().ok());
  }

  const std::vector<std::string> before = segments(directory);
  DCE_REQUIRE(!before.empty());
  const std::string active = dce::platform::join_path(directory, before.back());
  dce::Result<dce::platform::FileStat> stat = dce::platform::stat_path(active);
  DCE_REQUIRE_OK(stat);
  const std::uint64_t size_before = stat->size;

  // Corrupt a byte inside the payload of the third record, well away from both
  // ends, so a later valid record still exists after it.
  overwrite_byte(active, 100 + 2 * (100 + 8) + 3, 0xff);

  {
    dce::Result<Store> store = Store::open(directory, options, OpenMode::read_write);
    DCE_CHECK_TRUE(!store.ok());
    DCE_CHECK_EQ(store.code(), dce::ErrorCode::corruption);
  }
  dce::Result<dce::platform::FileStat> after = dce::platform::stat_path(active);
  DCE_REQUIRE_OK(after);
  DCE_CHECK_EQ(after->size, size_before);
  DCE_CHECK_TRUE(dce::platform::remove_tree(directory).ok());
}

DCE_TEST(persistence, a_second_writer_is_refused_but_a_reader_is_not) {
  const std::string directory = temporary_root("lock");
  DCE_REQUIRE(dce::platform::make_directories(directory).ok());
  StoreOptions options;

  dce::Result<Store> writer = Store::open(directory, options, OpenMode::read_write);
  DCE_REQUIRE_OK(writer);
  DCE_REQUIRE_OK(writer->append(RecordType::plan_revision, payload_for(1)));

  dce::Result<Store> second = Store::open(directory, options, OpenMode::read_write);
  DCE_CHECK_TRUE(!second.ok());
  DCE_CHECK_EQ(second.code(), dce::ErrorCode::busy);

  dce::Result<Store> reader = Store::open(directory, options, OpenMode::read_only);
  DCE_REQUIRE_OK(reader);
  dce::Result<dce::persist::RecoveredState> state = reader->recover();
  DCE_REQUIRE_OK(state);
  DCE_CHECK_TRUE(state->records.size() >= 1);

  DCE_REQUIRE(writer->close().ok());
  DCE_CHECK_TRUE(dce::platform::remove_tree(directory).ok());
}

DCE_TEST(persistence, the_epoch_fences_across_reopen) {
  const std::string directory = temporary_root("epoch");
  DCE_REQUIRE(dce::platform::make_directories(directory).ok());
  StoreOptions options;

  std::uint64_t first_epoch = 0;
  {
    dce::Result<Store> store = Store::open(directory, options, OpenMode::read_write);
    DCE_REQUIRE_OK(store);
    first_epoch = store->epoch().value();
    DCE_CHECK_TRUE(first_epoch > 0);
    DCE_REQUIRE(store->close().ok());
  }
  {
    dce::Result<Store> reader = Store::open(directory, options, OpenMode::read_only);
    DCE_REQUIRE_OK(reader);
    DCE_CHECK_EQ(reader->epoch().value(), first_epoch);
  }
  {
    dce::Result<Store> store = Store::open(directory, options, OpenMode::read_write);
    DCE_REQUIRE_OK(store);
    DCE_CHECK_TRUE(store->epoch().value() > first_epoch);
    DCE_REQUIRE(store->close().ok());
  }
  DCE_CHECK_TRUE(dce::platform::remove_tree(directory).ok());
}

DCE_TEST(persistence, replay_is_idempotent_and_conflicts_are_refused) {
  const std::string directory = temporary_root("replay");
  DCE_REQUIRE(dce::platform::make_directories(directory).ok());
  StoreOptions options;

  dce::Result<Store> store = Store::open(directory, options, OpenMode::read_write);
  DCE_REQUIRE_OK(store);
  const std::vector<std::byte> payload = payload_for(7);
  dce::Result<dce::persist::CommitReceipt> first =
      store->append(RecordType::stage_checkpoint, payload);
  DCE_REQUIRE_OK(first);

  dce::Result<dce::persist::CommitReceipt> duplicate =
      store->replay(RecordType::stage_checkpoint, first->sequence, payload);
  DCE_REQUIRE_OK(duplicate);
  DCE_CHECK_TRUE(duplicate->duplicate);

  const std::vector<std::byte> different = payload_for(8);
  dce::Result<dce::persist::CommitReceipt> conflict =
      store->replay(RecordType::stage_checkpoint, first->sequence, different);
  DCE_CHECK_TRUE(!conflict.ok());
  DCE_CHECK_TRUE(conflict.code() == dce::ErrorCode::conflict ||
                 conflict.code() == dce::ErrorCode::corruption);

  DCE_REQUIRE(store->close().ok());
  DCE_CHECK_TRUE(dce::platform::remove_tree(directory).ok());
}

DCE_TEST(persistence, a_snapshot_preserves_post_snapshot_records) {
  const std::string directory = temporary_root("snapshot");
  DCE_REQUIRE(dce::platform::make_directories(directory).ok());
  StoreOptions options;

  const std::string image = "the state image the snapshot covers";
  std::vector<std::byte> image_bytes(image.size());
  std::transform(image.begin(), image.end(), image_bytes.begin(),
                 [](char c) { return static_cast<std::byte>(static_cast<unsigned char>(c)); });

  {
    dce::Result<Store> store = Store::open(directory, options, OpenMode::read_write);
    DCE_REQUIRE_OK(store);
    for (std::uint64_t index = 0; index < 5; ++index) {
      DCE_REQUIRE_OK(store->append(RecordType::migration_receipt, payload_for(index)));
    }
    DCE_REQUIRE(store->snapshot(image_bytes).ok());
    // Records committed after the snapshot must still be there afterwards.
    for (std::uint64_t index = 5; index < 9; ++index) {
      DCE_REQUIRE_OK(store->append(RecordType::migration_receipt, payload_for(index)));
    }
    DCE_REQUIRE(store->close().ok());
  }
  {
    dce::Result<Store> store = Store::open(directory, options, OpenMode::read_only);
    DCE_REQUIRE_OK(store);
    dce::Result<dce::persist::RecoveredState> state = store->recover();
    DCE_REQUIRE_OK(state);
    DCE_CHECK_TRUE(store->recovery().snapshot_loaded);
    DCE_CHECK_EQ(state->snapshot.size(), image_bytes.size());
    DCE_CHECK_EQ(state->records.size(), static_cast<std::size_t>(4));
    for (std::size_t index = 0; index < state->records.size(); ++index) {
      DCE_CHECK_EQ(payload_text(state->records[index].payload),
                   std::string("record-") + std::to_string(index + 5));
    }
  }
  DCE_CHECK_TRUE(dce::platform::remove_tree(directory).ok());
}

DCE_TEST(persistence, refuses_a_payload_above_the_configured_bound) {
  const std::string directory = temporary_root("bound");
  DCE_REQUIRE(dce::platform::make_directories(directory).ok());
  StoreOptions options;
  options.max_payload_bytes = 64;

  dce::Result<Store> store = Store::open(directory, options, OpenMode::read_write);
  DCE_REQUIRE_OK(store);
  const std::vector<std::byte> small(64, std::byte{1});
  const std::vector<std::byte> large(65, std::byte{1});
  DCE_REQUIRE_OK(store->append(RecordType::migration_receipt, small));
  dce::Result<dce::persist::CommitReceipt> refused =
      store->append(RecordType::migration_receipt, large);
  DCE_CHECK_TRUE(!refused.ok());
  DCE_CHECK_EQ(refused.code(), dce::ErrorCode::limit_exceeded);
  // The refused append must not have consumed a sequence.
  DCE_CHECK_EQ(store->committed_sequence().value(), static_cast<std::uint64_t>(1));
  DCE_REQUIRE(store->close().ok());
  DCE_CHECK_TRUE(dce::platform::remove_tree(directory).ok());
}

// This case doubles as the crash writer. When the parent has created the
// handshake directory relative to this process's working directory, it appends
// records, publishes a marker, and then blocks until it is killed. Otherwise it
// is a no-op, so an ordinary run of the suite passes straight through it.
DCE_TEST(persistence, crash_writer_child) {
  const std::string handshake = "dce-crash-child-store";
  if (!dce::platform::stat_path(handshake).ok()) {
    DCE_CHECK_TRUE(true);
    return;
  }
  StoreOptions options;
  dce::Result<Store> store = Store::open(handshake, options, OpenMode::read_write);
  DCE_REQUIRE_OK(store);
  for (std::uint64_t index = 0; index < 16; ++index) {
    dce::Result<dce::persist::CommitReceipt> receipt =
        store->append(RecordType::migration_receipt, payload_for(index));
    DCE_REQUIRE_OK(receipt);
    DCE_REQUIRE(receipt->durable);
  }
  {
    dce::Result<dce::platform::File> marker =
        dce::platform::File::open("dce-crash-child-ready", dce::platform::OpenMode::read_write);
    DCE_REQUIRE_OK(marker);
    const std::string ready = "ready";
    std::vector<std::byte> bytes(ready.size());
    std::transform(ready.begin(), ready.end(), bytes.begin(),
                   [](char c) { return static_cast<std::byte>(static_cast<unsigned char>(c)); });
    DCE_REQUIRE(marker->write_at(0, bytes).ok());
    DCE_REQUIRE(marker->sync().ok());
  }
  // The store is deliberately never closed: the parent kills this process, so
  // the on-disk state is exactly what a crash leaves behind. The wait is long
  // enough that the parent always wins the race and bounded so that an orphaned
  // child cannot linger.
  std::this_thread::sleep_for(std::chrono::seconds(60));
  {
    // Reaching this point means the parent's kill did not take effect. The
    // parent asserts the opposite, so a kill that silently fails is reported as
    // a failure instead of merely making the suite slow.
    dce::Result<dce::platform::File> finished =
        dce::platform::File::open("dce-crash-child-finished", dce::platform::OpenMode::read_write);
    if (finished.ok()) {
      const std::string note = "the crash writer ran to completion";
      std::vector<std::byte> bytes(note.size());
      std::transform(note.begin(), note.end(), bytes.begin(),
                     [](char c) { return static_cast<std::byte>(static_cast<unsigned char>(c)); });
      (void)finished->write_at(0, bytes);
      (void)finished->sync();
    }
  }
}

DCE_TEST(persistence, a_killed_writer_leaves_a_store_that_still_opens) {
  const std::string root = temporary_root("crash");
  DCE_REQUIRE(dce::platform::make_directories(root).ok());
  // The handshake is a directory whose presence tells the child it is the child.
  DCE_REQUIRE(dce::platform::make_directories(
                  dce::platform::join_path(root, "dce-crash-child-store"))
                  .ok());

  dce::platform::ProcessOptions options;
  options.program = dce::test::program_path();
  DCE_REQUIRE(!std::string(options.program).empty());
  options.arguments = {"--filter", "persistence.crash_writer_child"};
  options.working_directory = root;
  options.capture_stdout = true;
  dce::Result<dce::platform::ChildProcess> child = dce::platform::ChildProcess::spawn(options);
  DCE_REQUIRE_OK(child);

  const std::string marker = dce::platform::join_path(root, "dce-crash-child-ready");
  // The wait ends when the writer reports readiness or when it dies without
  // doing so. It is deliberately not bounded by elapsed time: a slow machine
  // must not turn a working crash writer into a failure, and a writer that
  // exits without reporting is a defect however fast the machine is.
  bool ready = false;
  bool exited = false;
  for (std::size_t attempt = 0; attempt < 200000 && !ready && !exited; ++attempt) {
    ready = dce::platform::stat_path(marker).ok();
    if (ready) {
      break;
    }
    dce::Result<bool> running = child->running();
    DCE_REQUIRE_OK(running);
    exited = !*running;
    if (!exited) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  DCE_CHECK_TRUE(ready);
  DCE_CHECK_TRUE(!exited);
  if (!ready) {
    DCE_REQUIRE(child->terminate().ok());
    DCE_CHECK_TRUE(dce::platform::remove_tree(root).ok());
    DCE_FAIL("the crash writer never reported readiness");
    return;
  }

  DCE_REQUIRE(child->terminate().ok());
  dce::Result<int> exit_code = child->wait();
  DCE_CHECK_TRUE(exit_code.ok());
  // The child only creates this file if it survives its own wait, so its
  // absence is proof that the hard kill actually reached the process.
  DCE_CHECK_TRUE(!dce::platform::stat_path(dce::platform::join_path(root, "dce-crash-child-finished")).ok());

  StoreOptions store_options;
  dce::Result<Store> store =
      Store::open(dce::platform::join_path(root, "dce-crash-child-store"), store_options,
                  OpenMode::read_write);
  DCE_REQUIRE_OK(store);
  dce::Result<dce::persist::RecoveredState> state = store->recover();
  DCE_REQUIRE_OK(state);
  DCE_CHECK_EQ(state->records.size(), static_cast<std::size_t>(16));
  for (std::size_t index = 0; index < state->records.size(); ++index) {
    DCE_CHECK_EQ(payload_text(state->records[index].payload),
                 std::string("record-") + std::to_string(index));
  }
  DCE_REQUIRE(store->close().ok());
  DCE_CHECK_TRUE(dce::platform::remove_tree(root).ok());
}
