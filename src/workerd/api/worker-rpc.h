// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// Classes for calling a remote Worker/Durable Object's methods from the stub over RPC.
// This file contains the generic stub object (WorkerRpc), as well as classes for sending and
// delivering the RPC event.
//
// Upon invoking a method, the stub (WorkerRpc) obtains a capability (JsRpcTarget) by dispatching a
// `getJsRpcTarget` custom event. See worker-interface.capnp for the definition.
// The stub then uses the JsRpcTarget capability to send the serialized method name and arguments
// over RPC to the remote Worker/DO.

#include <workerd/api/http.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/function.h>
#include <workerd/api/basics.h>
#include <workerd/io/worker-interface.capnp.h>

namespace workerd::api {

// A WorkerRpc object forwards JS method calls to the remote Worker/Durable Object over RPC.
// Since methods are not known until runtime, WorkerRpc doesn't define any JS methods.
// Instead, we use JSG_NAMED_INTERCEPT to intercept property accesses of names that are not known at
// compile time.
//
// WorkerRpc only supports method calls. You cannot, for instance, access a property of a
// Durable Object over RPC.
class WorkerRpc : public Fetcher, public jsg::NamedIntercept {
public:
  WorkerRpc(
      IoOwn<OutgoingFactory> outgoingFactory,
      RequiresHostAndProtocol requiresHost,
      bool inHouse)
    : Fetcher(kj::mv(outgoingFactory), requiresHost, inHouse) {}

  // Serializes the method name and arguments, calls customEvent to get the capability, and uses
  // the capability to send our request to the remote Worker. This resolves once the RPC promise
  // resolves.
  kj::Promise<capnp::Response<rpc::CallResults>> sendWorkerRpc(jsg::Lock& js, kj::StringPtr name,
      const v8::FunctionCallbackInfo<v8::Value>& args);

  kj::Maybe<jsg::JsValue> getNamed(jsg::Lock& js, kj::StringPtr name) override;

  // WARNING: Adding a new JSG_METHOD to a class that extends WorkerRpc can conflict with RPC method
  // names defined on your remote target. For example, if you add a new method `bar()` to the
  // Durable Object stub, which extends WorkerRpc, then any scripts with a DO that defines `bar()`
  // and which call `stub.bar()` will stop calling the method over RPC, and start calling the method
  // you're adding to the Durable Object stub.
  //
  // This also applies to classes from which your final stub object is derived from. For example,
  // since the Durable Object stub extends WorkerRpc, and WorkerRpc extends Fetcher, any new
  // JSG_METHOD defined on Fetcher will override name interception on the Durable Object stub.
  //
  // New JSG_METHODs should be gated via compatibility flag/date and should be announced in the
  // change log.
  JSG_RESOURCE_TYPE(WorkerRpc, CompatibilityFlags::Reader flags) {
    if (flags.getWorkerdExperimental()) {
      JSG_NAMED_INTERCEPT();
    }
    JSG_INHERIT(Fetcher);
  }
private:
    // Event ID for WorkerRpc.
    //
    // Similar to WebSocket hibernation, we define this event ID in the internal codebase, but since
    // we don't create WorkerRpc stubs from our internal code, we can't pass the event type in --
    // so we hardcode it here.
    static constexpr uint16_t WORKER_RPC_EVENT_TYPE = 9;
};

class GetJsRpcTargetEvent final: public ExtendableEvent {
public:
  explicit GetJsRpcTargetEvent();

  JSG_RESOURCE_TYPE(GetJsRpcTargetEvent) {
    JSG_INHERIT(ExtendableEvent);
  }
};

// `getJsRpcTarget` returns a capability that provides the client a way to call remote methods
// over RPC. We drain the IncomingRequest after the capability is used to run the relevant JS.
class GetJsRpcTargetCustomEventImpl final: public WorkerInterface::CustomEvent {
public:
  GetJsRpcTargetCustomEventImpl(uint16_t typeId,
      kj::PromiseFulfillerPair<rpc::JsRpcTarget::Client> paf =
          kj::newPromiseAndFulfiller<rpc::JsRpcTarget::Client>())
    : capFulfiller(kj::mv(paf.fulfiller)),
      clientCap(kj::mv(paf.promise)),
      typeId(typeId) {}

  kj::Promise<Result> run(
      kj::Own<IoContext::IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName) override;

  kj::Promise<Result> sendRpc(
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      kj::TaskSet& waitUntilTasks,
      rpc::EventDispatcher::Client dispatcher) override;

  uint16_t getType() override {
    return typeId;
  }

  rpc::JsRpcTarget::Client getCap() { return clientCap; }

private:
  kj::Own<kj::PromiseFulfiller<workerd::rpc::JsRpcTarget::Client>> capFulfiller;

  // We need to set the client/server capability on the event itself to get around CustomEvent's
  // limited return type.
  rpc::JsRpcTarget::Client clientCap;
  uint16_t typeId;
};

#define EW_WORKER_RPC_ISOLATE_TYPES   \
  api::WorkerRpc,                     \
  api::GetJsRpcTargetEvent

}; // namespace workerd::api
