// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// UMRK: standalone-ra-account-v1 classifier, marker and transition rules.
// Shared verbatim with the Flycast standalone consumer and the reference
// classifier in leaf-contracts, so the three cannot drift.
//
// No DSperate headers on purpose: the host test compiles this file on its own
// and replays the shared leaf-contracts fixtures against classify().
#include "ra_account.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ds::cheevos::ra_account {

namespace {

const char *const kVersionVar  = "UMRK_RA_ACCOUNT_VERSION";
const char *const kStateVar    = "UMRK_RA_ACCOUNT_STATE";
const char *const kUsernameVar = "UMRK_RA_ACCOUNT_USERNAME";
const char *const kPasswordVar = "UMRK_RA_ACCOUNT_PASSWORD";
const char *const kRevisionVar = "UMRK_RA_ACCOUNT_REVISION";
// RetroArch's own per-launch credential handoff. It has no business in a
// standalone child; seeing it means an inherited value survived fork().
const char *const kRetroArchUserVar = "JAWAKA_CHEEVOS_USERNAME";
const char *const kRetroArchPassVar = "JAWAKA_CHEEVOS_PASSWORD";

const char *const kEnvVars[] = {
	kVersionVar, kStateVar, kUsernameVar, kPasswordVar, kRevisionVar,
	kRetroArchUserVar, kRetroArchPassVar,
};

const char *const kContractPrefix = "UMRK_RA_ACCOUNT_";

bool isValidUtf8(const std::string& value)
{
	const unsigned char *p = (const unsigned char *)value.data();
	size_t remaining = value.size();
	while (remaining > 0)
	{
		unsigned char lead = *p;
		size_t length;
		unsigned int codepoint;
		if (lead < 0x80) {
			length = 1;
			codepoint = lead;
		}
		else if ((lead & 0xe0) == 0xc0) {
			length = 2;
			codepoint = lead & 0x1f;
		}
		else if ((lead & 0xf0) == 0xe0) {
			length = 3;
			codepoint = lead & 0x0f;
		}
		else if ((lead & 0xf8) == 0xf0) {
			length = 4;
			codepoint = lead & 0x07;
		}
		else
			return false;
		const unsigned char *q = p + 1;
		if (length > remaining)
			return false;
		for (size_t i = 1; i < length; i++)
		{
			if ((p[i] & 0xc0) != 0x80)
				return false;
			codepoint = (codepoint << 6) | (p[i] & 0x3f);
		}
		(void)q;
		// Reject overlong encodings, UTF-16 surrogates and anything past the
		// Unicode range, exactly as a strict decoder does.
		static const unsigned int minCodepoint[5] = { 0, 0, 0x80, 0x800, 0x10000 };
		if (codepoint < minCodepoint[length])
			return false;
		if (codepoint > 0x10ffff)
			return false;
		if (codepoint >= 0xd800 && codepoint <= 0xdfff)
			return false;
		p += length;
		remaining -= length;
	}
	return true;
}

bool stateFromName(const std::string& name, State& out)
{
	if (name == "configured") { out = State::Configured; return true; }
	if (name == "never-configured") { out = State::NeverConfigured; return true; }
	if (name == "signed-out") { out = State::SignedOut; return true; }
	if (name == "invalid") { out = State::Invalid; return true; }
	if (name == "unreadable") { out = State::Unreadable; return true; }
	return false;
}

const char *transitionName(Transition transition)
{
	switch (transition)
	{
	case Transition::Pending:   return "pending";
	case Transition::Accepted:  return "accepted";
	case Transition::SignedOut: return "signed-out";
	}
	return "pending";
}

bool transitionFromName(const std::string& name, Transition& out)
{
	if (name == "pending") { out = Transition::Pending; return true; }
	if (name == "accepted") { out = Transition::Accepted; return true; }
	if (name == "signed-out") { out = Transition::SignedOut; return true; }
	return false;
}

// Strict ASCII decimal parse. `overflow` separates the contract's two rejection
// rules: a non-positive or non-numeric revision is malformed, one past the
// ceiling is the overflow the producer must refuse to write. Neither is ever
// clamped into a usable value.
bool parseRevision(const std::string& text, long long& out, bool& overflow)
{
	overflow = false;
	out = 0;
	if (text.empty())
		return false;
	long long value = 0;
	for (char c : text)
	{
		if (c < '0' || c > '9')
			return false;
		if (value > (RevisionMax / 10) + 1) {
			overflow = true;
			return false;
		}
		value = value * 10 + (c - '0');
		if (value > RevisionMax) {
			overflow = true;
			return false;
		}
	}
	if (value < 1)
		return false;
	out = value;
	return true;
}

bool lookup(const std::map<std::string, std::string>& env, const char *name,
		std::string& out)
{
	auto it = env.find(name);
	if (it == env.end())
		return false;
	out = it->second;
	return true;
}

void checkCredential(const std::string& value, const char *what, size_t limit,
		std::vector<std::string>& reasons)
{
	if (value.empty())
		reasons.push_back("credential-empty");
	if (value.size() > limit)
		reasons.push_back(std::string(what) + "-oversized");
	if (value.find('\0') != std::string::npos ||
		value.find('\r') != std::string::npos ||
		value.find('\n') != std::string::npos)
		reasons.push_back("credential-control-character");
	if (!isValidUtf8(value))
		reasons.push_back("credential-invalid-utf8");
}

} // namespace

bool Snapshot::hasReason(const std::string& reason) const
{
	for (const std::string& candidate : reasons)
		if (candidate == reason)
			return true;
	return false;
}

std::string Snapshot::primaryReason() const
{
	return reasons.empty() ? std::string() : reasons.front();
}

Snapshot classify(const std::map<std::string, std::string>& env)
{
	Snapshot snapshot;

	if (env.count(kRetroArchUserVar) != 0 || env.count(kRetroArchPassVar) != 0)
		snapshot.reasons.push_back("stale-retroarch-credentials");

	bool anyContractVar = false;
	const size_t prefixLength = strlen(kContractPrefix);
	for (const auto& [name, value] : env)
	{
		(void)value;
		if (name.compare(0, prefixLength, kContractPrefix) == 0) {
			anyContractVar = true;
			break;
		}
	}
	if (!anyContractVar) {
		// Total absence is "no supported handoff". A partial snapshot is not.
		snapshot.handoff = Handoff::None;
		return snapshot;
	}
	snapshot.handoff = Handoff::Malformed;

	std::string version;
	if (!lookup(env, kVersionVar, version) || version != "1") {
		snapshot.reasons.push_back("unsupported-version");
		return snapshot;
	}

	std::string stateName;
	if (!lookup(env, kStateVar, stateName)) {
		snapshot.reasons.push_back("missing-account-state");
		return snapshot;
	}
	State state;
	if (!stateFromName(stateName, state)) {
		snapshot.reasons.push_back("unknown-account-state");
		return snapshot;
	}
	snapshot.state = state;

	std::string username;
	std::string password;
	const bool hasUsername = lookup(env, kUsernameVar, username);
	const bool hasPassword = lookup(env, kPasswordVar, password);

	if (state != State::Configured && (hasUsername || hasPassword))
		snapshot.reasons.push_back("credentials-unexpected");

	if (state == State::Configured)
	{
		if (!hasUsername)
			snapshot.reasons.push_back("username-missing");
		if (!hasPassword)
			snapshot.reasons.push_back("password-missing");
		if (hasUsername)
			checkCredential(username, "username", UsernameMaxBytes, snapshot.reasons);
		if (hasPassword)
			checkCredential(password, "password", PasswordMaxBytes, snapshot.reasons);
	}

	std::string revisionText;
	const bool hasRevision = lookup(env, kRevisionVar, revisionText);
	if (state == State::Configured || state == State::SignedOut)
	{
		if (!hasRevision)
			snapshot.reasons.push_back("revision-missing");
		else
		{
			long long revision = 0;
			bool overflow = false;
			if (parseRevision(revisionText, revision, overflow))
				snapshot.revision = revision;
			else
				snapshot.reasons.push_back(overflow ? "revision-overflow"
													: "revision-invalid");
		}
	}
	else if (hasRevision)
		snapshot.reasons.push_back("revision-unexpected");

	if (snapshot.reasons.empty())
	{
		snapshot.handoff = Handoff::Valid;
		if (state == State::Configured) {
			snapshot.username = username;
			snapshot.password = password;
		}
	}
	else
	{
		// A rejected snapshot carries no credentials out of this function.
		snapshot.revision = 0;
	}
	return snapshot;
}

std::map<std::string, std::string> collectEnv()
{
	std::map<std::string, std::string> env;
	for (const char *name : kEnvVars)
	{
		const char *value = getenv(name);
		if (value != nullptr)
			env[name] = value;
	}
	return env;
}

void clearEnv()
{
	for (const char *name : kEnvVars)
		unsetenv(name);
}

MarkerStatus readMarker(const std::string& path, Marker& out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return errno == ENOENT ? MarkerStatus::Absent : MarkerStatus::Malformed;
	std::string content;
	char buffer[512];
	size_t read;
	bool tooLarge = false;
	while ((read = fread(buffer, 1, sizeof(buffer), f)) > 0)
	{
		content.append(buffer, read);
		if (content.size() > 4096) {
			tooLarge = true;
			break;
		}
	}
	const bool failed = ferror(f) != 0;
	fclose(f);
	if (failed || tooLarge)
		// An unreadable or absurd marker is managed state we cannot trust; it
		// is malformed, never absent, so it can never look unmanaged.
		return MarkerStatus::Malformed;

	Marker marker;
	bool haveHeader = false;
	bool haveState = false;
	bool haveRevision = false;
	bool haveAccount = false;
	size_t offset = 0;
	while (offset <= content.size())
	{
		size_t end = content.find('\n', offset);
		const bool last = end == std::string::npos;
		std::string line = content.substr(offset, last ? std::string::npos : end - offset);
		offset = last ? content.size() + 1 : end + 1;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty())
			continue;
		if (!haveHeader) {
			if (line != std::string("umrk-ra-account 1"))
				return MarkerStatus::Malformed;
			haveHeader = true;
			continue;
		}
		const size_t space = line.find(' ');
		const std::string key = line.substr(0, space);
		// The value runs to end of line: an account name may contain spaces.
		const std::string value =
			space == std::string::npos ? std::string() : line.substr(space + 1);
		if (key == "state") {
			if (haveState || !transitionFromName(value, marker.transition))
				return MarkerStatus::Malformed;
			haveState = true;
		}
		else if (key == "revision") {
			if (haveRevision)
				return MarkerStatus::Malformed;
			if (value == "0")
				marker.revision = 0;
			else
			{
				bool overflow = false;
				if (!parseRevision(value, marker.revision, overflow))
					return MarkerStatus::Malformed;
			}
			haveRevision = true;
		}
		else if (key == "account") {
			if (haveAccount)
				return MarkerStatus::Malformed;
			marker.account = value;
			haveAccount = true;
		}
		else
			// An unknown key means a marker written by something this build
			// does not understand. Refuse it rather than acting on a subset.
			return MarkerStatus::Malformed;
	}
	if (!haveHeader || !haveState || !haveRevision || !haveAccount)
		return MarkerStatus::Malformed;
	if (marker.revision == 0 && marker.transition != Transition::SignedOut)
		// Only a sign-out that followed a never-configured handoff has no
		// revision; anything else without one cannot justify a token.
		return MarkerStatus::Malformed;
	if (marker.account.find('\r') != std::string::npos ||
		marker.account.find('\n') != std::string::npos)
		return MarkerStatus::Malformed;
	out = marker;
	return MarkerStatus::Ok;
}

bool writeMarker(const std::string& path, const Marker& marker)
{
	if (marker.account.find('\n') != std::string::npos ||
		marker.account.find('\r') != std::string::npos)
		return false;

	char revision[32];
	snprintf(revision, sizeof(revision), "%lld", marker.revision);
	std::string content = "umrk-ra-account 1\n";
	content += "state ";
	content += transitionName(marker.transition);
	content += "\nrevision ";
	content += revision;
	content += "\naccount ";
	content += marker.account;
	content += "\n";

	// Same directory, so the rename cannot cross a filesystem, and a failure
	// anywhere below leaves the previous marker exactly as it was.
	const std::string temporary = path + ".tmp";
	FILE *f = fopen(temporary.c_str(), "wb");
	if (f == nullptr)
		return false;
	bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
	if (ok)
		ok = fflush(f) == 0;
#ifndef _WIN32
	if (ok)
		// FAT32 gives no ordering guarantee between the data and the rename,
		// but flushing before it still turns the common "power lost during the
		// write" case into "the previous marker survives".
		ok = fsync(fileno(f)) == 0;
#endif
	if (fclose(f) != 0)
		ok = false;
	if (ok)
		ok = rename(temporary.c_str(), path.c_str()) == 0;
	if (!ok)
		remove(temporary.c_str());
	return ok;
}

Decision decide(const Snapshot& snapshot, MarkerStatus markerStatus,
		const Marker& marker, const std::string& persistedAccount,
		bool persistedTokenPresent)
{
	Decision decision;
	const bool managedStateExists = markerStatus != MarkerStatus::Absent;

	if (snapshot.handoff == Handoff::None)
	{
		// Not an authorized launch, or a launcher that predates the contract.
		// With managed state on disk this must not quietly become an unmanaged
		// session running the last imported token.
		decision.action = managedStateExists ? Action::SuppressManaged : Action::Unmanaged;
		decision.reason = managedStateExists ? "handoff-missing" : "unmanaged";
		return decision;
	}

	if (snapshot.handoff == Handoff::Malformed)
	{
		decision.action = managedStateExists ? Action::SuppressManaged : Action::Idle;
		decision.reason = snapshot.primaryReason();
		return decision;
	}

	switch (snapshot.state)
	{
	case State::Configured:
		decision.revision = snapshot.revision;
		decision.account = snapshot.username;
		if (markerStatus == MarkerStatus::Ok &&
			marker.transition == Transition::Accepted &&
			marker.revision == snapshot.revision &&
			marker.account == snapshot.username &&
			persistedAccount == snapshot.username &&
			persistedTokenPresent)
		{
			// Marker, snapshot and DSperate's own persisted account all agree.
			decision.action = Action::ReuseToken;
			decision.reason = "token-reuse";
		}
		else
		{
			decision.action = Action::ImportLogin;
			if (markerStatus == MarkerStatus::Absent)
				decision.reason = "first-import";
			else if (markerStatus == MarkerStatus::Malformed)
				decision.reason = "marker-malformed";
			else if (marker.transition == Transition::Pending)
				decision.reason = "marker-pending";
			else if (marker.account != snapshot.username)
				decision.reason = "account-changed";
			else if (marker.revision != snapshot.revision)
				decision.reason = "revision-changed";
			else if (!persistedTokenPresent)
				decision.reason = "token-missing";
			else
				decision.reason = "account-mismatch";
		}
		return decision;

	case State::SignedOut:
		decision.revision = snapshot.revision;
		if (markerStatus == MarkerStatus::Ok &&
			marker.transition == Transition::SignedOut &&
			marker.revision == snapshot.revision)
		{
			// Already durable: the bridge keeps this session managed without
			// rewriting the marker or permitting native token fallback.
			decision.action = Action::Idle;
			decision.reason = "signed-out";
		}
		else
		{
			decision.action = Action::SignOut;
			decision.reason = "sign-out";
		}
		return decision;

	case State::NeverConfigured:
		if (!managedStateExists) {
			// Nothing was ever imported: an independently configured native
			// account is the user's own and stays untouched.
			decision.action = Action::Unmanaged;
			decision.reason = "never-configured";
		}
		else {
			// The account rows are gone while managed state remains. Clear the
			// managed credentials; revision 0 records that the sign-out carried
			// none, and never matches a real revision.
			decision.action = Action::SignOut;
			decision.reason = "never-configured-managed";
		}
		return decision;

	case State::Invalid:
	case State::Unreadable:
		// Not sign-out. Durable data survives for repair, and this session
		// simply runs without managed authentication.
		decision.action = managedStateExists ? Action::SuppressManaged : Action::Unmanaged;
		decision.reason = snapshot.state == State::Invalid ? "account-invalid"
														   : "account-unreadable";
		return decision;
	}

	decision.action = Action::SuppressManaged;
	decision.reason = "unknown-state";
	return decision;
}

} // namespace ds::cheevos::ra_account
