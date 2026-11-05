// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// UMRK: standalone-ra-account-v1 consumer for Leaf / Miniloong Pocket 1.
//
// The Leaf launcher (Jawaka) owns the RetroAchievements account. It resolves
// one coherent snapshot of it per launch and exports the five child-only
// UMRK_RA_ACCOUNT_* variables to an authorized emulator after fork(). This
// header is the consumer half of that contract: the classifier, the durable
// marker in the managed account directory, and the transition the two of them
// select.
//
// Everything declared here is deliberately free of DSperate dependencies: the
// host tests compile ra_account_contract.cpp and ra_account_bridge.cpp on their
// own, replay the shared leaf-contracts fixtures byte for byte, and drive the
// bridge through interrupted writes. The DSperate-facing glue (rc_client login,
// the achievement client's pop-ups, the menu) calls in from cheevos_client.cpp
// and the SDL frontend.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace ds::cheevos::ra_account {

// Byte limits, excluding NUL. The producer validates before storing; a
// consumer re-validates because a truncated legacy value must never be
// authenticated.
constexpr size_t UsernameMaxBytes = 63;
constexpr size_t PasswordMaxBytes = 127;
// Inclusive ceiling, 2^62. A producer fails the save rather than wrapping, so
// this is the largest revision a consumer ever sees.
constexpr long long RevisionMax = 4611686018427387904LL;

constexpr const char *ContractId = "standalone-ra-account-v1";
constexpr const char *MarkerFileName = ".umrk-ra-account";
constexpr const char *TokenFileName = "cheevos.token";

enum class State {
  Configured,
  NeverConfigured,
  SignedOut,
  Invalid,
  Unreadable,
};

enum class Handoff {
  // No UMRK_RA_ACCOUNT_* variable is set at all: this launch is not described
  // by the contract and native behavior is untouched.
  None,
  Valid,
  // A partial, unsupported or self-contradictory snapshot. Never guessed at.
  Malformed,
};

// One classified handoff. `username`/`password` are only populated for a valid
// configured snapshot; `reasons` carries the contract's own rejection
// identifiers and is always secret-free.
struct Snapshot {
  Handoff handoff = Handoff::None;
  State state = State::Unreadable;
  std::string username;
  std::string password;
  long long revision = 0;
  std::vector<std::string> reasons;

  bool hasReason(const std::string &reason) const;
  // First rejection reason, or "" for a clean snapshot.
  std::string primaryReason() const;
};

// Classify a raw environment. Values arrive as std::string so a fixture can
// express bytes getenv() could never return (an embedded NUL truncates a C
// string); the runtime collects the same map from getenv().
Snapshot classify(const std::map<std::string, std::string> &env);

// Collect the contract variables, plus the JAWAKA_CHEEVOS_* pair whose
// presence means the producer failed to scrub RetroArch's own handoff, from the
// process environment.
std::map<std::string, std::string> collectEnv();

// Remove every variable classify() looks at from the process environment, so
// no helper this process spawns can read the password. Called immediately
// after collectEnv(), before anything else runs.
void clearEnv();

// ---------------------------------------------------------------------------
// Durable marker
// ---------------------------------------------------------------------------

// The marker records which managed revision this emulator has actually
// committed. It never contains a password or a token: the token lives in the
// managed account directory's cheevos.token.
//
// Two files cannot be replaced atomically, so the order is conservative:
// pending is written BEFORE the credentials change, accepted only AFTER
// authentication and a checked token save. A pending or malformed marker is
// never proof of a reusable token.
enum class Transition {
  Pending,
  Accepted,
  SignedOut,
};

struct Marker {
  Transition transition = Transition::Pending;
  // The revision this transition refers to. 0 only for a sign-out that
  // followed a never-configured handoff, which carries no revision.
  long long revision = 0;
  std::string account;
};

enum class MarkerStatus {
  Absent,
  Malformed,
  Ok,
};

MarkerStatus readMarker(const std::string &path, Marker &out);
// Same-directory temporary file, checked write/flush/fsync/close, then rename.
// Returns false with the previous file intact.
bool writeMarker(const std::string &path, const Marker &marker);

// ---------------------------------------------------------------------------
// Transition
// ---------------------------------------------------------------------------

enum class Action {
  // No handoff and no managed state: this emulator's own account, if any,
  // behaves exactly as upstream DSperate.
  Unmanaged,
  // Marker, snapshot and persisted account agree: reuse the stored token.
  ReuseToken,
  // Authenticate the snapshot's credentials and, only on success, persist the
  // token and accept the revision.
  ImportLogin,
  // Clear the managed username/token and record the sign-out revision.
  SignOut,
  // Managed state exists but the current handoff cannot justify using it: no
  // managed authentication this session, and nothing durable is erased.
  SuppressManaged,
  // Nothing managed is active and nothing managed needs protecting, but the
  // launch is worth reporting (a malformed handoff, or a sign-out this
  // emulator already committed). Native behavior is untouched, exactly as for
  // Unmanaged.
  Idle,
};

struct Decision {
  Action action = Action::Unmanaged;
  long long revision = 0;
  std::string account;
  // Secret-free identifier for logs and the settings status line.
  std::string reason;
};

// Select the transition from the classified snapshot, the marker on disk and
// what DSperate itself has persisted. Pure: the caller performs the writes.
Decision decide(const Snapshot &snapshot, MarkerStatus markerStatus,
                const Marker &marker, const std::string &persistedAccount,
                bool persistedTokenPresent);

// ---------------------------------------------------------------------------
// Bridge (ra_account_bridge.cpp)
// ---------------------------------------------------------------------------

// Copy the handoff into private memory and scrub it from the process
// environment. Called as the first thing the frontend does, before any thread,
// helper or log line exists; import() calls it too, so a caller that forgets
// still never leaves the password in the environment. Idempotent.
void captureEnv();

// The non-secret managed account directory the wrapper resolved and passed
// (--managed-account-dir). Empty means the launch is not managed; the
// classifier then sees only what the environment carries.
void setManagedDir(const std::string &dir);
const std::string &managedDir();

// Consume the handoff: classify the captured snapshot, pick the transition and
// perform its durable part (write pending, import the credentials, or clear for
// a sign-out). Called once before the achievement client starts. Never blocks
// on the network.
//
// `achievementsOff` is an explicit per-game or per-launch "achievements off"
// (cheevos.enabled = false in a config file, or --no-cheevos). It wins over a
// managed account: the snapshot is still captured, scrubbed and forgotten, but
// nothing authenticates and nothing on disk -- marker or token -- is touched.
void import(bool achievementsOff);

// Whether the achievement client runs at all. An explicit setting wins either
// way. Without one, a managed account turns achievements on -- configured,
// suppressed or signed out, so the account page can say why -- and an
// unmanaged launch keeps upstream's default of off. Call after import().
bool achievementsOn(bool settingPresent, bool settingOn);

// One-shot: hand the imported credentials to the native password login. The
// bridge forgets the password as it answers.
bool takePendingLogin(std::string &username, std::string &password);
// One-shot: the contract's single native password retry after a stored token is
// rejected or expires. Answers once per session, so it can never loop.
bool takeTokenRetry(std::string &username, std::string &password);
// False when a stored token must not be used this session (a suppressed
// handoff, a sign-out, or an import whose login has not succeeded yet).
bool isTokenLoginAllowed();
// The managed account's stored token, when marker, snapshot and token file
// agreed at import(). False, with nothing filled in, otherwise.
bool loadManagedToken(std::string &username, std::string &token);
// Persist a verified managed token and only then accept its revision: a checked
// token write (same-directory temporary, write, flush, fsync, close, rename),
// then the accepted marker. Returns false when the token could not be saved;
// the marker then stays pending, so the next launch imports again, and the
// failure is reported. An unmanaged sign-in is not persisted: upstream keeps it
// for the session only, and so does this adapter.
bool commitLogin(const std::string &username, const std::string &token);
// Report a failed managed authentication: gameplay continues without
// achievements, and the previous account is never reused.
void reportLoginFailure(const std::string &reason, const std::string &detail);

// A player-facing problem the bridge could not solve by itself (a failed write,
// an unusable handoff). The achievement client drains these into its own
// pop-up queue, the same one the native sign-in failure uses, and the frontend
// shows them even with ordinary achievement toasts turned off. Never carries a
// password or a token.
struct Notice {
  std::string text;
  std::string detail;
};
std::vector<Notice> takeNotices();

// True when this session is running a managed account.
bool isManaged();
// True when a managed account is configured but must not authenticate this
// session (suppressed handoff, or a failed marker/token write).
bool isSuppressed();
// Secret-free one-line status for the native settings screen; "" when there is
// nothing managed to report.
std::string statusLine();
// Managed username, or "" when unmanaged.
std::string managedAccount();

// The managed token file, cheevos.token in DSperate's own username/newline/
// token format. Exposed for the host tests; the bridge is the only caller.
enum class TokenStatus {
  Absent,
  Malformed,
  Ok,
};
TokenStatus readTokenFile(const std::string &dir, std::string &username, std::string &token);
bool writeTokenFile(const std::string &dir, const std::string &username,
                    const std::string &token, std::string &error);
// Absent already counts as removed.
bool removeTokenFile(const std::string &dir, std::string &error);

#ifdef DS_RA_ACCOUNT_TESTING
// Host tests only: forget everything the bridge holds, as a new process would.
void resetForTesting();
#endif

} // namespace ds::cheevos::ra_account
