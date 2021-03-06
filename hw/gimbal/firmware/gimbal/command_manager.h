// Copyright 2015 Josh Pieper, jjp@pobox.com.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "base/gsl/gsl-lite.h"

#include "async_types.h"
#include "pool_ptr.h"
#include "static_function.h"

class AsyncStream;
class AsyncWriteStream;
class LockManager;

class CommandManager {
 public:
  CommandManager(Pool&, AsyncStream&, LockManager&);
  ~CommandManager();

  CommandManager(const CommandManager&) = delete;

  struct Response {
    AsyncWriteStream* stream = nullptr;
    ErrorCallback callback;

    Response(AsyncWriteStream* stream, ErrorCallback callback)
        : stream(stream), callback(callback) {}
    Response() {}
  };
  typedef StaticFunction<
    void (const gsl::cstring_span&, const Response&)> CommandFunction;

  void Register(const gsl::cstring_span& name, CommandFunction);

  template <typename Handler>
  void RegisterHandler(const gsl::cstring_span& name,
                       Handler& handler) {
    Register(name, [handler=&handler](const gsl::cstring_span& args,
                                      const Response& response) {
               handler->Command(args, response);
             });
  }

  void AsyncStart(ErrorCallback);

  void Poll();

 private:
  class Impl;
  PoolPtr<Impl> impl_;
};
