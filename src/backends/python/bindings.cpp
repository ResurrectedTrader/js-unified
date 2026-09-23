// STUB: generated placeholders for the bindings area; replaced by the real implementation.
#include "internal.h"

namespace ub::detail {

std::optional<Slot> MakeFunction(const Context& context, FunctionCallback callback, CallbackData data) { return std::nullopt; }
std::optional<Slot> MakeFunctionWithValue(const Context& context, FunctionCallback callback, Slot data) { return std::nullopt; }
std::optional<Slot> CallFunction(const Context& context, Slot function, Slot receiver, std::span<const Slot> arguments) { return std::nullopt; }
std::optional<Slot> ConstructObject(const Context& context, Slot function, std::span<const Slot> arguments) { return std::nullopt; }
Isolate& CallbackIsolate(const CallbackState& state) noexcept { return *CurrentIsolate(); }
const Context& CallbackContext(const CallbackState& state) noexcept { static const Context none; return none; }
std::uint32_t CallbackArgumentCount(const CallbackState& state) noexcept { return 0; }
Slot CallbackArgument(const CallbackState& state, std::uint32_t index) noexcept { return Slot{}; }
Slot CallbackThis(const CallbackState& state) noexcept { return Slot{}; }
Slot CallbackHolder(const CallbackState& state) noexcept { return Slot{}; }
bool CallbackIsConstruct(const CallbackState& state) noexcept { return false; }
CallbackData CallbackDataOf(const CallbackState& state) noexcept { return {}; }
Slot CallbackValueData(const CallbackState& state) noexcept { return Slot{}; }
void SetReturnSlot(const CallbackState& state, Slot value) noexcept {}
void SetReturnUndefined(const CallbackState& state) noexcept {}
void SetReturnNull(const CallbackState& state) noexcept {}
void SetReturnBoolean(const CallbackState& state, bool value) noexcept {}
void SetReturnNumber(const CallbackState& state, double value) noexcept {}
void SetReturnInteger(const CallbackState& state, std::int32_t value) noexcept {}
TemplateRec* NewObjectTemplate(Isolate& isolate) { return nullptr; }
TemplateRec* NewFunctionTemplate(Isolate& isolate, FunctionCallback callback, CallbackData data) { return nullptr; }
void TemplateSetConstant(TemplateRec* tpl, std::string_view name, Constant value, PropertyAttribute attributes) {}
void TemplateSetMethod(TemplateRec* tpl, std::string_view name, FunctionCallback callback, CallbackData data, PropertyAttribute attributes) {}
void TemplateSetSymbolMethod(TemplateRec* tpl, WellKnownSymbol key, FunctionCallback callback, CallbackData data) {}
void TemplateSetAccessor(TemplateRec* tpl, std::string_view name, AccessorGetterCallback getter, AccessorSetterCallback setter, CallbackData data, PropertyAttribute attributes) {}
void TemplateSetTemplate(TemplateRec* tpl, std::string_view name, TemplateRec* value, PropertyAttribute attributes) {}
void TemplateSetNamedHandler(TemplateRec* tpl, const NamedPropertyHandler& handler) {}
void TemplateSetIndexedHandler(TemplateRec* tpl, const IndexedPropertyHandler& handler) {}
void TemplateSetClassName(TemplateRec* tpl, std::string_view name) {}
void TemplateInherit(TemplateRec* child, TemplateRec* parent) {}
TemplateRec* TemplatePrototype(TemplateRec* tpl) { return nullptr; }
TemplateRec* TemplateInstance(TemplateRec* tpl) { return nullptr; }
std::optional<Slot> TemplateNewInstance(const Context& context, TemplateRec* tpl) { return std::nullopt; }
std::optional<Slot> TemplateGetFunction(const Context& context, TemplateRec* tpl) { return std::nullopt; }
std::optional<bool> TemplateHasInstance(const Context& context, TemplateRec* tpl, Slot value) { return std::nullopt; }
ClassRec* NewClass(Isolate& isolate, std::string_view name, TypeId nativeType) { return nullptr; }
void ClassSetConstructor(ClassRec* rec, NativeConstructor constructor, bool callableWithoutNew) {}
TemplateRec* ClassPrototypeTemplate(ClassRec* rec) { return nullptr; }
TemplateRec* ClassConstructorTemplate(ClassRec* rec) { return nullptr; }
TemplateRec* ClassInstanceTemplate(ClassRec* rec) { return nullptr; }
std::optional<Slot> ClassGetConstructor(const Context& context, ClassRec* rec) { return std::nullopt; }
std::optional<Slot> ClassInstantiate(const Context& context, ClassRec* rec, NativeBox* native) noexcept { return std::nullopt; }
std::optional<bool> ClassHasInstance(const Context& context, ClassRec* rec, Slot value) { return std::nullopt; }

struct BindingsState {};
void DestroyBindingsState(BindingsState* state) noexcept { delete state; }
bool InitBindingTypes(Isolate&, PyObject*) noexcept { return true; }
void IsolateBindingsTeardown(Isolate&) noexcept {}

}  // namespace ub::detail
