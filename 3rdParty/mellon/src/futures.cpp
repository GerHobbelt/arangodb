#include "mellon/futures.h"

using namespace mellon;

detail::invalid_pointer_type detail::invalid_pointer_inline_value;
detail::invalid_pointer_type detail::invalid_pointer_future_abandoned;
detail::invalid_pointer_type detail::invalid_pointer_promise_abandoned;
detail::invalid_pointer_type detail::invalid_pointer_promise_fulfilled;

const char* promise_abandoned_error::what() const noexcept {
  return "promise abandoned";
}

#include <sys/types.h>
#include <unistd.h>

#include <iostream>

namespace {
template <typename T, std::size_t N>
std::ostream& operator<<(std::ostream& os, std::array<T, N> const& arr) {
  os << "[";
  for (std::size_t i = 0; i < N; i++) {
    if (i != 0) {
      os << ",";
    }
    os << arr[i];
  }
  os << "]";
  return os;
}
}  // namespace

struct allocation_printer {
  ~allocation_printer() {
    std::cerr << "[FUTURES] number_of_allocations=" << mellon::detail::number_of_allocations
              << " number_of_bytes_allocated=" << mellon::detail::number_of_bytes_allocated
              << " number_of_inline_value_placements=" << mellon::detail::number_of_inline_value_placements
              << " number_of_temporary_objects=" << mellon::detail::number_of_temporary_objects
              << " number_of_prealloc_usage=" << mellon::detail::number_of_prealloc_usage
              << " number_of_inline_value_allocs=" << mellon::detail::number_of_inline_value_allocs
              << " number_of_final_usage=" << mellon::detail::number_of_final_usage
              << " number_of_step_usage=" << mellon::detail::number_of_step_usage
              << " histogram_value_sizes=" << mellon::detail::histogram_value_sizes
              << " histogram_final_lambda_sizes=" << mellon::detail::histogram_final_lambda_sizes
              << " number_of_promises_created=" << mellon::detail::number_of_promises_created
              << std::endl;
  }
};

#ifdef FUTURES_COUNT_ALLOC
std::atomic<std::size_t> mellon::detail::number_of_allocations = 0;
std::atomic<std::size_t> mellon::detail::number_of_bytes_allocated = 0;
std::atomic<std::size_t> mellon::detail::number_of_inline_value_placements = 0;
std::atomic<std::size_t> mellon::detail::number_of_temporary_objects = 0;
std::atomic<std::size_t> mellon::detail::number_of_prealloc_usage = 0;

std::atomic<std::size_t> mellon::detail::number_of_inline_value_allocs = 0;
std::atomic<std::size_t> mellon::detail::number_of_final_usage = 0;
std::atomic<std::size_t> mellon::detail::number_of_step_usage = 0;
std::atomic<std::size_t> mellon::detail::number_of_promises_created = 0;

std::array<std::atomic<std::size_t>, 10> mellon::detail::histogram_value_sizes{};
std::array<std::atomic<std::size_t>, 10> mellon::detail::histogram_final_lambda_sizes{};

allocation_printer printer;
#endif
