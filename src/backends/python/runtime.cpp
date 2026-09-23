// STUB: generated placeholders for the runtime area; replaced by the real implementation.
#include "internal.h"

namespace ub::detail {

std::optional<Slot> MakePromise(const Context& context) { return std::nullopt; }
std::optional<bool> ResolvePromise(const Context& context, Slot promise, Slot value) { return std::nullopt; }
std::optional<bool> RejectPromise(const Context& context, Slot promise, Slot reason) { return std::nullopt; }
PromiseState PromiseStateOf(Slot promise) noexcept { return PromiseState::Pending; }

struct RuntimeState {};
void DestroyRuntimeState(RuntimeState* state) noexcept { delete state; }
bool InitRuntimeTypes(Isolate&, PyObject*) noexcept { return true; }
bool PlatformRuntimeSetup() noexcept { return true; }
bool IsolateRuntimeSetup(Isolate&, const IsolateOptions&) noexcept { return true; }
void IsolateRuntimeTeardown(Isolate&) noexcept {}
PyObject* SpawnCoroutine(Isolate&, PyObject* coroutine) noexcept { Py_DECREF(coroutine); PyErr_SetString(PyExc_NotImplementedError, "promises"); return nullptr; }

}  // namespace ub::detail

namespace ub {
HeapStatistics Isolate::GetHeapStatistics() const noexcept { return {}; }
void Isolate::RequestGarbageCollection() noexcept {}
void Isolate::SetHeapLimitCallback(HeapLimitCallback, CallbackData) noexcept {}
void Isolate::TerminateExecution() noexcept {}
bool Isolate::IsExecutionTerminating() const noexcept { return false; }
void Isolate::CancelTerminateExecution() noexcept {}
bool Isolate::RequestInterrupt(InterruptCallback, CallbackData) noexcept { return false; }
bool Isolate::PostJob(JobCallback, CallbackData) noexcept { return false; }
bool Isolate::PostDelayedJob(JobCallback, CallbackData, double) noexcept { return false; }
void Isolate::PumpJobs() {}
}  // namespace ub
