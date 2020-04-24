/*
 * Copyright 2020 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//===- map_dataset.h --------------------------------------------*- C++ -*-===//
//
// This file declares MapDataset class which wraps around another Dataset
// instance and transforms the element before returning it to the caller.
//
//===----------------------------------------------------------------------===//

#ifndef TFRT_LIB_DATA_MAP_DATASET_H_
#define TFRT_LIB_DATA_MAP_DATASET_H_

#include "dataset.h"
#include "tfrt/host_context/function.h"
#include "tfrt/support/forward_decls.h"

namespace tfrt {
namespace data {

template <typename... T>
class MapDataset;

template <typename... T>
class MapDatasetIterator;

// This helper function takes SmallVector instead of ArrayRef to explicitly
// indicate the ownership of the input AsyncValue so that it can safely use
// std::move(...) to instantiate the return value.
template <typename... T, std::size_t... Indices>
static std::tuple<T...> ArrayToTupleHelper(
    SmallVector<RCReference<AsyncValue>, sizeof...(T)> results,
    std::index_sequence<Indices...>) {
  // IDEA(donglin): we currently change the input AsyncValue in order to
  // generate the return value. This will not work if the input AsyncValue needs
  // to be accessed later. A better solution is to generate an AsyncValue that
  // wraps around the input AsyncValue<std::tuple<...>>.
  return std::make_tuple(std::move(results[Indices]->template get<T>())...);
}

// This helper function takes SmallVector instead of ArrayRef to explicitly
// indicate the ownership of the input AsyncValue so that it can safely use
// std::move(...) to instantiate the return value.
template <typename... T>
static std::tuple<T...> ArrayToTuple(
    SmallVector<RCReference<AsyncValue>, sizeof...(T)> results) {
  assert(results.size() == sizeof...(T));
  return ArrayToTupleHelper<T...>(std::move(results),
                                  std::make_index_sequence<sizeof...(T)>());
}

// Partial specialization of MapDataset to support multiple parameter packs.
// MapDataset maps a user-defined function over the elements in its input
// dataset.
template <typename... InputTypes, typename... OutputTypes>
class MapDataset<std::tuple<InputTypes...>, std::tuple<OutputTypes...>>
    : public Dataset<OutputTypes...> {
 public:
  explicit MapDataset(RCReference<Dataset<InputTypes...>> input_dataset,
                      RCReference<const Function> map_fn, HostContext* host)
      : input_dataset_(std::move(input_dataset)),
        host_(host),
        allocator_(host->allocator()),
        map_fn_(std::move(map_fn)) {}

  // This class is not copyable or movable.
  MapDataset(const MapDataset&) = delete;
  MapDataset& operator=(const MapDataset&) = delete;

  std::unique_ptr<Iterator<OutputTypes...>> MakeIterator() override;

 private:
  // Allow iterator to rely on private data members of this dataset.
  friend class MapDatasetIterator<std::tuple<InputTypes...>,
                                  std::tuple<OutputTypes...>>;

  void Destroy() override {
    internal::DestroyImpl<
        MapDataset<std::tuple<InputTypes...>, std::tuple<OutputTypes...>>>(
        this, allocator_);
  }

  RCReference<Dataset<InputTypes...>> input_dataset_;
  HostContext* host_;
  HostAllocator* allocator_;
  RCReference<const Function> map_fn_;
};

template <typename... InputTypes, typename... OutputTypes>
class MapDatasetIterator<std::tuple<InputTypes...>, std::tuple<OutputTypes...>>
    : public Iterator<OutputTypes...> {
 public:
  explicit MapDatasetIterator(
      RCReference<
          MapDataset<std::tuple<InputTypes...>, std::tuple<OutputTypes...>>>
          parent_dataset)
      : Iterator<OutputTypes...>(parent_dataset->host_),
        parent_dataset_(std::move(parent_dataset)),
        input_iterator_(parent_dataset_->input_dataset_->MakeIterator()) {}

  AsyncValueRef<std::tuple<OutputTypes...>> GetNext(Location loc) override {
    auto* host = IteratorBase::host_;
    const Function* map_fn = parent_dataset_->map_fn_.get();
    auto args = input_iterator_->GetNext(loc);
    if (!args) {
      return AsyncValueRef<std::tuple<OutputTypes...>>();
    }
    if (args.IsError()) {
      return AsyncValueRef<std::tuple<OutputTypes...>>(args.ReleaseRCRef());
    }
    auto async_result = host->template MakeUnconstructedAsyncValueRef<
        std::tuple<OutputTypes...>>();
    // IDEA(donglin): We can optimize performance for small tasks by not
    // enqueueing small tasks to the threadpool. We need a way to identify small
    // tasks.
    //
    // Enqueue the map function to the threadpool to improve performance by
    // running the map function in parallel. An alternative approach to increase
    // parallelism is to compose map function with async kernels. This
    // alternative approach likely incurs higher thread context switch overhead
    // because different async kernels may be run by different threads.
    host->EnqueueWork([host, map_fn = FormRef(map_fn), args = std::move(args),
                       async_result = async_result.CopyRef()]() {
      // IDEA(donglin): We can optimize performance by constructing a view of
      // AsyncValue<T> from AsyncValue<std::tuple<T>> without moving data.
      args.AndThen([host, args = args.CopyRef(), map_fn = map_fn.CopyRef(),
                    async_result = async_result.CopyRef()] {
        if (args.IsError()) {
          async_result.SetError(args.GetError());
        }
        // Wrap input argument in AsyncValue for function execution.
        SmallVector<AsyncValue*, 4> arguments;
        auto arg = host->template MakeConcreteAsyncValueRef<InputTypes...>(
            std::move(std::get<0>(args.get())));

        arguments.push_back(arg.GetAsyncValue());
        SmallVector<RCReference<AsyncValue>, sizeof...(OutputTypes)> results;
        results.resize(map_fn->result_types().size());
        map_fn->Execute(arguments, results, host);
        for (size_t i = 0; i < sizeof...(OutputTypes); ++i) {
          if (results[i]->IsError()) {
            async_result.SetError(results[i]->GetError());
            return;
          }
        }
        // Translate RCReference<AsyncValue> to AsyncValue*.
        SmallVector<AsyncValue*, 4> async_value_ptrs;
        for (size_t i = 0; i < sizeof...(OutputTypes); ++i) {
          async_value_ptrs.push_back(results[i].get());
        }
        host->RunWhenReady(async_value_ptrs,
                           [results = std::move(results),
                            async_result = async_result.CopyRef()]() mutable {
                             for (auto& result : results) {
                               if (result->IsError()) {
                                 async_result.SetError(result->GetError());
                                 return;
                               }
                             }
                             async_result.emplace(ArrayToTuple<OutputTypes...>(
                                 std::move(results)));
                           });
      });
    });
    return async_result;
  }

 private:
  // This class is not copyable or movable.
  MapDatasetIterator(const MapDatasetIterator&) = delete;
  MapDatasetIterator& operator=(const MapDatasetIterator&) = delete;

  RCReference<MapDataset<std::tuple<InputTypes...>, std::tuple<OutputTypes...>>>
      parent_dataset_;
  std::unique_ptr<Iterator<InputTypes...>> input_iterator_;
};

template <typename... InputTypes, typename... OutputTypes>
std::unique_ptr<Iterator<OutputTypes...>> MapDataset<
    std::tuple<InputTypes...>, std::tuple<OutputTypes...>>::MakeIterator() {
  return std::make_unique<MapDatasetIterator<std::tuple<InputTypes...>,
                                             std::tuple<OutputTypes...>>>(
      FormRef(this));
}

}  // namespace data
}  // namespace tfrt

#endif  // TFRT_LIB_DATA_MAP_DATASET_H_