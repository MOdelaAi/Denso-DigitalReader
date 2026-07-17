#include <catch2/catch_test_macros.hpp>

#include "instance/single_instance.h"

#include <QFile>
#include <QTemporaryDir>

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

TEST_CASE("is_running reports false when nothing holds the lock", "[instance]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString lock = dir.filePath(QStringLiteral("denso.lock"));

    REQUIRE_FALSE(SingleInstance::is_running(lock));
}

TEST_CASE("is_running reports true while the lock is held", "[instance]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString lock = dir.filePath(QStringLiteral("denso.lock"));

    SingleInstance held(lock);
    REQUIRE(held.acquire());
    REQUIRE(SingleInstance::is_running(lock));
}

TEST_CASE("the lock is released on destruction", "[instance]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString lock = dir.filePath(QStringLiteral("denso.lock"));

    {
        SingleInstance scoped(lock);
        REQUIRE(scoped.acquire());
    }
    REQUIRE_FALSE(SingleInstance::is_running(lock));

    SingleInstance again(lock);
    REQUIRE(again.acquire());
}

TEST_CASE("is_running leaves no lock file behind when nothing holds it", "[instance]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString lock = dir.filePath(QStringLiteral("denso.lock"));

    // is_running is the ONE mode allowed to touch the lock (it must tryLock to
    // answer), but it must not leave a corpse that later looks like an owner.
    REQUIRE_FALSE(SingleInstance::is_running(lock));
    REQUIRE_FALSE(QFile::exists(lock));
}
