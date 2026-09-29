// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// umrk standalone-ra-account-v1: the DSperate half. Consumes the snapshot
// Leaf's launcher hands an authorized DSperate launch, drives the native
// rcheevos login, and owns the durable token and marker. The classifier and
// the transition rules are in ra_account_contract.cpp and are shared with the
// Flycast consumer and the leaf-contracts reference.
//
// The durable state is two files that cannot be replaced together, so the
// order is conservative, exactly as in Flycast:
//
//   1. import(), before any credential changes: write the marker as pending
//      for the incoming revision and stop reusing the old token in memory.
//   2. the native password login completes.
//   3. commitLogin(): checked save of username + token, then the marker
//      becomes accepted.
//
// A crash anywhere in between leaves a pending marker, which is never proof of
// a reusable token: the next launch simply imports again. Nothing here writes a
// password or token to a log, to argv or to the marker.
//
// No DSperate headers: the host tests compile this file with the contract and
// replay interrupted writes against it. The achievement client pulls the
// bridge's notices into its own pop-up queue (cheevos_client.cpp).
#include "ra_account.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace ds::cheevos::ra_account {

namespace {

struct Bridge {
  bool captured = false;
  std::map<std::string, std::string> env;
  bool imported = false;
  bool managed = false;
  bool suppressed = false;
  // One pending password login, handed to rc_client exactly once.
  bool pendingLogin = false;
  // One token-rejection retry, answered exactly once. Never a loop.
  bool tokenRetryAvailable = false;
  bool tokenLoginAllowed = true;
  bool tokenReuse = false;
  std::string account;
  std::string password;
  long long revision = 0;
  std::string managed_dir;
  std::string status;
  std::vector<Notice> notices;
};

Bridge &bridge() {
  static Bridge instance;
  return instance;
}

// Overwrite before releasing: the password should not outlive its single use.
void wipe(std::string &secret) {
  if (!secret.empty()) std::memset(&secret[0], 0, secret.size());
  secret.clear();
  secret.shrink_to_fit();
}

void wipeEnv(std::map<std::string, std::string> &env) {
  for (auto &entry : env) wipe(entry.second);
  env.clear();
}

std::string markerPath(Bridge &b) { return b.managed_dir + "/" + MarkerFileName; }
std::string tokenPath(const std::string &dir) { return dir + "/" + TokenFileName; }

void setStatus(Bridge &b, const std::string &status) { b.status = status; }

// Every player-facing account problem goes two ways: the log, and the native
// pop-up queue the achievement client drains (takeNotices), which is where
// upstream's own sign-in failure appears. The account page's status line reads
// statusLine() as well.
void report(Bridge &b, const std::string &text, const std::string &detail = std::string()) {
  std::fprintf(stderr, "cheevos: %s%s%s\n", text.c_str(), detail.empty() ? "" : ": ",
               detail.c_str());
  b.notices.push_back(Notice{text, detail});
}

bool writeMarkerFile(Bridge &b, Transition transition, long long revision,
                     const std::string &account) {
  Marker marker;
  marker.transition = transition;
  marker.revision = revision;
  marker.account = account;
  return writeMarker(markerPath(b), marker);
}

// write(2) until everything is out: a short write is not an error by itself,
// but a full card eventually answers -1/ENOSPC, which is.
bool writeAll(int fd, const std::string &data) {
  size_t done = 0;
  while (done < data.size()) {
    const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) {
      errno = EIO;
      return false;
    }
    done += static_cast<size_t>(n);
  }
  return true;
}

std::string errorText(const char *step, int err) {
  return std::string(step) + ": " + std::strerror(err);
}

} // namespace

// ---------------------------------------------------------------------------
// Token file

TokenStatus readTokenFile(const std::string &dir, std::string &username, std::string &token) {
  username.clear();
  token.clear();
  FILE *f = std::fopen(tokenPath(dir).c_str(), "rb");
  if (f == nullptr) return errno == ENOENT ? TokenStatus::Absent : TokenStatus::Malformed;
  std::string content;
  char buffer[512];
  size_t n;
  bool tooLarge = false;
  while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) {
    content.append(buffer, n);
    if (content.size() > 4096) {
      tooLarge = true;
      break;
    }
  }
  const bool failed = std::ferror(f) != 0;
  std::fclose(f);
  if (failed || tooLarge) {
    wipe(content);
    return TokenStatus::Malformed;
  }
  // DSperate's own format (cheevos_client.cpp load_credentials): the username
  // on the first line, the token on the second.
  std::string fields[2];
  size_t offset = 0;
  for (int i = 0; i < 2 && offset < content.size(); ++i) {
    const size_t end = content.find('\n', offset);
    fields[i] = content.substr(offset, end == std::string::npos ? std::string::npos : end - offset);
    offset = end == std::string::npos ? content.size() : end + 1;
    while (!fields[i].empty() && fields[i].back() == '\r') fields[i].pop_back();
  }
  wipe(content);
  if (fields[0].empty() || fields[1].empty()) {
    wipe(fields[1]);
    return TokenStatus::Malformed;
  }
  username = fields[0];
  token = fields[1];
  wipe(fields[1]);
  return TokenStatus::Ok;
}

bool writeTokenFile(const std::string &dir, const std::string &username,
                    const std::string &token, std::string &error) {
  error.clear();
  if (username.empty() || token.empty()) {
    error = "nothing to save";
    return false;
  }
  if (username.find_first_of("\r\n") != std::string::npos ||
      token.find_first_of("\r\n") != std::string::npos) {
    error = "credentials contain a line break";
    return false;
  }
  // Same directory, so the rename cannot cross a filesystem, and created 0600
  // from the start. Every step is checked; a failure anywhere removes the
  // temporary and leaves the previous cheevos.token exactly as it was.
  const std::string path = tokenPath(dir);
  const std::string tmp = path + ".tmp";
  ::unlink(tmp.c_str());
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    error = errorText("open", errno);
    return false;
  }
  std::string body = username + "\n" + token + "\n";
  const char *failed = nullptr;
  int err = 0;
  if (!writeAll(fd, body)) {
    failed = "write";
    err = errno;
  } else if (::fsync(fd) != 0) {
    failed = "fsync";
    err = errno;
  }
  wipe(body);
  if (::close(fd) != 0 && failed == nullptr) {
    failed = "close";
    err = errno;
  }
  if (failed == nullptr && std::rename(tmp.c_str(), path.c_str()) != 0) {
    failed = "rename";
    err = errno;
  }
  if (failed != nullptr) {
    ::unlink(tmp.c_str());
    error = errorText(failed, err);
    return false;
  }
  return true;
}

bool removeTokenFile(const std::string &dir, std::string &error) {
  error.clear();
  if (::unlink(tokenPath(dir).c_str()) == 0 || errno == ENOENT) return true;
  error = errorText("unlink", errno);
  return false;
}

// ---------------------------------------------------------------------------
// Handoff

void captureEnv() {
  Bridge &b = bridge();
  if (b.captured) return;
  b.captured = true;
  // Copy and scrub first, whatever the verdict turns out to be: nothing this
  // process spawns afterwards can read the password out of its environment.
  b.env = collectEnv();
  clearEnv();
}

void setManagedDir(const std::string &dir) { bridge().managed_dir = dir; }
const std::string &managedDir() { return bridge().managed_dir; }

void import(bool achievementsOff) {
  Bridge &b = bridge();
  if (b.imported) return;
  b.imported = true;
  captureEnv();

  Snapshot snapshot = classify(b.env);
  wipeEnv(b.env);
  if (snapshot.hasReason("stale-retroarch-credentials"))
    std::fprintf(stderr, "cheevos: RetroArch credential variables leaked into this launch\n");

  if (achievementsOff) {
    // An explicit "achievements off" wins over the managed account. The
    // snapshot was captured and scrubbed above and is forgotten here; no login
    // runs, and neither the marker nor the token is read or written, so the
    // next launch without the switch picks up exactly where this one found it.
    wipe(snapshot.password);
    if (snapshot.handoff != Handoff::None)
      std::fprintf(stderr, "cheevos: achievements are off for this game; the Leaf account is not used\n");
    return;
  }

  Marker marker;
  MarkerStatus markerStatus = MarkerStatus::Absent;
  std::string persistedUser;
  std::string persistedToken;
  TokenStatus tokenStatus = TokenStatus::Absent;
  if (!b.managed_dir.empty()) {
    markerStatus = readMarker(markerPath(b), marker);
    tokenStatus = readTokenFile(b.managed_dir, persistedUser, persistedToken);
    wipe(persistedToken);
    if (tokenStatus == TokenStatus::Malformed)
      std::fprintf(stderr, "cheevos: the managed token file is unreadable or incomplete\n");
  }
  const Decision decision =
      decide(snapshot, markerStatus, marker, persistedUser, tokenStatus == TokenStatus::Ok);

  switch (decision.action) {
  case Action::Unmanaged:
    return;

  case Action::Idle:
    if (snapshot.handoff == Handoff::Valid && snapshot.state == State::SignedOut) {
      // The sign-out is already durable, but still owns this session: do not
      // let external, per-game or CFW tokens revive an older native account.
      b.managed = true;
      b.tokenLoginAllowed = false;
      setStatus(b, "signed-out");
    }
    if (snapshot.handoff == Handoff::Malformed) {
      setStatus(b, decision.reason);
      report(b, "The Leaf account handoff was not usable", decision.reason);
    }
    return;

  case Action::SuppressManaged:
    // Managed state exists but this launch cannot justify using it. Nothing
    // durable is erased: the account is repaired in Leaf, not here.
    b.managed = true;
    b.suppressed = true;
    b.tokenLoginAllowed = false;
    setStatus(b, decision.reason);
    report(b, "The Leaf account is unavailable this session", decision.reason);
    return;

  case Action::ReuseToken:
    b.managed = true;
    b.account = decision.account;
    b.revision = decision.revision;
    b.tokenReuse = true;
    // Keep the password for the single retry the contract allows if the stored
    // token turns out to be rejected or expired.
    b.password = snapshot.password;
    wipe(snapshot.password);
    b.tokenRetryAvailable = true;
    setStatus(b, "accepted");
    return;

  case Action::SignOut: {
    b.managed = true;
    b.tokenLoginAllowed = false;
    wipe(snapshot.password);
    if (b.managed_dir.empty()) {
      b.suppressed = true;
      setStatus(b, "sign-out-marker-failed");
      report(b, "Could not record the signed-out account", "no managed account directory");
      return;
    }
    std::string err;
    const bool removed = removeTokenFile(b.managed_dir, err);
    // Record the sign-out even when the token could not be removed: a
    // signed-out marker never justifies reusing it.
    if (!writeMarkerFile(b, Transition::SignedOut, decision.revision, std::string())) {
      b.suppressed = true;
      setStatus(b, "sign-out-marker-failed");
      report(b, "Could not record the signed-out account", "the marker was not written");
      return;
    }
    if (!removed) {
      b.suppressed = true;
      setStatus(b, "sign-out-token-failed");
      report(b, "Could not remove the signed-out account's token", err);
      return;
    }
    setStatus(b, "signed-out");
    return;
  }

  case Action::ImportLogin:
    b.managed = true;
    b.account = decision.account;
    b.revision = decision.revision;
    b.tokenLoginAllowed = false;

    if (b.managed_dir.empty() ||
        !writeMarkerFile(b, Transition::Pending, decision.revision, decision.account)) {
      // The old files stay exactly as they were, and this session runs without
      // managed achievements.
      wipe(snapshot.password);
      b.suppressed = true;
      setStatus(b, "pending-marker-failed");
      report(b, "Could not prepare the Leaf account import",
             b.managed_dir.empty() ? "no managed account directory" : "the marker was not written");
      return;
    }

    b.password = snapshot.password;
    wipe(snapshot.password);
    b.pendingLogin = true;
    setStatus(b, "pending");
    return;
  }
}

bool achievementsOn(bool settingPresent, bool settingOn) {
  if (settingPresent) return settingOn;
  Bridge &b = bridge();
  return b.managed || b.suppressed;
}

bool takePendingLogin(std::string &username, std::string &password) {
  Bridge &b = bridge();
  if (!b.pendingLogin) return false;
  b.pendingLogin = false;
  username = b.account;
  password = b.password;
  // The import login and the token retry are mutually exclusive: this is the
  // first and only password attempt of the two.
  b.tokenRetryAvailable = false;
  wipe(b.password);
  return true;
}

bool takeTokenRetry(std::string &username, std::string &password) {
  Bridge &b = bridge();
  if (!b.tokenRetryAvailable) return false;
  b.tokenRetryAvailable = false;
  username = b.account;
  password = b.password;
  wipe(b.password);
  return true;
}

bool isTokenLoginAllowed() {
  Bridge &b = bridge();
  return b.tokenLoginAllowed;
}

bool loadManagedToken(std::string &username, std::string &token) {
  Bridge &b = bridge();
  username.clear();
  token.clear();
  if (!b.managed || !b.tokenLoginAllowed || !b.tokenReuse) return false;
  if (readTokenFile(b.managed_dir, username, token) != TokenStatus::Ok ||
      username != b.account) {
    // The file changed under us since import(); never authenticate as anyone
    // but the account the marker accepted.
    username.clear();
    wipe(token);
    return false;
  }
  return true;
}

bool commitLogin(const std::string &username, const std::string &token) {
  Bridge &b = bridge();
  // An unmanaged native sign-in lasts for the session, as upstream DSperate's
  // does: upstream never writes one, and this adapter does not either. In
  // particular nothing is scattered into the per-game config directories the
  // Leaf wrapper gives each game.
  if (!b.managed) return true;
  (void)username;

  if (b.managed_dir.empty() || b.account.empty() || token.empty()) return false;
  std::string err;
  if (!writeTokenFile(b.managed_dir, b.account, token, err)) {
    // Authenticated, but not durably: the marker stays pending, so the next
    // launch imports again instead of trusting a token that is not on disk.
    b.suppressed = true;
    setStatus(b, "token-save-failed");
    report(b, "The Leaf account was verified but could not be saved", err);
    return false;
  }
  if (!writeMarkerFile(b, Transition::Accepted, b.revision, b.account)) {
    // Authenticated and persisted, but the revision is not accepted: the next
    // launch imports again rather than trusting an unrecorded state.
    setStatus(b, "accept-marker-failed");
    report(b, "The Leaf account will be imported again on the next launch",
           "the marker was not written");
    return true;
  }
  setStatus(b, "accepted");
  wipe(b.password);
  return true;
}

void reportLoginFailure(const std::string &reason, const std::string &detail) {
  Bridge &b = bridge();
  if (!b.managed) return;
  b.suppressed = true;
  // Never fall back to the previous account: the marker stays pending and the
  // in-memory token is already gone. Gameplay continues without achievements.
  b.tokenRetryAvailable = false;
  wipe(b.password);
  setStatus(b, reason);
  // Account notices bypass the optional achievement-toast switch.
  report(b, "RetroAchievements sign-in failed", detail);
}

std::vector<Notice> takeNotices() {
  std::vector<Notice> out;
  out.swap(bridge().notices);
  return out;
}

bool isManaged() { return bridge().managed; }
bool isSuppressed() { return bridge().suppressed; }
std::string statusLine() { return bridge().status; }
std::string managedAccount() { return bridge().account; }

#ifdef DS_RA_ACCOUNT_TESTING
void resetForTesting() {
  Bridge &b = bridge();
  wipeEnv(b.env);
  wipe(b.password);
  b = Bridge{};
}
#endif

} // namespace ds::cheevos::ra_account
