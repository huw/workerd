#!/usr/bin/env node

const { createServer } = require('http');

const server = createServer((req, res) => {
  // The response from the fallback service must be a vaid JSON
  // serialization of a Worker::Module config.

  // The x-resolve-method tells us if the module was imported or required.
  console.log(req.headers['x-resolve-method']);

  // The req.url tells us what we are importing
  console.log(req.url);

  res.end(`{
  "name":"foo.js",
  "esModule":"export default 1;"
}`);
});

server.listen(8888, () => {
  console.log('ready...');
});
