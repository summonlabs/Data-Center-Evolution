# Contributing to Data Center Evolution

Data Center Evolution is the DCCP boundary that governs live evolution of a
multi-site data-center control plane. It is maintained by Summon Software Labs
and distributed under the Apache License 2.0; the full text is in
[LICENSE](LICENSE).

## Licensing of contributions

Contributions are accepted under the Apache License 2.0. By submitting a
contribution to this project, in any form and through any channel (a pull
request, a patch sent to a maintainer, or any other submission intended for
inclusion in the project), you license that contribution to Summon Software
Labs and to every downstream recipient under the terms of the Apache License
2.0, and you confirm that you hold the rights needed to grant that license. No
additional terms or conditions are attached to your submission.

There is no Contributor License Agreement (CLA) to sign, and there is no
copyright assignment requirement. You keep the copyright in your contribution;
you simply license it under Apache-2.0, exactly as the project itself is
licensed. This is the default described in section 5 of the license.

Do not add `Co-authored-by` trailers, generated-by notices, or attribution lines
that you cannot justify. Commit authorship is recorded by Git itself.

## Before you open a pull request

1. Build both configurations with warnings-as-errors:

   ```
   cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build/release
   cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
   cmake --build build/debug
   ```

2. Run the complete test suite in both configurations:

   ```
   ctest --test-dir build/release --output-on-failure
   ctest --test-dir build/debug --output-on-failure
   ```

   Tests are run plainly. Do not add CTest timeouts, shell `timeout` wrappers,
   watchdog-success logic, watchdog process-kill-as-pass behaviour, or CI
   `timeout-minutes`. A test that hangs is a defect to diagnose, not something
   to abort and call passing.

3. Keep the public API strongly typed. Identities, plan generations, epochs,
   site generations, ordinals and external boundary references are distinct
   types with no implicit conversions between them, and no sentinel values
   where an optional or a sum type says the same thing more precisely.

4. Any change to the on-disk format requires a format version bump, a migration
   note in `docs/PERSISTENCE.md`, and an explicit compatibility statement.
   Recovery must never truncate through interior corruption.

5. New behaviour needs evidence. Tests assert observable behaviour, not
   implementation details, and every invariant needs a test that fails when the
   invariant is violated.

## Code quality expectations

* C++20, standard library only for the shipped library. A new third-party
  dependency must be justified in the pull request and is normally rejected
  when the standard library suffices.
* Zero first-party warnings under `/W4 /WX` (MSVC) or
  `-Wall -Wextra -Wpedantic -Werror` (GCC/Clang). Warnings are fixed at the
  cause; blanket suppression is not acceptable.
* No TODO placeholders, dead code, debug prints, machine-specific absolute
  paths, or generated junk in the tree.
* Deterministic behaviour must be reproducible: seeded randomized tests, stable
  iteration order, and canonical serialization for anything persisted or
  hashed.
* Never fabricate authority, capacity, health, compatibility or success.
  Unknown, stale, conflicting, unsupported, invalid and indeterminate are
  distinct outcomes and must stay distinct.
* The boundary owns evolution governance only. Do not reach into the internals
  of an adjacent runtime; consume its explicit contract and record the
  provenance of what was consumed.

## Testing expectations

New behaviour needs tests of the kind that would actually catch its failure
modes. Depending on the change that means unit, integration, end-to-end,
property/seeded-randomized, malformed-input, failure-injection, persistence and
recovery, real independent OS-process, concurrency stress, generation-fencing,
package-consumer or CLI smoke coverage. Multi-process suites spawn real
processes and clean them up themselves.

## Reporting defects

Open an issue with the exact command, the observed result, and the expected
result. Durability, integrity, generation-fencing, gate-skipping and ordering
defects are treated as release blockers; include a minimised reproduction and
the store directory when the defect is in recovery.
