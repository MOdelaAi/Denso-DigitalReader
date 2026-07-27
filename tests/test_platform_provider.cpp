// Release B, Slice 7 — the platform-information provider.
//
// Proves the provider MEASURES the running platform and normalizes it to the
// manifest built_for representation, that every failure mode FAILS CLOSED (never a
// substituted qualified constant), and that a probe failure cannot corroborate a
// TensorRT engine nor admit one to the warm-up allow-list. No GPU is required: the
// pure normalization is exercised directly and the probe is an injected seam.
//
// Lives in denso_integration_tests because the provider is an app-layer TU
// (denso_app); the injected probe means no real device is ever touched.
#include <catch2/catch_test_macros.hpp>

#include "platform/platform_info.h"

#include "detection/detection.h"
#include "models/compatibility.h"
#include "models/hashing.h"
#include "models/manifest.h"
#include "models/model_identity.h"
#include "mode/mode.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using denso::mode::TargetMode;
using denso::models::Backend;
using denso::models::Manifest;
using denso::models::ManifestView;
using denso::models::ModelGeneration;
using denso::models::ModelMetadata;
using denso::models::PlatformInfo;
using denso::detection::DetectionModel;

namespace pf = denso::platform;

// ─────────────────────────────────────────────────────────────────────────────
// Pure normalization — the manifest built_for representation.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("TensorRT version normalizes to major.minor", "[platform]") {
    CHECK(pf::normalize_trt(10, 3) == std::optional<std::string>{"10.3"});  // 10.3.0
    CHECK(pf::normalize_trt(8, 6) == std::optional<std::string>{"8.6"});    // 8.6.1
}

TEST_CASE("running TensorRT lib version decodes to major.minor", "[platform]") {
    // getInferLibVersion() encodes (MAJOR*100 + MINOR)*100 + PATCH: 100300 → 10.3
    // (the qualified platform), 80601 → 8.6.
    CHECK(pf::decode_trt_lib_version(100300) == std::optional<std::pair<int, int>>{{10, 3}});
    CHECK(pf::decode_trt_lib_version(80601) == std::optional<std::pair<int, int>>{{8, 6}});
    CHECK_FALSE(pf::decode_trt_lib_version(5000).has_value());  // too small → no major
    // Compose decode → normalize == the manifest representation.
    auto mm = pf::decode_trt_lib_version(100300);
    REQUIRE(mm.has_value());
    CHECK(pf::normalize_trt(mm->first, mm->second) == std::optional<std::string>{"10.3"});
}

TEST_CASE("CUDA runtime version normalizes to major.minor", "[platform]") {
    CHECK(pf::normalize_cuda(12060) == std::optional<std::string>{"12.6"});
    CHECK(pf::normalize_cuda(12040) == std::optional<std::string>{"12.4"});
}

TEST_CASE("compute capability normalizes to concatenated majorminor", "[platform]") {
    CHECK(pf::normalize_sm(8, 7) == std::optional<std::string>{"87"});
    CHECK(pf::normalize_sm(7, 5) == std::optional<std::string>{"75"});
}

// ─────────────────────────────────────────────────────────────────────────────
// Every malformed / unavailable input is rejected (fail closed) — no bad
// measurement can become a valid-looking version.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("normalization rejects malformed or missing versions", "[platform]") {
    SECTION("TensorRT version unavailable / zero / negative") {
        CHECK_FALSE(pf::normalize_trt(0, 0).has_value());
        CHECK_FALSE(pf::normalize_trt(-1, 3).has_value());
        CHECK_FALSE(pf::normalize_trt(10, -1).has_value());
        CHECK_FALSE(pf::decode_trt_lib_version(0).has_value());   // lib version unavailable
        CHECK_FALSE(pf::decode_trt_lib_version(-5).has_value());
    }
    SECTION("CUDA version zero / negative / malformed") {
        CHECK_FALSE(pf::normalize_cuda(0).has_value());
        CHECK_FALSE(pf::normalize_cuda(-1).has_value());
        CHECK_FALSE(pf::normalize_cuda(999).has_value());  // no major → malformed
    }
    SECTION("invalid compute capability") {
        CHECK_FALSE(pf::normalize_sm(0, 0).has_value());
        CHECK_FALSE(pf::normalize_sm(-1, 0).has_value());
        CHECK_FALSE(pf::normalize_sm(8, -1).has_value());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// normalize(RawPlatform): all fields good → the qualified triple; any bad field →
// nullopt (the whole measurement is rejected, not partially trusted).
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("normalize(RawPlatform) yields the qualified triple, or nullopt", "[platform]") {
    const pf::RawPlatform good{10, 3, 12060, 8, 7};
    const auto pi = pf::normalize(good);
    REQUIRE(pi.has_value());
    CHECK(pi->trt == "10.3");
    CHECK(pi->cuda == "12.6");
    CHECK(pi->sm == "87");

    CHECK_FALSE(pf::normalize(pf::RawPlatform{0, 3, 12060, 8, 7}).has_value());   // bad TRT
    CHECK_FALSE(pf::normalize(pf::RawPlatform{10, 3, 0, 8, 7}).has_value());      // bad CUDA
    CHECK_FALSE(pf::normalize(pf::RawPlatform{10, 3, 12060, 0, 0}).has_value());  // bad SM
}

// ─────────────────────────────────────────────────────────────────────────────
// resolve_platform_info(probe): composes probe → normalize. A successful probe
// yields the measured triple; every failure path yields nullopt.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("resolve_platform_info composes probe and normalization", "[platform]") {
    SECTION("successful probe → measured platform") {
        auto probe = []() -> std::optional<pf::RawPlatform> {
            return pf::RawPlatform{10, 3, 12060, 8, 7};
        };
        const auto pi = pf::resolve_platform_info(probe);
        REQUIRE(pi.has_value());
        CHECK(pi->trt == "10.3");
        CHECK(pi->cuda == "12.6");
        CHECK(pi->sm == "87");
    }
    SECTION("CUDA runtime query fails → probe nullopt → nullopt") {
        CHECK_FALSE(pf::resolve_platform_info(
                        []() -> std::optional<pf::RawPlatform> { return std::nullopt; })
                        .has_value());
    }
    SECTION("no CUDA device → probe nullopt → nullopt") {
        // The Jetson probe returns nullopt for device_count < 1; modelled here as a
        // probe that cannot measure.
        CHECK_FALSE(pf::resolve_platform_info(
                        []() -> std::optional<pf::RawPlatform> { return std::nullopt; })
                        .has_value());
    }
    SECTION("device-properties query fails → probe nullopt → nullopt") {
        CHECK_FALSE(pf::resolve_platform_info(
                        []() -> std::optional<pf::RawPlatform> { return std::nullopt; })
                        .has_value());
    }
    SECTION("probe returns a malformed measurement → normalize nullopt → nullopt") {
        CHECK_FALSE(pf::resolve_platform_info([]() -> std::optional<pf::RawPlatform> {
                        return pf::RawPlatform{0, 0, 0, 0, 0};  // TRT unavailable
                    }).has_value());
    }
    SECTION("empty (unset) probe → nullopt") {
        CHECK_FALSE(pf::resolve_platform_info(pf::Probe{}).has_value());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// The production seam. On ONNX Runtime (the Windows host build) there is no
// qualified device: the real probe returns nullopt and measured_platform_info()
// fails closed to an EMPTY PlatformInfo (never the qualified constants), quietly,
// because ORT ignores the field. (The TensorRt measured value is proved on-device.)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("measured_platform_info fails closed to empty, never the constants", "[platform]") {
    const PlatformInfo pi = pf::measured_platform_info();
    if (denso::models::active_backend() == Backend::OnnxRuntime) {
        CHECK(pi.trt.empty());
        CHECK(pi.cuda.empty());
        CHECK(pi.sm.empty());
    }
    // Whatever the backend, a fail-closed / measured value is NEVER a partial
    // manufacture: it is either the fully-measured triple or fully empty.
    const bool empty = pi.trt.empty() && pi.cuda.empty() && pi.sm.empty();
    const bool full = !pi.trt.empty() && !pi.cuda.empty() && !pi.sm.empty();
    CHECK((empty || full));
}

// ─────────────────────────────────────────────────────────────────────────────
// Shared caller behaviour + no-hard-coded-constant invariant (kills the "re-add
// the constant" and "give startup/headless separate normalization" mutations).
// A static assertion over the two production call sites.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("startup and headless share the provider; no hard-coded platform remains",
          "[platform]") {
    const QString root = QStringLiteral(DENSO_SOURCE_DIR);
    for (const char* rel : {"src/app/ui/startup.cpp", "src/app/cli/run_headless.cpp"}) {
        QFile f(root + "/" + QLatin1String(rel));
        REQUIRE(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString src = QString::fromUtf8(f.readAll());

        INFO(rel);
        // Both obtain their PlatformInfo from the ONE shared provider.
        CHECK(src.contains(QStringLiteral("measured_platform_info(")));
        // Neither carries the qualified triple as a literal.
        CHECK_FALSE(src.contains(QStringLiteral("\"10.3\"")));
        CHECK_FALSE(src.contains(QStringLiteral("\"12.6\"")));
        CHECK_FALSE(src.contains(QStringLiteral("\"87\"")));
        // Neither normalizes the platform itself (normalization lives once, in the
        // provider TU) — no duplicated logic across the two paths.
        CHECK_FALSE(src.contains(QStringLiteral("normalize_trt")));
        CHECK_FALSE(src.contains(QStringLiteral("normalize_cuda")));
        CHECK_FALSE(src.contains(QStringLiteral("normalize_sm")));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Fail-closed INTEGRATION, on the TensorRt resolution path (forced via the
// ManifestView test seam so it is host-independent). A probe failure yields the
// empty fail-closed platform; with it, a genuinely-corroborated engine:
//   (1) cannot produce provenance_ok == true, and
//   (2) cannot enter the warm-up allow-list.
// The good platform is the control: everything passes when the probe succeeds.
// ─────────────────────────────────────────────────────────────────────────────
namespace {

std::string write_and_hash(const QString& dir, const std::string& name,
                           const QByteArray& bytes) {
    const QString path = QDir(dir).filePath(QString::fromStdString(name));
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write(bytes) == bytes.size());
    f.close();
    const auto h = denso::models::file_sha256(path);
    REQUIRE(h.has_value());
    return *h;
}

// A DECLARED schema-2 TensorRT generation whose engine + sidecar exist on disk and
// hash-match, and whose built_for is the qualified triple. Independent of the host
// backend — this is exercised through the TensorRt test seam.
ModelGeneration declare_trt(const QString& dir, const std::string& id,
                            const std::string& family,
                            const std::vector<std::string>& classes) {
    ModelGeneration g;
    g.declared = true;
    g.canonical_id = id;
    g.family = family;
    g.task = "detect";
    g.input_size = 640;
    g.class_count = static_cast<int>(classes.size());
    g.class_names = classes;

    denso::models::TensorRtArtifact trt;
    trt.engine = id + ".engine";
    trt.engine_sha256 = write_and_hash(dir, trt.engine, QByteArrayLiteral("engine-bytes"));
    trt.sidecar = id + ".names.json";
    QByteArray sidecar = "[";
    for (size_t i = 0; i < classes.size(); ++i)
        sidecar += (i ? ",\"" : "\"") + QByteArray::fromStdString(classes[i]) + "\"";
    sidecar += "]";
    trt.sidecar_sha256 = write_and_hash(dir, trt.sidecar, sidecar);
    trt.class_metadata_source = denso::models::kSourceNamesSidecar;
    trt.built_for = {"10.3", "12.6", "87"};
    g.runtime.tensorrt = trt;
    return g;
}

}  // namespace

TEST_CASE("a platform probe failure cannot corroborate an engine or admit it",
          "[platform]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString dir = tmp.path();

    Manifest m;
    m.schema = 2;
    m.generations.push_back(declare_trt(dir, "digitv3", "digit_numeric", {"0", "1", "2", "3"}));
    // Force the TensorRt resolution path regardless of host (3-arg test seam).
    const ManifestView view(std::move(m), dir, Backend::TensorRt);

    DetectionModel row;
    row.name = "digitv3";
    row.filename = "digitv3.engine";
    row.class_names = {"0", "1", "2", "3"};

    // The empty platform is EXACTLY what measured_platform_info() substitutes when
    // the probe fails — derive it that way to bind the test to the real fail path.
    const PlatformInfo failed =
        pf::resolve_platform_info([]() -> std::optional<pf::RawPlatform> {
            return std::nullopt;  // simulated probe failure
        }).value_or(PlatformInfo{});
    REQUIRE(failed.trt.empty());  // fail-closed sentinel, not the constants

    const PlatformInfo good{"10.3", "12.6", "87"};  // control: a successful probe

    SECTION("provenance_ok is false after a probe failure, true with the real platform") {
        const ModelMetadata bad = denso::models::resolve_model_metadata(view, row, failed);
        CHECK(bad.declared);            // the generation is still found
        CHECK(bad.artifact_matches);    // class names still agree
        CHECK_FALSE(bad.provenance_ok); // but built_for cannot corroborate → fail closed

        const ModelMetadata ok = denso::models::resolve_model_metadata(view, row, good);
        CHECK(ok.provenance_ok);        // control: the qualified platform corroborates
    }

    SECTION("the model cannot enter the warm-up allow-list after a probe failure") {
        const ModelMetadata bad = denso::models::resolve_model_metadata(view, row, failed);
        CHECK(denso::models::loadable_model_files(TargetMode::DigitReader, {bad}).empty());

        const ModelMetadata ok = denso::models::resolve_model_metadata(view, row, good);
        CHECK(denso::models::loadable_model_files(TargetMode::DigitReader, {ok}) ==
              std::set<std::string>{"digitv3.engine"});  // control: measured → admitted
    }
}
