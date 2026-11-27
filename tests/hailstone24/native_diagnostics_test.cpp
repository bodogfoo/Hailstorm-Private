#include <doctest/doctest.h>

#include <string>

#include "combat/diagnostics.h"

using namespace hailstone::combat;

TEST_CASE("diagnostics: a record carries who, what, why and the state") {
    DiagnosticsSink sink;
    sink.set_identity("Aegis", "EZ", "WAR", "g1");

    const DiagnosticRecord& rec = sink.emit(DiagnosticModule::Casting, DiagnosticSeverity::Warning, "Cast",
                                            "Child of Fire", false, "NotMemorized", "Combat", 1000);
    CHECK(rec.character == "Aegis");
    CHECK(rec.server == "EZ");
    CHECK(rec.class_short == "WAR");
    CHECK(rec.group_id == "g1");
    CHECK(rec.module == DiagnosticModule::Casting);
    CHECK(rec.severity == DiagnosticSeverity::Warning);
    CHECK(rec.action == "Cast");
    CHECK(rec.target == "Child of Fire");
    CHECK_FALSE(rec.ok);
    CHECK(rec.reason == "NotMemorized");
    // DiagnosticsSink tags the state lowercase, like its other key=value tags
    // (native_engine_diagnostics_test asserts "idle" the same way).
    CHECK(rec.state == "combat");
    CHECK(rec.occurrences == 1);

    const std::string line = format_diagnostic(rec);
    CHECK(line.find("char=Aegis") != std::string::npos);
    CHECK(line.find("module=Casting") != std::string::npos);
    CHECK(line.find("result=failed") != std::string::npos);
    CHECK(line.find("reason=NotMemorized") != std::string::npos);
}

TEST_CASE("diagnostics: an identical repeat inside the window is folded, not appended") {
    DiagnosticsSink sink;
    sink.set_options(DiagnosticsOptions{5000, 200});

    sink.emit(DiagnosticModule::Movement, DiagnosticSeverity::Info, "MoveMelee", "a rat", false,
              "OutOfRange", "Combat", 0);
    CHECK(sink.size() == 1);

    for (std::uint64_t t = 100; t <= 4000; t += 100) {
        sink.emit(DiagnosticModule::Movement, DiagnosticSeverity::Info, "MoveMelee", "a rat", false,
                  "OutOfRange", "Combat", t);
    }
    CHECK(sink.size() == 1); // one record, many occurrences
    const auto recs = sink.records();
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].occurrences == 41);
    CHECK(recs[0].suppressed_repeats == 40);
    CHECK(recs[0].first_ms == 0);
    CHECK(recs[0].last_ms == 4000);
    CHECK(sink.suppressed_total() == 40);
}

TEST_CASE("diagnostics: a repeat after the window is a new record") {
    DiagnosticsSink sink;
    sink.set_options(DiagnosticsOptions{1000, 200});

    sink.emit(DiagnosticModule::Casting, DiagnosticSeverity::Error, "Cast", "x", false, "NoMana", "Combat", 0);
    sink.emit(DiagnosticModule::Casting, DiagnosticSeverity::Error, "Cast", "x", false, "NoMana", "Combat", 5000);
    CHECK(sink.size() == 2);
}

TEST_CASE("diagnostics: a different reason is a different record") {
    DiagnosticsSink sink;
    sink.emit(DiagnosticModule::Casting, DiagnosticSeverity::Warning, "Cast", "x", false, "NoMana", "Combat", 0);
    sink.emit(DiagnosticModule::Casting, DiagnosticSeverity::Warning, "Cast", "x", false, "NotMemorized",
              "Combat", 100);
    CHECK(sink.size() == 2);
}

TEST_CASE("diagnostics: success and failure do not fold into each other") {
    DiagnosticsSink sink;
    sink.emit(DiagnosticModule::Combat, DiagnosticSeverity::Info, "Attack", "a rat", true, "", "Combat", 0);
    sink.emit(DiagnosticModule::Combat, DiagnosticSeverity::Info, "Attack", "a rat", false, "", "Combat", 100);
    CHECK(sink.size() == 2);
}

TEST_CASE("diagnostics: capacity drops the oldest record") {
    DiagnosticsSink sink;
    sink.set_options(DiagnosticsOptions{0, 3}); // window 0 => never fold
    for (int i = 0; i < 10; ++i) {
        sink.emit(DiagnosticModule::Combat, DiagnosticSeverity::Info, "Attack", std::to_string(i), true,
                  "", "Combat", static_cast<std::uint64_t>(i) * 10);
    }
    CHECK(sink.size() == 3);
    const auto recs = sink.records();
    REQUIRE(recs.size() == 3);
    CHECK(recs.front().target == "7");
    CHECK(recs.back().target == "9");
}

TEST_CASE("diagnostics: an unsupported legacy command warns once per session") {
    DiagnosticsSink sink;
    CHECK(sink.emit_unsupported_command("rgmez", 0));
    CHECK(sink.command_warned("rgmez"));
    CHECK(sink.size() == 1);

    // Regardless of how much later it is asked again, it does not print again.
    CHECK_FALSE(sink.emit_unsupported_command("rgmez", 600000));
    CHECK(sink.size() == 1);
    const auto recs = sink.records();
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].occurrences == 2);

    // A different command is its own single warning.
    CHECK(sink.emit_unsupported_command("rgsay", 600001));
    CHECK(sink.size() == 2);
}

TEST_CASE("diagnostics: module filtering and clearing") {
    DiagnosticsSink sink;
    sink.emit(DiagnosticModule::Healing, DiagnosticSeverity::Info, "Heal", "tank", true, "", "Combat", 0);
    sink.emit(DiagnosticModule::Pets, DiagnosticSeverity::Info, "PetAttack", "rat", true, "", "Combat", 10);

    CHECK(sink.records(DiagnosticModule::Healing).size() == 1);
    CHECK(sink.records(DiagnosticModule::Pets).size() == 1);
    CHECK(sink.records(DiagnosticModule::Buffs).empty());

    CHECK_FALSE(sink.last_line().empty());
    sink.clear();
    CHECK(sink.size() == 0);
    CHECK(sink.last_line().empty());
}

TEST_CASE("diagnostics: a value with separators cannot break the line format") {
    DiagnosticsSink sink;
    const DiagnosticRecord& rec = sink.emit(DiagnosticModule::Command, DiagnosticSeverity::Warning, "x",
                                            "bad|name\nwith newline", false, "r", "s", 0);
    const std::string line = format_diagnostic(rec);
    CHECK(line.find('|') == std::string::npos);
    CHECK(line.find('\n') == std::string::npos);
}
