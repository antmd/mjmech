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

#include <tuple>

#include <boost/property_tree/ptree.hpp>

#include "visit_archive.h"
#include "visitor.h"

namespace legtool {
class PropertyTreeWriteArchive
    : public VisitArchive<PropertyTreeWriteArchive> {
 public:
  typedef boost::property_tree::ptree ptree;

  template <typename NameValuePair>
  void Visit(const NameValuePair& pair) {
    ptree tree;
    VisitHelper(pair.value(), &tree, 0);
    tree_.put_child(pair.name(), tree);
  }

  template <typename Value>
  auto VisitHelper(Value* value, ptree* tree, int) ->
      decltype(value->Serialize((PropertyTreeWriteArchive*)nullptr)) {
    PropertyTreeWriteArchive sub;
    sub.Accept(value);
    *tree = sub.tree();
  }

  template <typename Value>
  void VisitHelper(std::vector<Value>* value_vector,
                   ptree* tree, int) {
    for (int i = 0; i < value_vector->size(); i++) {
      ptree element;
      VisitHelper(&(*value_vector)[i], &element, 0);
      tree->push_back(ptree::value_type("", element));
    }
  }

  template <typename Value>
  void VisitHelper(Value* value, ptree* tree, long) {
    tree->put_value(*value);
  }

  boost::property_tree::ptree tree() const { return tree_; }

 private:
  boost::property_tree::ptree tree_;
};

class PropertyTreeReadArchive
    : public VisitArchive<PropertyTreeReadArchive> {
 public:
  PropertyTreeReadArchive(const boost::property_tree::ptree& tree)
      : tree_(tree) {}

  template <typename NameValuePair>
  void VisitSerializable(const NameValuePair& pair) {
    if (!tree_.get_child_optional(pair.name())) { return; }
    PropertyTreeReadArchive(tree_.get_child(pair.name())).
        Accept(pair.value());
  }

  template <typename NameValuePair>
  void VisitScalar(const NameValuePair& pair) {
    VisitHelper(pair.name(), pair.value(), 0);
  }

  template <typename T>
  class FakeNvp {
   public:
    FakeNvp(const char* name, T* value): name_(name), value_(value) {}

    const char* name() const { return name_; }
    T* value() const { return value_; }

   private:
    const char* const name_;
    T* const value_;
  };

  template <typename T>
  void VisitHelper(const char* name,
                   std::vector<T>* value_vector,
                   int) {
    auto optional_child = tree_.get_child_optional(name);
    if (!optional_child) { return; }
    auto child = *optional_child;
    value_vector->clear();
    for (auto it = child.begin(); it != child.end(); ++it) {
      value_vector->push_back(T());
      PropertyTreeReadArchive(it->second).
          Visit(FakeNvp<T>("", &value_vector->back()));
    }
  }

  template <typename T>
  void VisitHelper(const char* name,
                   T* value,
                   long) {
    auto optional_child = tree_.get_child_optional(name);
    if (!optional_child) { return; }
    auto child = *optional_child;
    *value = child.template get_value<T>();
  }

 private:
  boost::property_tree::ptree tree_;
};
}
