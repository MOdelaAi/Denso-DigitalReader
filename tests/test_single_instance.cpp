#include <catch2/catch_test_macros.hpp>

#include "instance/single_instance.h"

#include <QFile>
#include <QTemporaryDir>

using denso::instance::RunState;
using denso::instance::SingleInstance;

TEST_CASE("a second acquire on the same lock fails", "[instance]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString lock = dir.filePath(QStringLiteral("denso.lock"));

    SingleInstance first(lock);
    REQUIRE(first.acquire());
    REQUIRE(first.is_held());

    SingleInstance second(lock);
    REQUIRE_FALSE(second.acquire());
    REQUIRE_FALSE(second.is_held());
}

TEST_CASE("running_state reports NotRunning when nothing holds the lock", "[instance]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString lock = dir.filePath(QStringLiteral("denso.lock"));

    REQUIRE(SingleInstance::running_state(lock) == RunState::NotRunning);
}

TEST_CASE("running_state reports Running while the lock is held", "[instance]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString lock = dir.filePath(QStringLiteral("denso.lock"));

    SingleInstance held(lock);
    REQUIRE(held.acquire());
    REQUIRE(SingleInstance::running_state(lock) == RunState::Running);
}

TEST_CASE("running_state reports Indeterminate when the lock path cannot be created",
          "[instance]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    // A path inside a directory that does not exist: QLockFile cannot create
    // the underlying file, so this must be Indeterminate — never Running (a
    // false "Running" would make a --check-running caller like prerm refuse
    // every upgrade forever) and never NotRunning (a caller could then
    // proceed as though it were safe to start).
    //
    // Deliberately NOT testing a permission failure via chmod: that is
    // unreliable under MSYS2 on Windows. A missing parent directory is the
    // portable way to make lock creation fail.
    const QString lock = dir.filePath(QStringLiteral("missing-subdir/denso.lock"));
    REQUIRE(SingleInstance::running_state(lock) == RunState::Indeterminate);
}

TEST_CASE("the lock is released on destruction", "[instance]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString lock = dir.filePath(QStringLiteral("denso.lock"));

    {
        SingleInstance scoped(lock);
        REQUIRE(scoped.acquire());
    }
    REQUIRE(SingleInstance::running_state(lock) == RunState::NotRunning);

    SingleInstance again(lock);
    REQUIRE(again.acquire());
}

TEST_CASE("running_state leaves no lock file behind when nothing holds it", "[instance]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString lock = dir.filePath(QStringLiteral("denso.lock"));

    // running_state is the ONE mode allowed to touch the lock (it must
    // tryLock to answer), but it must not leave a corpse that later looks
    // like an owner.
    REQUIRE(SingleInstance::running_state(lock) == RunState::NotRunning);
    REQUIRE_FALSE(QFile::exists(lock));
}
