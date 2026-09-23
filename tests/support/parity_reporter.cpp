/// \file
/// A doctest reporter that writes one line per test case: `STATE|name`.
///
/// The parity comparison needs the two backends' results in a form it can join
/// on the case name, and it needs three states rather than two - a case the
/// backend cannot answer yet is neither a pass nor a failure. doctest's own
/// reporters give two states and a lot of prose, so this writes the three.

#include <doctest/doctest.h>

#include <ostream>
#include <string>

#include "support/harness.h"
#include "unibind/isolate.h"

namespace {

struct ParityReporter : public doctest::IReporter {
    std::ostream& out;
    const doctest::TestCaseData* current = nullptr;
    bool failed = false;

    explicit ParityReporter(const doctest::ContextOptions& options) : out(*options.cout) {}

    void report_query(const doctest::QueryData& /*query*/) override {}

    void test_run_start() override { out << "#backend|" << ub::Platform::BackendName() << "\n"; }

    void test_run_end(const doctest::TestRunStats& /*stats*/) override { out.flush(); }

    void test_case_start(const doctest::TestCaseData& data) override {
        current = &data;
        failed = false;
        // Any reason left over from an earlier case is not this case's.
        (void)ub_test::TakeSkipReason();
    }

    void test_case_reenter(const doctest::TestCaseData& /*data*/) override {}

    void test_case_end(const doctest::CurrentTestCaseStats& stats) override {
        const bool broken = failed || stats.failure_flags != 0 || stats.numAssertsFailedCurrentTest > 0;
        const std::string skipped = ub_test::TakeSkipReason();
        const char* state = "PASSED";
        if (broken) {
            state = "FAILED";
        } else if (!skipped.empty()) {
            state = "SKIPPED";
        }
        out << state << "|" << (current != nullptr ? current->m_name : "?") << "\n";
        current = nullptr;
    }

    void test_case_exception(const doctest::TestCaseException& /*exception*/) override { failed = true; }

    void subcase_start(const doctest::SubcaseSignature& /*signature*/) override {}
    void subcase_end() override {}

    void log_assert(const doctest::AssertData& data) override {
        if (data.m_failed) {
            failed = true;
        }
    }

    void log_message(const doctest::MessageData& /*message*/) override {}

    void test_case_skipped(const doctest::TestCaseData& data) override { out << "SKIPPED|" << data.m_name << "\n"; }
};

DOCTEST_REGISTER_REPORTER("unibind-parity", 1, ParityReporter);

}  // namespace
