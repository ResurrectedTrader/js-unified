// Compiling a header is a weak check on a header full of templates: a template
// nobody instantiates is barely parsed. This forces the parts that carry the
// API's rules - the narrowing lattice, the class machinery, the typed callback
// data - to be instantiated, still with no engine in sight.
//
// Nothing here is linked and nothing here runs; the backend's definitions are
// deliberately absent.

#include <array>
#include <memory>

#include "unibind/unibind.h"

namespace {

struct Native {
    int value = 0;
};

struct EmbedderState {
    int calls = 0;
};

void Callback(const ub::CallbackInfo& info) {
    if (auto* state = info.Data<EmbedderState>()) {
        ++state->calls;
    }
    // The value data sits beside the typed pointer, so both have to resolve.
    if (info.Data().IsString()) {
        info.GetReturnValue().Set(info.Data());
        return;
    }
    info.GetReturnValue().Set(42);
}

std::unique_ptr<Native> Construct(const ub::CallbackInfo& /*info*/) {
    return std::make_unique<Native>();
}

void Increment(Native& self, const ub::CallbackInfo& info) {
    ++self.value;
    info.GetReturnValue().Set(self.value);
}

void ReadValue(Native& self, const ub::PropertyCallbackInfo& info) {
    info.GetReturnValue().Set(self.value);
}

void WriteValue(Native& self, const ub::Local<ub::Value>& value, const ub::PropertyCallbackInfo& info) {
    if (auto number = value.ToInt32(info.GetContext())) {
        self.value = *number;
    }
}

ub::Intercepted Intercept(const ub::Local<ub::Name>& /*name*/, const ub::PropertyCallbackInfo& /*info*/) {
    return ub::Intercepted::No;
}

[[maybe_unused]] void Surface(ub::Isolate& isolate, const ub::Context& context) {
    ub::EscapableHandleScope scope(isolate);
    ub::TryCatch tryCatch(isolate);

    auto object = ub::Object::New(context);
    auto array = ub::Array::New(context, 3);
    auto string = ub::String::New(isolate, "x");
    (void)ub::String::NewFromUtf8(isolate, "\xC3");
    auto symbol = ub::Symbol::WellKnown(isolate, ub::WellKnownSymbol::Iterator);
    auto number = ub::Number::New(isolate, 1.0);
    (void)symbol;
    (void)ub::Undefined(isolate);
    (void)ub::Null(isolate);

    // Widening is implicit, narrowing is checked.
    ub::Local<ub::Value> value = number;
    (void)value.Is<ub::Number>();
    (void)value.To<ub::String>();
    (void)value.Kind();

    if (object && string) {
        (void)object->Set(context, "key", *string);
        (void)object->Get(context, "key");
        (void)object->Get(context, 0U);
        (void)object->Has(context, *string);
        (void)object->Delete(context, *string);
        (void)object->DefineOwnProperty(context, *string, *string, ub::PropertyAttribute::ReadOnly);
        (void)object->GetOwnPropertyNames(context, {.includeNonEnumerable = true, .includeSymbols = true});
        (void)object->GetPrototype(context);
        (void)scope.Escape(*object);
    }
    if (array) {
        (void)array->Length();
    }

    EmbedderState state;
    auto function = ub::Function::New(context, &Callback, ub::CallbackData::For(state));
    if (function) {
        std::array<ub::Local<ub::Value>, 1> arguments{value};
        (void)function->Call(context, context.GlobalObject(), arguments);
        (void)function->NewInstance(context, arguments);
    }
    if (string) {
        (void)ub::Function::New(context, &Callback, *string);
    }
    (void)ub::Function::New(context, &Callback, {});

    // The view lattice: a typed array is a view, and so is a DataView.
    if (auto buffer = ub::ArrayBuffer::New(context, 8)) {
        if (auto view = ub::DataView::New(context, *buffer, 2, 4)) {
            std::array<std::byte, 4> bytes{};
            (void)ub::ByteLength(*view);
            (void)ub::ByteOffset(*view);
            (void)ub::GetBuffer(context, *view);
            (void)ub::CopyBytes(*view, bytes);
        }
        if (auto typed = ub::TypedArray::New(context, ub::ElementType::Uint8, *buffer, 0, 8)) {
            const ub::Local<ub::ArrayBufferView> asView = *typed;
            (void)ub::ByteOffset(*typed);
            (void)ub::ByteLength(asView);
            (void)ub::GetBuffer(context, *typed);
        }
    }
    (void)value.IsArrayBufferView();
    (void)value.IsDataView();
    (void)value.To<ub::ArrayBufferView>();
    (void)value.To<ub::DataView>();

    auto objectTemplate = ub::ObjectTemplate::New(isolate);
    objectTemplate.Set("answer", ub::Constant(42));
    objectTemplate.Set("call", &Callback, ub::CallbackData::For(state));
    objectTemplate.Set(ub::WellKnownSymbol::Iterator, &Callback);
    objectTemplate.SetAccessor("named", nullptr, nullptr);
    objectTemplate.SetHandler(ub::NamedPropertyHandler{.getter = &Intercept});
    objectTemplate.SetHandler(ub::IndexedPropertyHandler{});
    (void)objectTemplate.NewInstance(context);

    auto functionTemplate = ub::FunctionTemplate::New(isolate, &Callback);
    functionTemplate.SetClassName("Thing");
    functionTemplate.PrototypeTemplate().Set("method", &Callback);
    functionTemplate.InstanceTemplate().Set("field", ub::Constant(true));
    (void)functionTemplate.GetFunction(context);
    (void)functionTemplate.HasInstance(context, value);

    auto cls = ub::Class<Native>::New(isolate, "Native");
    cls.Construct<&Construct>()
        .Method<&Increment>("increment")
        .SymbolMethod<&Increment>(ub::WellKnownSymbol::Iterator)
        .Accessor<&ReadValue, &WriteValue>("value")
        .StaticMethod("make", &Callback)
        .StaticValue("kind", ub::Constant("native"));
    (void)cls.GetConstructor(context);
    (void)cls.Wrap(context, std::make_unique<Native>());
    (void)cls.IsInstance(value);
    (void)ub::Class<Native>::Unwrap(value);

    ub::Global<ub::Object> global;
    if (object) {
        global = ub::Global<ub::Object>(isolate, *object);
    }
    (void)global.Get(isolate);

    (void)ub::Evaluate(context, "1 + 1", {.resourceName = "test.js"});
    auto script = ub::Script::Compile(context, "1 + 1", {}, ub::CompileOptions::EagerCompile);
    if (script) {
        (void)script->Run(context);
    }

    (void)tryCatch.HasCaught();
    (void)tryCatch.Message(context);
    (void)tryCatch.StackTrace(context);
    (void)tryCatch.Exception();
    isolate.ThrowError(ub::ErrorKind::TypeError, "no");
    isolate.SetEmbedderData(state);
    (void)isolate.GetEmbedderData<EmbedderState>();
}

}  // namespace
