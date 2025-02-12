/*!
 * Copyright 2022 XGBoost contributors
 */
#include "in_memory_handler.h"

#include <algorithm>
#include <functional>

namespace xgboost {
namespace collective {

/**
 * @brief Functor for allreduce.
 */
class AllreduceFunctor {
 public:
  std::string const name{"Allreduce"};

  AllreduceFunctor(DataType dataType, Operation operation)
      : data_type_(dataType), operation_(operation) {}

  void operator()(char const* input, std::size_t bytes, std::string* buffer) const {
    if (buffer->empty()) {
      // Copy the input if this is the first request.
      buffer->assign(input, bytes);
    } else {
      // Apply the reduce_operation to the input and the buffer.
      Accumulate(input, bytes / GetTypeSize(data_type_), &buffer->front());
    }
  }

 private:
  template <class T>
  void Accumulate(T* buffer, T const* input, std::size_t size, Operation reduce_operation) const {
    switch (reduce_operation) {
      case Operation::kMax:
        std::transform(buffer, buffer + size, input, buffer,
                       [](T a, T b) { return std::max(a, b); });
        break;
      case Operation::kMin:
        std::transform(buffer, buffer + size, input, buffer,
                       [](T a, T b) { return std::min(a, b); });
        break;
      case Operation::kSum:
        std::transform(buffer, buffer + size, input, buffer, std::plus<T>());
        break;
      default:
        throw std::invalid_argument("Invalid reduce operation");
    }
  }

  void Accumulate(char const* input, std::size_t size, char* buffer) const {
    switch (data_type_) {
      case DataType::kInt8:
        Accumulate(reinterpret_cast<std::int8_t*>(buffer),
                   reinterpret_cast<std::int8_t const*>(input), size, operation_);
        break;
      case DataType::kUInt8:
        Accumulate(reinterpret_cast<std::uint8_t*>(buffer),
                   reinterpret_cast<std::uint8_t const*>(input), size, operation_);
        break;
      case DataType::kInt32:
        Accumulate(reinterpret_cast<std::int32_t*>(buffer),
                   reinterpret_cast<std::int32_t const*>(input), size, operation_);
        break;
      case DataType::kUInt32:
        Accumulate(reinterpret_cast<std::uint32_t*>(buffer),
                   reinterpret_cast<std::uint32_t const*>(input), size, operation_);
        break;
      case DataType::kInt64:
        Accumulate(reinterpret_cast<std::int64_t*>(buffer),
                   reinterpret_cast<std::int64_t const*>(input), size, operation_);
        break;
      case DataType::kUInt64:
        Accumulate(reinterpret_cast<std::uint64_t*>(buffer),
                   reinterpret_cast<std::uint64_t const*>(input), size, operation_);
        break;
      case DataType::kFloat:
        Accumulate(reinterpret_cast<float*>(buffer), reinterpret_cast<float const*>(input), size,
                   operation_);
        break;
      case DataType::kDouble:
        Accumulate(reinterpret_cast<double*>(buffer), reinterpret_cast<double const*>(input), size,
                   operation_);
        break;
      default:
        throw std::invalid_argument("Invalid data type");
    }
  }

 private:
  DataType data_type_;
  Operation operation_;
};

/**
 * @brief Functor for broadcast.
 */
class BroadcastFunctor {
 public:
  std::string const name{"Broadcast"};

  BroadcastFunctor(int rank, int root) : rank_(rank), root_(root) {}

  void operator()(char const* input, std::size_t bytes, std::string* buffer) const {
    if (rank_ == root_) {
      // Copy the input if this is the root.
      buffer->assign(input, bytes);
    }
  }

 private:
  int rank_;
  int root_;
};

void InMemoryHandler::Init(int world_size, int) {
  CHECK(world_size_ < world_size) << "In memory handler already initialized.";

  std::unique_lock<std::mutex> lock(mutex_);
  world_size_++;
  cv_.wait(lock, [this, world_size] { return world_size_ == world_size; });
  lock.unlock();
  cv_.notify_all();
}

void InMemoryHandler::Shutdown(uint64_t sequence_number, int) {
  CHECK(world_size_ > 0) << "In memory handler already shutdown.";

  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this, sequence_number] { return sequence_number_ == sequence_number; });
  received_++;
  cv_.wait(lock, [this] { return received_ == world_size_; });

  received_ = 0;
  world_size_ = 0;
  sequence_number_ = 0;
  lock.unlock();
  cv_.notify_all();
}

void InMemoryHandler::Allreduce(char const* input, std::size_t bytes, std::string* output,
                                std::size_t sequence_number, int rank, DataType data_type,
                                Operation op) {
  Handle(input, bytes, output, sequence_number, rank, AllreduceFunctor{data_type, op});
}

void InMemoryHandler::Broadcast(char const* input, std::size_t bytes, std::string* output,
                                std::size_t sequence_number, int rank, int root) {
  Handle(input, bytes, output, sequence_number, rank, BroadcastFunctor{rank, root});
}

template <class HandlerFunctor>
void InMemoryHandler::Handle(char const* input, std::size_t bytes, std::string* output,
                             std::size_t sequence_number, int rank, HandlerFunctor const& functor) {
  // Pass through if there is only 1 client.
  if (world_size_ == 1) {
    if (input != output->data()) {
      output->assign(input, bytes);
    }
    return;
  }

  std::unique_lock<std::mutex> lock(mutex_);

  LOG(INFO) << functor.name << " rank " << rank << ": waiting for current sequence number";
  cv_.wait(lock, [this, sequence_number] { return sequence_number_ == sequence_number; });

  LOG(INFO) << functor.name << " rank " << rank << ": handling request";
  functor(input, bytes, &buffer_);
  received_++;

  if (received_ == world_size_) {
    LOG(INFO) << functor.name << " rank " << rank << ": all requests received";
    output->assign(buffer_);
    sent_++;
    lock.unlock();
    cv_.notify_all();
    return;
  }

  LOG(INFO) << functor.name << " rank " << rank << ": waiting for all clients";
  cv_.wait(lock, [this] { return received_ == world_size_; });

  LOG(INFO) << functor.name << " rank " << rank << ": sending reply";
  output->assign(buffer_);
  sent_++;

  if (sent_ == world_size_) {
    LOG(INFO) << functor.name << " rank " << rank << ": all replies sent";
    sent_ = 0;
    received_ = 0;
    buffer_.clear();
    sequence_number_++;
    lock.unlock();
    cv_.notify_all();
  }
}
}  // namespace collective
}  // namespace xgboost
