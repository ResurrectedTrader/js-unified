#include "support/harness.h"

#include <cstring>
#include <iostream>
#include <utility>

namespace ub_test {
namespace {

std::string g_skipReason;

}  // namespace

void ReportSkip(std::string_view reason) {
    g_skipReason.assign(reason);
    // The exact text CTest matches on; see tests/CMakeLists.txt.
    std::cout << "[unibind] SKIPPED: " << reason << "\n";
}

void ReportUnimplementedArea(std::string_view area) {
    ReportSkip(
        std::string(area).append(" is not implemented by the ").append(ub::Platform::BackendName()).append(" backend"));
}

std::string TakeSkipReason() {
    std::string reason;
    reason.swap(g_skipReason);
    return reason;
}

void EngineFaultLog::Clear() noexcept {
    count.store(0, std::memory_order_release);
    fault = ub::EngineFault::OutOfMemory;
    isolate = nullptr;
    message.fill('\0');
    sawData = false;
}

EngineFaultLog& EngineFaults() noexcept {
    static EngineFaultLog log;
    return log;
}

void RecordEngineFault(const ub::EngineFaultReport& report, ub::CallbackData data) {
    auto* log = data.As<EngineFaultLog>();
    if (log == nullptr) {
        // Nothing to record into, and nowhere to say so: the one thing a
        // handler may not do is allocate, and that includes whatever a stream
        // would.
        return;
    }
    log->sawData = true;
    log->fault = report.fault;
    log->isolate = report.isolate;
    // Copied into storage that already exists. `std::string` here would be the
    // bug this callback's contract is about.
    const std::size_t room = EngineFaultLog::TEXT - 1;
    const std::size_t taken = report.message.size() < room ? report.message.size() : room;
    std::memcpy(log->message.data(), report.message.data(), taken);
    log->message[taken] = '\0';
    log->count.fetch_add(1, std::memory_order_acq_rel);
}

ub::PlatformOptions PlatformOptionsWithFaultHandler() noexcept {
    return ub::PlatformOptions{.onEngineFault = &RecordEngineFault,
                               .engineFaultData = ub::CallbackData::For(EngineFaults())};
}

Fixture::Fixture() : isolate(MakeIsolate()), scope(*isolate), context(MakeContext(*isolate)), entered(context) {}

std::unique_ptr<ub::Isolate> Fixture::MakeIsolate() {
    auto created = ub::Isolate::New();
    REQUIRE(created != nullptr);
    return created;
}

ub::Context Fixture::MakeContext(ub::Isolate& isolate) {
    auto created = ub::Context::New(isolate);
    REQUIRE(created.has_value());
    return std::move(*created);
}

ub::Local<ub::String> Str(ub::Isolate& isolate, std::string_view utf8) {
    auto made = ub::String::New(isolate, utf8);
    REQUIRE(made.has_value());
    return *made;
}

ub::Local<ub::Value> Eval(const ub::Context& context, std::string_view source) {
    auto result = ub::Evaluate(context, source);
    REQUIRE_MESSAGE(result.has_value(), "evaluating: ", source);
    return *result;
}

std::int32_t EvalInt(const ub::Context& context, std::string_view source) {
    auto value = Eval(context, source);
    auto asInt = value.ToInt32(context);
    REQUIRE(asInt.has_value());
    return *asInt;
}

double EvalNumber(const ub::Context& context, std::string_view source) {
    auto value = Eval(context, source);
    auto asNumber = value.ToNumber(context);
    REQUIRE(asNumber.has_value());
    return *asNumber;
}

std::string EvalText(const ub::Context& context, std::string_view source) {
    auto value = Eval(context, source);
    auto asString = value.ToString(context);
    REQUIRE(asString.has_value());
    return asString->Utf8Value();
}

bool EvalTruth(const ub::Context& context, std::string_view source) {
    auto value = Eval(context, source);
    auto asBool = value.ToBoolean(context);
    REQUIRE(asBool.has_value());
    return *asBool;
}

std::string TextOf(const ub::Local<ub::Value>& value) {
    auto asString = value.To<ub::String>();
    REQUIRE(asString.has_value());
    return asString->Utf8Value();
}

}  // namespace ub_test
