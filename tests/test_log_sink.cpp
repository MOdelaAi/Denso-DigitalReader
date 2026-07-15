#include <catch2/catch_test_macros.hpp>

#include "logging/log_sink.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

TEST_CASE("RotatingLogSink bounds the active file and rolls archives", "[log]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString base = dir.filePath("denso.log");

    {
        denso::logging::RotatingLogSink sink(base, /*max_bytes=*/100, /*max_files=*/3);
        const QByteArray rec(40, 'x');  // 40 + '\n' = 41 bytes per record
        for (int i = 0; i < 30; ++i) {
            sink.write(rec);
        }
    }

    // max_files=3 total → active (denso.log) + at most 2 archives (.1, .2).
    CHECK(QFile::exists(base));
    CHECK(QFile::exists(base + ".1"));
    CHECK(QFile::exists(base + ".2"));
    CHECK_FALSE(QFile::exists(base + ".3"));  // oldest dropped; total stays bounded

    // Active file never exceeds the cap (+ at most one record it was rotated at).
    CHECK(QFileInfo(base).size() <= 100 + 41);
    CHECK(QFileInfo(base + ".1").size() <= 100 + 41);
}
