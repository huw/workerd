// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/worker-rpc.h>
#include <workerd/io/features.h>
#include <workerd/api/global-scope.h>

namespace workerd::api {

namespace {
kj::Array<kj::byte> serializeV8(jsg::Lock& js, jsg::JsValue value) {
  jsg::Serializer serializer(js, jsg::Serializer::Options {
    .version = 15,
    .omitHeader = false,
  });
  serializer.write(js, value);
  return serializer.release().data;
}

jsg::JsValue deserializeV8(jsg::Lock& js, kj::ArrayPtr<const kj::byte> ser) {
  jsg::Deserializer deserializer(js, ser, kj::none, kj::none,
      jsg::Deserializer::Options {
    .version = 15,
    .readHeader = true,
  });

  return deserializer.readValue(js);
}

// Deserializes the RPC response.
jsg::Value handleRpcResponse(jsg::Lock& js, capnp::Response<rpc::CallResults> result) {
  switch(result.which()) {
    case rpc::CallResults::EXCEPTION: {
      js.throwException(deserializeV8(js, result.getException().getV8Serialized().asBytes()));
    }
    case rpc::CallResults::VALUE: {
      return jsg::Value(js.v8Isolate, deserializeV8(js,
          result.getValue().getV8Serialized().asBytes()));
    }
  }
}
} // namespace

kj::Promise<capnp::Response<rpc::CallResults>> WorkerRpc::sendWorkerRpc(
    jsg::Lock& js,
    kj::StringPtr name,
    const v8::FunctionCallbackInfo<v8::Value>& args) {

  auto& ioContext = IoContext::current();
  auto worker = getClient(ioContext, kj::none, "getJsRpcTarget"_kjc);
  auto event = kj::heap<api::GetJsRpcTargetCustomEventImpl>(WORKER_RPC_EVENT_TYPE);

  rpc::JsRpcTarget::Client client = event->getCap();
  auto builder = client.callRequest();
  builder.setMethodName(name);

  kj::Vector<jsg::JsValue> argv(args.Length());
  for (int n = 0; n < args.Length(); n++) {
    argv.add(jsg::JsValue(args[n]));
  }

  // If we have arguments, serialize them.
  // Note that we may fail to serialize some element, in which case this will throw back to JS.
  if (argv.size() > 0) {
    auto ser = serializeV8(js, js.arr(argv.asPtr()));
    builder.initSerializedArgs().setV8Serialized(kj::mv(ser));
  }

  auto callResult = builder.send();
  auto customEventResult = worker->customEvent(kj::mv(event));

  co_return co_await callResult.attach(kj::mv(customEventResult));
}

kj::Maybe<jsg::JsValue> WorkerRpc::getNamed(jsg::Lock& js, kj::StringPtr name) {
  // Named intercept is enabled, this means we won't default to legacy behavior.
  // The return value of the function is a promise that resolves once the remote returns the result
  // of the RPC call.
  return jsg::JsValue(js.wrapReturningFunction(js.v8Context(), [this, methodName=kj::str(name)]
      (jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) {
        auto& ioContext = IoContext::current();
        // Wait for the RPC to resolve and then process the result.
        return js.wrapSimplePromise(ioContext.awaitIo(js, sendWorkerRpc(js, methodName, args),
            [](jsg::Lock& js, capnp::Response<rpc::CallResults> result) -> jsg::Value {
          return handleRpcResponse(js, kj::mv(result));
        }));
      }
  ));
}

// The capability that lets us call remote methods over RPC.
// The client capability is dropped after each callRequest().
class JsRpcTargetImpl final : public rpc::JsRpcTarget::Server {
public:
  JsRpcTargetImpl(
      kj::Own<kj::PromiseFulfiller<void>> callFulfiller,
      IoContext& ctx,
      kj::Maybe<kj::StringPtr> entrypointName)
      : callFulfiller(kj::mv(callFulfiller)), ctx(ctx), entrypointName(entrypointName) {}

  // Handles the delivery of JS RPC method calls.
  kj::Promise<void> call(CallContext callContext) override {
    auto methodName = kj::heapString(callContext.getParams().getMethodName());
    auto serializedArgs = callContext.getParams().getSerializedArgs().getV8Serialized().asBytes();

    // Try to execute the requested method.
    return ctx.run(
        [this, methodName=kj::mv(methodName), serializedArgs=kj::mv(serializedArgs),
          entrypointName=entrypointName, callContext] (Worker::Lock& lock) mutable ->
            kj::Promise<void> {

      jsg::Lock& js = lock;
      if (!FeatureFlags::get(js).getJsRpc()) {
        // JS RPC is not enabled on the server side, we cannot call any methods.
        auto rpcDisabled =
            "The receiving Worker does not allow its methods to be called over RPC."_kjc;
        JSG_FAIL_REQUIRE(Error, rpcDisabled);
      }

      // We will try to get the function, if we can't we'll throw an error to the client.
      auto fn = tryGetFn(lock, entrypointName, ctx, methodName);

      // We have a function, so let's call it and serialize the result for RPC.
      // We account for both the JS method throwing and our serialization attempt failing.
      // If the function returns a promise we will wait for the promise to finish so we can
      // serialize the result.
      // TODO(now): Do we need to use awaitJs here? `call()` has to return a kj::Promise, so maybe
      // we can return some other promise that resolves when this jsg::Promise does?
      return ctx.awaitJs(js, callFuncAndSerialize(js, fn, serializedArgs).then(js,
          [callContext](jsg::Lock& js, SerializedResult result) mutable {
        KJ_SWITCH_ONEOF(result) {
          KJ_CASE_ONEOF(ok, Returned) {
            auto builder = callContext.initResults(capnp::MessageSize {
                ok.serializedValue.size() / 8 + 8, 0 });
            builder.initValue().setV8Serialized(kj::mv(ok.serializedValue));
          }
          KJ_CASE_ONEOF(err, ThrewException) {
            auto builder = callContext.initResults(capnp::MessageSize {
                err.serializedException.size() / 8 + 8, 0 });
            builder.initException().setV8Serialized(kj::mv(err.serializedException));
          }
        }
      }));
    }).then([callFulfiller = kj::mv(callFulfiller)]() mutable {
      // We want to fulfill the callPromise so customEvent can continue executing.
      callFulfiller->fulfill();
    });
  }

  KJ_DISALLOW_COPY_AND_MOVE(JsRpcTargetImpl);

private:
  // Prevents us from calling a function defined on the Object Prototype, or a registered method.
  bool verifyCallable(jsg::Lock& js, v8::Local<v8::Object> workerObj, kj::StringPtr name) {
    v8::Local<v8::Object> obj = js.obj();
    auto objProto = obj->GetPrototype().As<v8::Object>();

    // Get() checks the Object and the prototype chain.
    // This check tells us if the function we intend to call is the one on the Object prototype,
    auto nameStr = js.strExtern(name);
    if (jsg::check(workerObj->Get(js.v8Context(), nameStr)) ==
        jsg::check(objProto->Get(js.v8Context(), nameStr))) {
      return false;
    }
    return isRegisteredName(name);
  }

  // The following names are registered by the Workers Runtime and cannot be called over RPC.
  bool isRegisteredName(kj::StringPtr name) {
    if (name == "fetch"_kjc ||
        name == "connect"_kjc ||
        name == "alarm"_kjc ||
        name == "webSocketMessage"_kjc ||
        name == "webSocketClose"_kjc ||
        name == "webSocketError"_kjc) {
      return true;
    }
    return false;
  }

  // If the `methodName` is a known public method, we'll return it.
  inline v8::Local<v8::Function> tryGetFn(
      Worker::Lock& lock,
      kj::Maybe<kj::StringPtr> entrypointName,
      IoContext& ctx,
      kj::StringPtr methodName) {

    auto& handler = KJ_REQUIRE_NONNULL(lock.getExportedHandler(entrypointName, ctx.getActor()),
        "Failed to get handler to worker.");

    auto handle = handler.self.getHandle(lock);
    if (verifyCallable(lock, handle, methodName)) {
      JSG_FAIL_REQUIRE(Error,
          kj::str("'", methodName, "' is a registered method and cannot be called over RPC."_kj));
    }
    auto methodStr = jsg::v8StrIntern(lock.getIsolate(), methodName);
    auto fnHandle = jsg::check(handle->Get(lock.getContext(), methodStr));

    // Get() doesn't handle private properties so we have to get this separately.
    auto priv = v8::Private::New(lock.getIsolate(), methodStr);
    auto privHandle = jsg::check(handle->GetPrivate(lock.getContext(), priv));

    // TODO(someday): The interceptor on the client side doesn't support private property
    // interception, so the check is essentially dead code today. We'll check anyways to guard
    // against that changing in the future.
    if (!fnHandle->IsFunction() || (!privHandle.IsEmpty() && privHandle->IsPrivate())) {
      // We can't return this because it's not a function, or if it is, it's private.
      JSG_FAIL_REQUIRE(Error,
          "The RPC receiver does not implement the requested method."_kjc);
    }
    return fnHandle.As<v8::Function>();
  }

  // Deserializes the arguments and passes them to the given function.
  v8::Local<v8::Value> invokeFn(
      jsg::Lock& js,
      v8::Local<v8::Function> fn,
      kj::ArrayPtr<const kj::byte> serializedArgs) {
    // We received arguments from the client, deserialize them back to JS.
    if (serializedArgs.size() > 0) {
      auto args = KJ_REQUIRE_NONNULL(
          deserializeV8(js, serializedArgs).tryCast<jsg::JsArray>(),
          "expected JsArray when deserializing arguments.");
      // Call() expects a `Local<Value> []`... so we populate an array.
      auto arguments = kj::heapArray<v8::Local<v8::Value>>(args.size());
      for (size_t i = 0; i < args.size(); ++i) {
        arguments[i] = args.get(js, i);
      }
      return jsg::check(fn->Call(js.v8Context(), fn, args.size(), arguments.begin()));
    } else {
      return jsg::check(fn->Call(js.v8Context(), fn, 0, nullptr));
    }
  };

  struct Returned {
    kj::Array<kj::byte> serializedValue;
  };
  struct ThrewException {
    kj::Array<kj::byte> serializedException;
  };
  // The serialized JS value.
  using SerializedResult = kj::OneOf<Returned, ThrewException>;

  // Invokes the given function and serializes the result for RPC.
  jsg::Promise<SerializedResult> callFuncAndSerialize(
      jsg::Lock& js,
      v8::Local<v8::Function> fn,
      kj::ArrayPtr<const kj::byte> serializedArgs) {

    // `processAsyncResult` expects a promise, so even if we return synchronously we want to wrap
    // the result of calling the JS function in a promise.
    auto wrapValInPromise = [](jsg::Lock& js, v8::Local<v8::Value> value) {
      auto resolver = jsg::check(v8::Promise::Resolver::New(js.v8Context()));
      auto promise = resolver->GetPromise();

      jsg::check(resolver->Resolve(js.v8Context(), value));
      return js.toPromise(kj::mv(promise));
    };

    kj::Maybe<jsg::Promise<jsg::Value>> eventualResult;
    bool caughtException = false;
    js.tryCatch([&] {
      eventualResult = wrapValInPromise(js, invokeFn(js, fn, serializedArgs));
    }, [&](jsg::Value exception) {
      // We threw while executing JS.
      caughtException = true;
      eventualResult = wrapValInPromise(js, exception.getHandle(js));
    });

    auto& result = KJ_REQUIRE_NONNULL(eventualResult);
    return processAsyncResult(js, kj::mv(result)).then(js,
        [caughtException](jsg::Lock& js, SerializedResult serializedResult) -> SerializedResult {
          if (caughtException) {
            KJ_IF_SOME(ser, serializedResult.tryGet<Returned>()) {
              // Since we threw while executing the function, we still want to throw the
              // serialized result even though serialization was successful.
              return ThrewException { .serializedException = kj::mv(ser.serializedValue) };
            }
          }
          return kj::mv(serializedResult);
        });
  }

  // The JS method returned a promise and we need to wait for the promise to resolve or reject
  // before serializing the result.
  jsg::Promise<SerializedResult> processAsyncResult(
      jsg::Lock& js,
      jsg::Promise<jsg::Value> promise) {
    return promise.then(js, [] (jsg::Lock& lock, jsg::Value value) -> SerializedResult {
      auto jsVal = jsg::JsValue(value.getHandle(lock));
      return Returned { .serializedValue = serializeV8(lock, jsVal) };
    }).catch_(js, [](jsg::Lock& lock, jsg::Value exception) -> SerializedResult {
      auto jsVal = jsg::JsValue(exception.getHandle(lock));
      return ThrewException { .serializedException = serializeV8(lock, jsVal) };
    });
  }

  // We use the callFulfiller to let the custom event know we've finished executing the method.
  kj::Own<kj::PromiseFulfiller<void>> callFulfiller;
  IoContext& ctx;
  kj::Maybe<kj::StringPtr> entrypointName;
};

GetJsRpcTargetEvent::GetJsRpcTargetEvent()
    : ExtendableEvent("getJsRpcTarget") {};

kj::Promise<WorkerInterface::CustomEvent::Result> GetJsRpcTargetCustomEventImpl::run(
    kj::Own<IoContext::IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName) {
  incomingRequest->delivered();
  auto [callPromise, callFulfiller] = kj::newPromiseAndFulfiller<void>();
  capFulfiller->fulfill(kj::heap<JsRpcTargetImpl>(
      kj::mv(callFulfiller), incomingRequest->getContext(), entrypointName));

  // `callPromise` resolves once `JsRpcTargetImpl::call()` (invoked by client) completes.
  co_await callPromise;
  co_await incomingRequest->drain();
  co_return WorkerInterface::CustomEvent::Result {
    .outcome = EventOutcome::OK
  };
}

kj::Promise<WorkerInterface::CustomEvent::Result>
  GetJsRpcTargetCustomEventImpl::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    kj::TaskSet& waitUntilTasks,
    rpc::EventDispatcher::Client dispatcher) {
  auto req = dispatcher.getJsRpcTargetRequest();
  auto sent = req.send();
  this->capFulfiller->fulfill(sent.getServer());

  auto resp = co_await sent;
  co_return WorkerInterface::CustomEvent::Result {
    .outcome = EventOutcome::OK
  };
}
}; // namespace workerd::api
