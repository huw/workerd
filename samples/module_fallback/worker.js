// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as bar from 'foo';

// The fallback service should also work with modules that are
// not specified within the worker bundle at all.
import * as baz from 'baz';

import * as vm from 'node:vm';

console.log(bar, baz, vm);

export default {
  async fetch(req, env) {
    return new Response("Hello World\n");
  }
};
