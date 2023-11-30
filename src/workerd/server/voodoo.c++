// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This server interacts directly with the GPU, and listens on a UNIX socket for clients
// of the Dawn Wire protocol.

#include <dawn/dawn_proc.h>
#include <dawn/native/DawnNative.h>
#include <dawn/webgpu_cpp.h>
#include <dawn/wire/WireServer.h>
#include <filesystem>
#include <kj/async-io.h>
#include <kj/debug.h>
#include <kj/main.h>
#include <unistd.h>

struct DawnRemoteSerializer : public dawn::wire::CommandSerializer {
  void* GetCmdSpace(size_t size) {
    KJ_UNIMPLEMENTED();
  }
  bool Flush() {
    KJ_UNIMPLEMENTED();
  };
  size_t GetMaximumAllocationSize() const {
    KJ_UNIMPLEMENTED();
  };
};

class VoodooMain : public kj::TaskSet::ErrorHandler {
public:
  VoodooMain(kj::ProcessContext& context)
      : context(context), tasks(*this), nativeProcs(dawn::native::GetProcs()) {}

  struct ServerContext {
    kj::Own<kj::AsyncIoStream> stream;

    ServerContext(kj::Own<kj::AsyncIoStream>&& stream, DawnProcTable* nativeProcs,
                  wgpu::Instance instance)
        : stream(kj::mv(stream)) {
      KJ_DBG("server created");

      // setup wire
      auto serializer = kj::heap<DawnRemoteSerializer>();
      dawn::wire::WireServerDescriptor wDesc{
          .serializer = serializer,
          .procs = nativeProcs,
      };

      // TODO: this calls GetMaximumAllocationSize() immediately which currently throws
      // but the "not implemented" message is lost.
      auto wireServer = kj::heap<dawn::wire::WireServer>(wDesc);

      wireServer->InjectInstance(instance.Get(), 1, 0);

      KJ_DBG("all done");
    }
  };

  void taskFailed(kj::Exception&& exception) override {
    kj::throwFatalException(kj::mv(exception));
  }

  kj::MainBuilder::Validity setListenPath(kj::StringPtr path) {
    listenPath = path;
    return true;
  }

  kj::MainBuilder::Validity startServer() {
    KJ_DBG(listenPath, "will start listening server");

    // initialize dawn
    dawnProcSetProcs(&nativeProcs);
    auto adapters = instance.EnumerateAdapters();
    KJ_REQUIRE(!adapters.empty(), "no GPU adapters found");

    // initialize event loop
    kj::AsyncIoContext io = kj::setupAsyncIo();

    // create listening socket
    unlink(listenPath.cStr());
    auto addr =
        io.provider->getNetwork().parseAddress(kj::str("unix:", listenPath)).wait(io.waitScope);

    auto listener = addr->listen();

    // process requests
    acceptLoop(kj::mv(listener));

    kj::NEVER_DONE.wait(io.waitScope);
  }

  kj::MainFunc getMain() {
    return kj::MainBuilder(context, "Foo Builder v1.5", "Reads <source>s and builds a Foo.")
        .expectArg("<listen_path>", KJ_BIND_METHOD(*this, setListenPath))
        .callAfterParsing(KJ_BIND_METHOD(*this, startServer))
        .build();
  }

  void acceptLoop(kj::Own<kj::ConnectionReceiver>&& listener) {
    auto ptr = listener.get();
    tasks.add(ptr->accept().then(
        [this, listener = kj::mv(listener)](kj::Own<kj::AsyncIoStream>&& connection) mutable {
          acceptLoop(kj::mv(listener));

          auto server = kj::heap<ServerContext>(kj::mv(connection), &nativeProcs, instance.Get());

          tasks.add(server->stream->whenWriteDisconnected().attach(kj::mv(server)));
        }));
  }

private:
  kj::StringPtr listenPath;
  kj::ProcessContext& context;
  kj::TaskSet tasks;
  DawnProcTable nativeProcs;
  dawn::native::Instance instance;
};

KJ_MAIN(VoodooMain)
