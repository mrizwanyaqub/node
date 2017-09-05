#include "node_messaging.h"
#include "node_internals.h"
#include "node_buffer.h"
#include "node_errors.h"
#include "util.h"
#include "util-inl.h"
#include "async_wrap.h"
#include "async_wrap-inl.h"

using v8::Array;
using v8::ArrayBuffer;
using v8::ArrayBufferCreationMode;
using v8::Context;
using v8::EscapableHandleScope;
using v8::Exception;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Isolate;
using v8::Just;
using v8::Local;
using v8::Maybe;
using v8::MaybeLocal;
using v8::Nothing;
using v8::Object;
using v8::String;
using v8::Value;
using v8::ValueDeserializer;
using v8::ValueSerializer;

namespace node {
namespace worker {

Message::Message(MallocedBuffer<char>&& buffer)
    : main_message_buf_(std::move(buffer)) {}

namespace {

// This is used to tell V8 how to read transferred host objects, like other
// `MessagePort`s and `SharedArrayBuffer`s, and make new JS objects out of them.
class DeserializerDelegate : public ValueDeserializer::Delegate {
 public:
  DeserializerDelegate(Message* m, Environment* env)
    : env_(env), msg_(m) {}

  ValueDeserializer* deserializer = nullptr;

 private:
  Environment* env_;
  Message* msg_;
};

}  // anonymous namespace

MaybeLocal<Value> Message::Deserialize(Environment* env,
                                       Local<Context> context) {
  EscapableHandleScope handle_scope(env->isolate());
  Context::Scope context_scope(context);

  DeserializerDelegate delegate(this, env);
  ValueDeserializer deserializer(
      env->isolate(),
      reinterpret_cast<const uint8_t*>(main_message_buf_.data),
      main_message_buf_.size,
      &delegate);
  delegate.deserializer = &deserializer;

  // Attach all transfered ArrayBuffers to their new Isolate.
  for (uint32_t i = 0; i < array_buffer_contents_.size(); ++i) {
    Local<ArrayBuffer> ab =
        ArrayBuffer::New(env->isolate(),
                         array_buffer_contents_[i].release(),
                         array_buffer_contents_[i].size,
                         ArrayBufferCreationMode::kInternalized);
    deserializer.TransferArrayBuffer(i, ab);
  }
  array_buffer_contents_.clear();

  if (deserializer.ReadHeader(context).IsNothing())
    return MaybeLocal<Value>();
  return handle_scope.Escape(
      deserializer.ReadValue(context).FromMaybe(Local<Value>()));
}

namespace {

// This tells V8 how to serialize objects that it does not understand
// (e.g. C++ objects) into the output buffer, in a way that our own
// DeserializerDelegate understands how to unpack.
class SerializerDelegate : public ValueSerializer::Delegate {
 public:
  SerializerDelegate(Environment* env, Local<Context> context, Message* m)
      : env_(env), context_(context), msg_(m) {}

  void ThrowDataCloneError(Local<String> message) override {
    env_->isolate()->ThrowException(Exception::Error(message));
  }

  ValueSerializer* serializer = nullptr;

 private:
  Environment* env_;
  Local<Context> context_;
  Message* msg_;

  friend class worker::Message;
};

}  // anynomous namespace

Maybe<bool> Message::Serialize(Environment* env,
                               Local<Context> context,
                               Local<Value> input,
                               Local<Value> transfer_list_v) {
  HandleScope handle_scope(env->isolate());
  Context::Scope context_scope(context);

  // Verify that we're not silently overwriting an existing message.
  CHECK(main_message_buf_.is_empty());

  SerializerDelegate delegate(env, context, this);
  ValueSerializer serializer(env->isolate(), &delegate);
  delegate.serializer = &serializer;

  std::vector<Local<ArrayBuffer>> array_buffers;
  if (transfer_list_v->IsArray()) {
    Local<Array> transfer_list = transfer_list_v.As<Array>();
    uint32_t length = transfer_list->Length();
    for (uint32_t i = 0; i < length; ++i) {
      Local<Value> entry;
      if (!transfer_list->Get(context, i).ToLocal(&entry))
        return Nothing<bool>();
      // Currently, we support ArrayBuffers.
      if (entry->IsArrayBuffer()) {
        Local<ArrayBuffer> ab = entry.As<ArrayBuffer>();
        // If we cannot render the ArrayBuffer unusable in this Isolate and
        // take ownership of its memory, copying the buffer will have to do.
        if (!ab->IsNeuterable() || ab->IsExternal())
          continue;
        // We simply use the array index in the `array_buffers` list as the
        // ID that we write into the serialized buffer.
        uint32_t id = array_buffers.size();
        array_buffers.push_back(ab);
        serializer.TransferArrayBuffer(id, ab);
        continue;
      }

      THROW_ERR_INVALID_TRANSFER_OBJECT(env);
      return Nothing<bool>();
    }
  }

  serializer.WriteHeader();
  if (serializer.WriteValue(context, input).IsNothing()) {
    return Nothing<bool>();
  }

  for (Local<ArrayBuffer> ab : array_buffers) {
    // If serialization succeeded, we want to take ownership of
    // (a.k.a. externalize) the underlying memory region and render
    // it inaccessible in this Isolate.
    ArrayBuffer::Contents contents = ab->Externalize();
    ab->Neuter();
    array_buffer_contents_.push_back(
        MallocedBuffer<char> { static_cast<char*>(contents.Data()),
                               contents.ByteLength() });
  }

  // The serializer gave us a buffer allocated using `malloc()`.
  std::pair<uint8_t*, size_t> data = serializer.Release();
  main_message_buf_ =
      MallocedBuffer<char>(reinterpret_cast<char*>(data.first), data.second);
  return Just(true);
}

MessagePortData::MessagePortData(MessagePort* owner) : owner_(owner) { }

MessagePortData::~MessagePortData() {
  CHECK_EQ(owner_, nullptr);
  Disentangle();
}

void MessagePortData::AddToIncomingQueue(Message&& message) {
  // This function will be called by other threads.
  Mutex::ScopedLock lock(mutex_);
  incoming_messages_.emplace_back(std::move(message));

  if (owner_ != nullptr)
    owner_->TriggerAsync();
}

bool MessagePortData::IsSiblingClosed() const {
  Mutex::ScopedLock lock(*sibling_mutex_);
  return sibling_ == nullptr;
}

void MessagePortData::Entangle(MessagePortData* a, MessagePortData* b) {
  CHECK_EQ(a->sibling_, nullptr);
  CHECK_EQ(b->sibling_, nullptr);
  a->sibling_ = b;
  b->sibling_ = a;
  a->sibling_mutex_ = b->sibling_mutex_;
}

void MessagePortData::PingOwnerAfterDisentanglement() {
  Mutex::ScopedLock lock(mutex_);
  if (owner_ != nullptr)
    owner_->TriggerAsync();
}

void MessagePortData::Disentangle() {
  // Grab a copy of the sibling mutex, then replace it so that each sibling
  // has its own sibling_mutex_ now.
  std::shared_ptr<Mutex> sibling_mutex = sibling_mutex_;
  Mutex::ScopedLock sibling_lock(*sibling_mutex);
  sibling_mutex_ = std::make_shared<Mutex>();

  MessagePortData* sibling = sibling_;
  if (sibling_ != nullptr) {
    sibling_->sibling_ = nullptr;
    sibling_ = nullptr;
  }

  // We close MessagePorts after disentanglement, so we trigger the
  // corresponding uv_async_t to let them know that this happened.
  PingOwnerAfterDisentanglement();
  if (sibling != nullptr) {
    sibling->PingOwnerAfterDisentanglement();
  }
}

MessagePort::~MessagePort() {
  if (data_)
    data_->owner_ = nullptr;
}

MessagePort::MessagePort(Environment* env,
                         Local<Context> context,
                         Local<Object> wrap)
  : HandleWrap(env,
               wrap,
               reinterpret_cast<uv_handle_t*>(new uv_async_t()),
               AsyncWrap::PROVIDER_MESSAGEPORT),
    data_(new MessagePortData(this)) {
  auto onmessage = [](uv_async_t* handle) {
    // Called when data has been put into the queue.
    MessagePort* channel = static_cast<MessagePort*>(handle->data);
    channel->OnMessage();
  };
  CHECK_EQ(uv_async_init(env->event_loop(),
                         async(),
                         onmessage), 0);
  async()->data = static_cast<void*>(this);

  Local<Value> fn;
  if (!wrap->Get(context, env->oninit_string()).ToLocal(&fn))
    return;

  if (fn->IsFunction()) {
    Local<Function> init = fn.As<Function>();
    USE(init->Call(context, wrap, 0, nullptr));
  }
}

void MessagePort::AddToIncomingQueue(Message&& message) {
  data_->AddToIncomingQueue(std::move(message));
}

uv_async_t* MessagePort::async() {
  return reinterpret_cast<uv_async_t*>(GetHandle());
}

void MessagePort::TriggerAsync() {
  CHECK_EQ(uv_async_send(async()), 0);
}

void MessagePort::New(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  if (!args.IsConstructCall()) {
    THROW_ERR_CONSTRUCT_CALL_REQUIRED(env);
    return;
  }

  Local<Context> context = args.This()->CreationContext();
  Context::Scope context_scope(context);

  new MessagePort(env, context, args.This());
}

MessagePort* MessagePort::New(
    Environment* env,
    Local<Context> context,
    std::unique_ptr<MessagePortData> data) {
  Context::Scope context_scope(context);
  Local<Function> ctor;
  if (!GetMessagePortConstructor(env, context).ToLocal(&ctor))
    return nullptr;
  MessagePort* port = nullptr;

  // Construct a new instance, then assign the listener instance and possibly
  // the MessagePortData to it.
  Local<Object> instance;
  if (!ctor->NewInstance(context).ToLocal(&instance))
    return nullptr;
  ASSIGN_OR_RETURN_UNWRAP(&port, instance, nullptr);
  if (data) {
    port->Detach();
    port->data_ = std::move(data);
    port->data_->owner_ = port;
    // If the existing MessagePortData object had pending messages, this is
    // the easiest way to run that queue.
    port->TriggerAsync();
  }
  return port;
}

void MessagePort::OnMessage() {
  HandleScope handle_scope(env()->isolate());
  Local<Context> context = object()->CreationContext();

  // data_ can only ever be modified by the owner thread, so no need to lock.
  // However, the message port may be transferred while it is processing
  // messages, so we need to check that this handle still owns its `data_` field
  // on every iteration.
  while (data_) {
    Message received;
    {
      // Get the head of the message queue.
      Mutex::ScopedLock lock(data_->mutex_);
      if (!data_->receiving_messages_)
        break;
      if (data_->incoming_messages_.empty())
        break;
      received = std::move(data_->incoming_messages_.front());
      data_->incoming_messages_.pop_front();
    }

    if (!env()->can_call_into_js()) {
      // In this case there is nothing to do but to drain the current queue.
      continue;
    }

    {
      // Call the JS .onmessage() callback.
      HandleScope handle_scope(env()->isolate());
      Context::Scope context_scope(context);
      Local<Value> args[] = {
        received.Deserialize(env(), context).FromMaybe(Local<Value>())
      };

      if (args[0].IsEmpty() ||
          !object()->Has(context, env()->onmessage_string()).FromMaybe(false) ||
          MakeCallback(env()->onmessage_string(), 1, args).IsEmpty()) {
        // Re-schedule OnMessage() execution in case of failure.
        if (data_)
          TriggerAsync();
        return;
      }
    }
  }

  if (data_ && data_->IsSiblingClosed()) {
    Close();
  }
}

bool MessagePort::IsSiblingClosed() const {
  CHECK(data_);
  return data_->IsSiblingClosed();
}

void MessagePort::OnClose() {
  if (data_) {
    data_->owner_ = nullptr;
    data_->Disentangle();
  }
  data_.reset();
  delete async();
}

std::unique_ptr<MessagePortData> MessagePort::Detach() {
  Mutex::ScopedLock lock(data_->mutex_);
  data_->owner_ = nullptr;
  return std::move(data_);
}


void MessagePort::Send(Message&& message) {
  Mutex::ScopedLock lock(*data_->sibling_mutex_);
  if (data_->sibling_ == nullptr)
    return;
  data_->sibling_->AddToIncomingQueue(std::move(message));
}

void MessagePort::Send(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Message msg;
  if (msg.Serialize(env, object()->CreationContext(), args[0], args[1])
          .IsNothing()) {
    return;
  }
  Send(std::move(msg));
}

void MessagePort::PostMessage(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  MessagePort* port;
  ASSIGN_OR_RETURN_UNWRAP(&port, args.This());
  if (!port->data_) {
    return THROW_ERR_CLOSED_MESSAGE_PORT(env);
  }
  if (args.Length() == 0) {
    return THROW_ERR_MISSING_ARGS(env, "Not enough arguments to "
                                       "MessagePort.postMessage");
  }
  port->Send(args);
}

void MessagePort::Start() {
  Mutex::ScopedLock lock(data_->mutex_);
  data_->receiving_messages_ = true;
  if (!data_->incoming_messages_.empty())
    TriggerAsync();
}

void MessagePort::Stop() {
  Mutex::ScopedLock lock(data_->mutex_);
  data_->receiving_messages_ = false;
}

void MessagePort::Start(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  MessagePort* port;
  ASSIGN_OR_RETURN_UNWRAP(&port, args.This());
  if (!port->data_) {
    THROW_ERR_CLOSED_MESSAGE_PORT(env);
    return;
  }
  port->Start();
}

void MessagePort::Stop(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  MessagePort* port;
  ASSIGN_OR_RETURN_UNWRAP(&port, args.This());
  if (!port->data_) {
    THROW_ERR_CLOSED_MESSAGE_PORT(env);
    return;
  }
  port->Stop();
}

size_t MessagePort::self_size() const {
  Mutex::ScopedLock lock(data_->mutex_);
  size_t sz = sizeof(*this) + sizeof(*data_);
  for (const Message& msg : data_->incoming_messages_)
    sz += sizeof(msg) + msg.main_message_buf_.size;
  return sz;
}

void MessagePort::Entangle(MessagePort* a, MessagePort* b) {
  Entangle(a, b->data_.get());
}

void MessagePort::Entangle(MessagePort* a, MessagePortData* b) {
  MessagePortData::Entangle(a->data_.get(), b);
}

MaybeLocal<Function> GetMessagePortConstructor(
    Environment* env, Local<Context> context) {
  // Factor generating the MessagePort JS constructor into its own piece
  // of code, because it is needed early on in the child environment setup.
  Local<FunctionTemplate> templ = env->message_port_constructor_template();
  if (!templ.IsEmpty())
    return templ->GetFunction(context);

  {
    Local<FunctionTemplate> m = env->NewFunctionTemplate(MessagePort::New);
    m->SetClassName(env->message_port_constructor_string());
    m->InstanceTemplate()->SetInternalFieldCount(1);

    AsyncWrap::AddWrapMethods(env, m);

    env->SetProtoMethod(m, "postMessage", MessagePort::PostMessage);
    env->SetProtoMethod(m, "start", MessagePort::Start);
    env->SetProtoMethod(m, "stop", MessagePort::Stop);
    env->SetProtoMethod(m, "close", HandleWrap::Close);
    env->SetProtoMethod(m, "unref", HandleWrap::Unref);
    env->SetProtoMethod(m, "ref", HandleWrap::Ref);
    env->SetProtoMethod(m, "hasRef", HandleWrap::HasRef);

    env->set_message_port_constructor_template(m);
  }

  return GetMessagePortConstructor(env, context);
}

namespace {

static void MessageChannel(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  if (!args.IsConstructCall()) {
    THROW_ERR_CONSTRUCT_CALL_REQUIRED(env);
    return;
  }

  Local<Context> context = args.This()->CreationContext();
  Context::Scope context_scope(context);

  MessagePort* port1 = MessagePort::New(env, context);
  MessagePort* port2 = MessagePort::New(env, context);
  MessagePort::Entangle(port1, port2);

  args.This()->Set(env->context(), env->port1_string(), port1->object())
      .FromJust();
  args.This()->Set(env->context(), env->port2_string(), port2->object())
      .FromJust();
}

static void InitMessaging(Local<Object> target,
                          Local<Value> unused,
                          Local<Context> context,
                          void* priv) {
  Environment* env = Environment::GetCurrent(context);

  {
    Local<String> message_channel_string =
        FIXED_ONE_BYTE_STRING(env->isolate(), "MessageChannel");
    Local<FunctionTemplate> templ = env->NewFunctionTemplate(MessageChannel);
    templ->SetClassName(message_channel_string);
    target->Set(env->context(),
                message_channel_string,
                templ->GetFunction(context).ToLocalChecked()).FromJust();
  }

  target->Set(context,
              env->message_port_constructor_string(),
              GetMessagePortConstructor(env, context).ToLocalChecked())
                  .FromJust();
}

}  // anonymous namespace

}  // namespace worker
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(messaging, node::worker::InitMessaging)