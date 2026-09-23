// STUB: generated placeholders for the data area; replaced by the real implementation.
#include "internal.h"

namespace ub::detail {

std::optional<Slot> MakeArrayBuffer(const Context& context, std::span<const std::byte> bytes, std::size_t byteLength) { return std::nullopt; }
std::size_t ArrayBufferByteLength(Slot buffer) noexcept { return 0; }
std::size_t ArrayBufferCopyOut(Slot buffer, std::span<std::byte> out) noexcept { return 0; }
std::optional<Slot> MakeTypedArray(const Context& context, ElementType type, Slot buffer, std::size_t byteOffset, std::size_t length) { return std::nullopt; }
ElementType TypedArrayElementType(Slot view) noexcept { return ElementType::Uint8; }
std::size_t TypedArrayLength(Slot view) noexcept { return 0; }
std::size_t TypedArrayByteOffset(Slot view) noexcept { return 0; }
std::optional<Slot> TypedArrayBuffer(const Context& context, Slot view) { return std::nullopt; }
std::size_t TypedArrayCopyOut(Slot view, std::span<std::byte> out) noexcept { return 0; }
std::size_t ArrayBufferViewByteLength(Slot view) noexcept { return 0; }
std::size_t ArrayBufferViewByteOffset(Slot view) noexcept { return 0; }
std::optional<Slot> ArrayBufferViewBuffer(const Context& context, Slot view) { return std::nullopt; }
std::size_t ArrayBufferViewCopyOut(Slot view, std::span<std::byte> out) noexcept { return 0; }
std::optional<Slot> MakeDataView(const Context& context, Slot buffer, std::size_t byteOffset, std::size_t byteLength) { return std::nullopt; }
std::optional<std::vector<std::uint8_t>> SerializeValue(const Context& context, Slot value) { return std::nullopt; }
std::optional<Slot> DeserializeValue(const Context& context, std::span<const std::uint8_t> blob) { return std::nullopt; }
ScriptRec* CompileScriptWithCache(const Context& context, std::string_view source, const ScriptOrigin& origin, std::span<const std::uint8_t> codeCache, CompileOptions options) { return nullptr; }
bool ScriptUsedCodeCache(const ScriptRec* script) noexcept { return false; }
std::optional<std::vector<std::uint8_t>> ScriptCreateCodeCache(const ScriptRec* script) { return std::nullopt; }

struct DataState {};
void DestroyDataState(DataState* state) noexcept { delete state; }
bool InitDataTypes(Isolate&, PyObject*) noexcept { return true; }

}  // namespace ub::detail
