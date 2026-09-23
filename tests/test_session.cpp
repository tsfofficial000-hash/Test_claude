//
// The SMC session's lifecycle.
//
// This exists because of a real crash in the shipped binary. The command line
// tool returns a session out of a std::optional, which move-constructs it and
// leaves the original empty; the original is then destroyed at the end of the
// function. A moved-from session whose destructor dereferenced its state took
// the whole process down - but only on the success path, where the move actually
// happens, so it never appeared on any machine without a Mac's SMC. Every
// command in the CLI would have crashed on real hardware and nowhere else.
//
// The failure mode is identical in both implementations of SmcSession, so it is
// testable here, on the platform where the tests actually run.
//
#include <string>
#include <utility>

#include "platform/platform.h"
#include "test_framework.h"

using namespace fanforge;

TEST(a_moved_from_session_is_safe_to_use_and_destroy) {
    SmcSession original;
    SmcSession moved(std::move(original));

    // Every accessor has to tolerate the emptied source.
    CHECK(original.transport() == nullptr);
    CHECK(original.activeName() == "none");
    CHECK(original.attempts().empty());
    CHECK(!original.summary().empty());

    // Being closed explicitly, and being destroyed, must both be fine.
    original.close();
}

TEST(a_moved_from_session_can_still_be_opened) {
    SmcSession original;
    SmcSession moved(std::move(original));

    // "Moved from" is a usable state, not a trap: it rebuilds itself.
    original.open();
    CHECK(original.transport() == nullptr);   // accurate, there is no SMC here
    CHECK(!original.attempts().empty());
}

TEST(move_assignment_releases_what_the_target_was_holding) {
    SmcSession a;
    SmcSession b;
    a.open();

    // Assigning over an open session must not leak or corrupt it.
    a = std::move(b);
    CHECK(a.transport() == nullptr);

    // And the source is safe afterwards too.
    CHECK(b.attempts().empty());
    CHECK(b.activeName() == "none");
    b.close();
}

TEST(a_fresh_session_reports_why_it_cannot_open) {
    SmcSession session;
    CHECK(!session.open());
    CHECK(session.transport() == nullptr);
    CHECK(session.activeName() == "none");
    CHECK(!session.attempts().empty());
    CHECK(session.summary().find("unavailable") != std::string::npos);
}

TEST(closing_a_session_twice_is_harmless) {
    SmcSession session;
    session.open();
    session.close();
    session.close();          // idempotent
    CHECK(session.transport() == nullptr);
}
