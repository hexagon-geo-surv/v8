// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --experimental-wasm-type-reflection

new WebAssembly.Function({
    parameters: [],
    results: []
  }, x => x);
const long_variable = {
  toString: () => {
  }
};
