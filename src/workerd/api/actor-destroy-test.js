// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert'

export class DurableObjectExample {
  constructor(state, env) {
    this.state = state;
  }

  async fetch() {
    assert.ok(this.destroyCalled);
    return new Response("OK");
  }

  async destroy() {
    this.destroyCalled = true;
  }
}

export default {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName("A");
    let obj = env.ns.get(id);
    await env.ns.destroy(id);
    return await obj.fetch("http://foo/test");
  }
}
