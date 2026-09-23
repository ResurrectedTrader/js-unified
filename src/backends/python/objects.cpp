// STUB: generated placeholders for the objects area; replaced by the real implementation.
#include "internal.h"

namespace ub::detail {

std::optional<Slot> MakeObject(const Context& context) { return std::nullopt; }
std::optional<Slot> MakeArray(const Context& context, std::uint32_t length) { return std::nullopt; }
std::optional<Slot> GetProperty(const Context& context, Slot object, Slot key) { return std::nullopt; }
std::optional<Slot> GetIndex(const Context& context, Slot object, std::uint32_t index) { return std::nullopt; }
std::optional<bool> SetProperty(const Context& context, Slot object, Slot key, Slot value) { return std::nullopt; }
std::optional<bool> SetIndex(const Context& context, Slot object, std::uint32_t index, Slot value) { return std::nullopt; }
std::optional<bool> DefineProperty(const Context& context, Slot object, Slot key, Slot value, PropertyAttribute attributes) { return std::nullopt; }
std::optional<bool> SetAccessorProperty(const Context& context, Slot object, std::string_view name, AccessorGetterCallback getter, AccessorSetterCallback setter, CallbackData data, PropertyAttribute attributes) { return std::nullopt; }
std::optional<bool> HasProperty(const Context& context, Slot object, Slot key) { return std::nullopt; }
std::optional<bool> HasOwnProperty(const Context& context, Slot object, Slot key) { return std::nullopt; }
std::optional<bool> DeleteProperty(const Context& context, Slot object, Slot key) { return std::nullopt; }
std::optional<PropertyAttribute> GetPropertyAttributes(const Context& context, Slot object, Slot key) { return std::nullopt; }
std::optional<Slot> GetOwnPropertyNames(const Context& context, Slot object, KeyFilter filter) { return std::nullopt; }
std::optional<Slot> GetPrototype(const Context& context, Slot object) { return std::nullopt; }
std::optional<bool> SetPrototype(const Context& context, Slot object, Slot prototype) { return std::nullopt; }
std::uint32_t ArrayLength(Slot array) noexcept { return 0; }
NativeBox* GetNativeBox(Slot object) noexcept { return nullptr; }

bool InitObjectTypes(Isolate&, PyObject*) noexcept { return true; }

}  // namespace ub::detail
