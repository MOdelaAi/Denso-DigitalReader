#include <catch2/catch_test_macros.hpp>

#include "paths/paths.h"

#include <QCoreApplication>
#include <QDir>

namespace {

/// Sets DENSO_DATA_DIR for one test case and always restores it. qputenv leaks
/// into every later test case otherwise.
class EnvGuard {
public:
    explicit EnvGuard(const QByteArray& value) {
        had_ = qEnvironmentVariableIsSet("DENSO_DATA_DIR");
        if (had_) prev_ = qgetenv("DENSO_DATA_DIR");
        qputenv("DENSO_DATA_DIR", value);
    }
    ~EnvGuard() {
        if (had_) qputenv("DENSO_DATA_DIR", prev_);
        else qunsetenv("DENSO_DATA_DIR");
    }
private:
    bool had_ = false;
    QByteArray prev_;
};

/// Unsets DENSO_DATA_DIR for one test case and always restores it.
class EnvUnsetGuard {
public:
    EnvUnsetGuard() {
        had_ = qEnvironmentVariableIsSet("DENSO_DATA_DIR");
        if (had_) prev_ = qgetenv("DENSO_DATA_DIR");
        qunsetenv("DENSO_DATA_DIR");
    }
    ~EnvUnsetGuard() { if (had_) qputenv("DENSO_DATA_DIR", prev_); }
private:
    bool had_ = false;
    QByteArray prev_;
};

} // namespace

using namespace denso::paths;

TEST_CASE("data_dir honors DENSO_DATA_DIR", "[paths]") {
    EnvGuard g("/opt/denso/data");
    REQUIRE(data_dir() == QStringLiteral("/opt/denso/data"));
}

TEST_CASE("data_dir falls back to the application dir when unset", "[paths]") {
    EnvUnsetGuard g;
    REQUIRE(data_dir() == QCoreApplication::applicationDirPath());
}

TEST_CASE("an empty DENSO_DATA_DIR falls back, not to an empty path", "[paths]") {
    EnvGuard g("");
    REQUIRE(data_dir() == QCoreApplication::applicationDirPath());
}

TEST_CASE("a relative DENSO_DATA_DIR is cleaned, not rejected", "[paths]") {
    EnvGuard g("foo/../bar");
    REQUIRE(data_dir() == QStringLiteral("bar"));
}

TEST_CASE("a trailing slash does not double up in derived paths", "[paths]") {
    EnvGuard g("/opt/denso/data/");
    REQUIRE(db_file() == QStringLiteral("/opt/denso/data/denso.db"));
}

TEST_CASE("a filesystem root does not double its separator", "[paths]") {
    // cleanPath() keeps the separator on a root, so naive concatenation would
    // produce "//denso.db". This is why the impl uses QDir::filePath.
    EnvGuard g("/");
    REQUIRE(db_file() == QStringLiteral("/denso.db"));
}

TEST_CASE("every derived path hangs off data_dir", "[paths]") {
    EnvGuard g("/opt/denso/data");
    REQUIRE(db_file()               == QStringLiteral("/opt/denso/data/denso.db"));
    REQUIRE(log_file()              == QStringLiteral("/opt/denso/data/denso.log"));
    REQUIRE(models_dir()            == QStringLiteral("/opt/denso/data/models"));
    REQUIRE(trt_cache_dir()         == QStringLiteral("/opt/denso/data/models/trt_cache"));
    REQUIRE(lock_file()             == QStringLiteral("/opt/denso/data/denso.lock"));
    REQUIRE(legacy_settings_json()  == QStringLiteral("/opt/denso/data/settings.json"));
    REQUIRE(status_file()           == QStringLiteral("/opt/denso/data/status.json"));
}

TEST_CASE("status_file lives in the data dir", "[paths]") {
    CHECK(denso::paths::status_file().endsWith(QStringLiteral("status.json")));
    CHECK(denso::paths::status_file() ==
          QDir(denso::paths::data_dir()).filePath(QStringLiteral("status.json")));
}
