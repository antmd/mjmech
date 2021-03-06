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

#include "telemetry_manager.h"

#include <cstdlib>

#include "lock_manager.h"
#include "named_registry.h"
#include "tokenizer.h"

class TelemetryManager::Impl {
 public:
  Impl(Pool& pool, LockManager& lock_manager)
      : pool_(pool),
        lock_manager_(lock_manager) {}

  struct Element {
    int rate = 0;
    int next = 1;
    bool to_send = false;
    bool text = false;
    AsyncWriteStream* stream = nullptr;
    SerializableHandlerBase* base = nullptr;
  };

  typedef NamedRegistryBase<Element, 16> NamedRegistry;

  void UpdateItem(NamedRegistry::Element* element) {
    // If we are in the mode where we emit on updates, mark this guy
    // as ready to update.  If we're not locked, then try to start
    // sending it out now.
    if (element->ptr->rate == 1) {
      element->ptr->to_send = true;
      maybe_need_to_send_ = true;
    }
  }

  void PollMillisecond() {
    // Iterate over all of our elements, updating their next counters.
    for (size_t i = 0; i < elements_.size(); i++) {
      if (elements_[i].name.size() == 0) { break; }
      auto ptr = elements_[i].ptr;
      if (ptr->rate > 1 && ptr->next > 0) {
        ptr->next--;
        if (ptr->next == 0) {
          ptr->to_send = true;
          ptr->next = ptr->rate;
          maybe_need_to_send_ = true;
        }
      }
    }
  }

  void Poll() {
    if (!maybe_need_to_send_) { return; }

    // Are we locked currently?  If so, then there's nothing we can
    // do.
    if (lock_manager_.locked(LockManager::kTelemetryManager)) { return; }

    // Look to see if something is good to send.
    for (size_t i = 0; i < elements_.size(); i++) {
      if (elements_[i].name.size() == 0) { break; }
      auto ptr = elements_[i].ptr;
      if (ptr->to_send) {
        ptr->to_send = false;
        // Start sending this guy out.
        LockManager::LockCallback work =
            [this, i](LockManager::ReleaseCallback release) {
          CommandManager::Response response(
              this->elements_[i].ptr->stream, release);
          this->EmitData(&this->elements_[i], response);
        };
        lock_manager_.Lock(LockManager::kTelemetryManager, work);
        return;
      }
    }

    maybe_need_to_send_ = false;
  }

  void Get(const gsl::cstring_span& name,
           const CommandManager::Response& response) {
    auto* const element =
        elements_.FindOrCreate(name, NamedRegistry::kFindOnly);
    if (!element) {
      WriteMessage(gsl::ensure_z("unknown name\r\n"), response);
      return;
    }

    EmitData(element, response);
  }

  void Enumerate(NamedRegistry::Element* element,
                 const CommandManager::Response& response) {
    current_response_ = response;

    element->ptr->base->Enumerate(
        &enumerate_context_,
        send_buffer_,
        element->name,
        *response.stream,
        [this](int err) {
          this->WriteOK(current_response_);
        });
  }

  void EmitData(NamedRegistry::Element* element,
                const CommandManager::Response& response) {
    if (element->ptr->text) {
      Enumerate(element, response);
    } else {
      Emit(
          gsl::ensure_z("emit "),
          element,
          [](NamedRegistry::Element* element, SimpleOStream* ostream) {
            element->ptr->base->WriteBinary(*ostream);
          },
          response);
    }
  }

  typedef StaticFunction<void (NamedRegistry::Element*,
                               SimpleOStream*)> WorkFunction;
  void Emit(const gsl::cstring_span& prefix,
            NamedRegistry::Element* element,
            WorkFunction work,
            const CommandManager::Response& response) {
    SimpleOStream ostream(send_buffer_, sizeof(send_buffer_));
    ostream.write(prefix);
    ostream.write(element->name);
    ostream.write(gsl::ensure_z("\r\n"));

    char* const size_position = send_buffer_ + ostream.position();
    ostream.skip(sizeof(uint32_t));

    work(element, &ostream);

    SimpleOStream size_stream(size_position, sizeof(uint32_t));
    mjmech::base::TelemetryWriteStream<SimpleOStream> tstream(size_stream);
    tstream.Write(static_cast<uint32_t>(
                      ostream.position() + send_buffer_ -
                      (size_position + sizeof(uint32_t))));

    AsyncWrite(
        *response.stream,
        gsl::cstring_span(send_buffer_, send_buffer_ + ostream.position()),
        response.callback);
  }

  void List(const CommandManager::Response& response) {
    current_response_ = response;
    current_list_index_ = 0;
    ListCallback(0);
  }

  void ListCallback(int error) {
    if (error) { current_response_.callback(error); return; }

    if (current_list_index_ >= elements_.size()) {
      WriteOK(current_response_);
      return;
    }
    auto* const element = &elements_[current_list_index_];
    if (element->name.size() == 0) {
      WriteOK(current_response_);
      return;
    }

    current_list_index_++;

    char *ptr = &send_buffer_[0];
    std::copy(element->name.begin(), element->name.end(), ptr);
    ptr += element->name.size();
    *ptr = '\r';
    ptr++;
    *ptr = '\n';
    ptr++;
    AsyncWrite(*current_response_.stream,
               gsl::cstring_span(send_buffer_, ptr),
               [this](int error) { ListCallback(error); });
  }

  void Schema(const gsl::cstring_span& name,
              const CommandManager::Response& response) {
    auto* const element =
        elements_.FindOrCreate(name, NamedRegistry::kFindOnly);
    if (!element) {
      WriteMessage(gsl::ensure_z("unknown name\r\n"), response);
      return;
    }

    Emit(
        gsl::ensure_z("schema "),
        element, [](NamedRegistry::Element* element, SimpleOStream* ostream) {
          element->ptr->base->WriteSchema(*ostream);
        },
        response);
  }

  void Rate(const gsl::cstring_span& command,
            const CommandManager::Response& response) {
    Tokenizer tokenizer(command, " ");
    auto name = tokenizer.next();
    auto rate_str = tokenizer.next();

    auto* const element =
        elements_.FindOrCreate(name, NamedRegistry::kFindOnly);
    if (!element) {
      WriteMessage(gsl::ensure_z("unknown name\r\n"), response);
      return;
    }

    char buffer[24] = {};
    Expects(rate_str.size() < (sizeof(buffer) - 1));
    std::copy(rate_str.begin(), rate_str.end(), buffer);
    long rate = strtol(buffer, nullptr, 0);
    element->ptr->rate = rate;
    if (rate == 0) {
      element->ptr->next = 1;
    } else if (rate < 10) {
      element->ptr->rate = 1;
      element->ptr->next = 1;
    } else {
      element->ptr->next = rate;
    }
    element->ptr->stream = response.stream;

    WriteOK(response);
  }

  void Format(const gsl::cstring_span& command,
              const CommandManager::Response& response) {
    Tokenizer tokenizer(command, " ");
    auto name = tokenizer.next();
    auto format_str = tokenizer.next();

    auto* const element =
        elements_.FindOrCreate(name, NamedRegistry::kFindOnly);
    if (!element) {
      WriteMessage(gsl::ensure_z("unknown name\r\n"), response);
      return;
    }

    char buffer[5] = {};
    Expects(format_str.size() < (sizeof(buffer) - 1));
    std::copy(format_str.begin(), format_str.end(), buffer);
    const long format = strtol(buffer, nullptr, 0);
    element->ptr->text = format != 0;

    WriteOK(response);
  }

  void Stop(const CommandManager::Response& response) {
    for (size_t i = 0; i < elements_.size(); i++) {
      if (elements_[i].name.size() == 0) { break; }
      elements_[i].ptr->to_send = false;
      elements_[i].ptr->rate = 0;
    }

    WriteOK(response);
  }

  void Text(const CommandManager::Response& response) {
    for (size_t i = 0; i < elements_.size(); i++) {
      if (elements_[i].name.size() == 0) { break; }
      elements_[i].ptr->text = true;
    }

    WriteOK(response);
  }

  void UnknownCommand(const gsl::cstring_span& command,
                      const CommandManager::Response& response) {
    WriteMessage(gsl::ensure_z("unknown command\r\n"), response);
  }

  void WriteOK(const CommandManager::Response& response) {
    WriteMessage(gsl::ensure_z("OK\r\n"), response);
  }

  void WriteMessage(const gsl::cstring_span& message,
                    const CommandManager::Response& response) {
    AsyncWrite(*response.stream, message, response.callback);
  }


  Pool& pool_;
  LockManager& lock_manager_;

  NamedRegistry elements_;

  CommandManager::Response current_response_;
  char send_buffer_[1024] = {};
  std::size_t current_list_index_ = 0;
  detail::EnumerateArchive::Context enumerate_context_;
  bool maybe_need_to_send_ = false;
};

TelemetryManager::TelemetryManager(
    Pool& pool, LockManager& lock_manager)
    : impl_(&pool, pool, lock_manager) {}

TelemetryManager::~TelemetryManager() {}

void TelemetryManager::Command(const gsl::cstring_span& command,
                               const CommandManager::Response& response) {
  Tokenizer tokenizer(command, " ");
  auto cmd = tokenizer.next();
  if (cmd == gsl::ensure_z("get")) {
    impl_->Get(tokenizer.remaining(), response);
  } else if (cmd == gsl::ensure_z("list")) {
    impl_->List(response);
  } else if (cmd == gsl::ensure_z("schema")) {
    impl_->Schema(tokenizer.remaining(), response);
  } else if (cmd == gsl::ensure_z("rate")) {
    impl_->Rate(tokenizer.remaining(), response);
  } else if (cmd == gsl::ensure_z("fmt")) {
    impl_->Format(tokenizer.remaining(), response);
  } else if (cmd == gsl::ensure_z("stop")) {
    impl_->Stop(response);
  } else if (cmd == gsl::ensure_z("text")) {
    impl_->Text(response);
  } else {
    impl_->UnknownCommand(cmd, response);
  }
}

void TelemetryManager::PollMillisecond() {
  impl_->PollMillisecond();
}

void TelemetryManager::Poll() {
  impl_->Poll();
}

Pool* TelemetryManager::pool() const { return &impl_->pool_; }

StaticFunction<void ()> TelemetryManager::RegisterDetail(
    const gsl::cstring_span& name, SerializableHandlerBase* base) {
  auto* item = impl_->elements_.FindOrCreate(
      name, Impl::NamedRegistry::kAllowCreate);
  PoolPtr<Impl::Element> element(&impl_->pool_);
  element->base = base;

  item->ptr = element.get();

  return [i = impl_.get(), item]() { i->UpdateItem(item); };
}
