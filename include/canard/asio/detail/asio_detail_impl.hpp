#ifndef CANARD_ASIO_DETAIL_ASIO_DETAIL_IMPL_HPP
#define CANARD_ASIO_DETAIL_ASIO_DETAIL_IMPL_HPP

#include <cstddef>
#include <boost/asio/io_service.hpp>
#include <boost/asio/detail/buffer_sequence_adapter.hpp>
#include <boost/asio/detail/fenced_block.hpp>
#include <boost/asio/detail/op_queue.hpp>
#include <boost/asio/detail/operation.hpp>

namespace canard {
namespace detail {

  using boost::asio::detail::io_service_impl;
  using boost::asio::detail::op_queue_access;
  using boost::asio::detail::operation;
  using boost::asio::detail::op_queue;
  using boost::asio::detail::fenced_block;

  class post_operation
    : public boost::asio::detail::operation
  {
  protected:
    using post_func_type = void(*)(boost::asio::io_service&, post_operation*);

    explicit post_operation(func_type func, post_func_type)
      : boost::asio::detail::operation{func}
    {
    }
  };

  inline auto get_io_service_impl(boost::asio::io_service& io_service)
    -> io_service_impl&
  {
    return boost::asio::use_service<io_service_impl>(io_service);
  }

  inline void post_to(io_service_impl& io_service_impl, operation* op)
  {
    io_service_impl.post_immediate_completion(op, true);
  }

  struct asio_limiting_params
    : private boost::asio::detail::buffer_sequence_adapter_base
  {
    using buffer_sequence_adapter_base::max_buffers;
  };

  constexpr std::size_t max_buffers = asio_limiting_params::max_buffers;

} // namespace detail
} // namespace canard

#endif // CANARD_ASIO_DETAIL_ASIO_DETAIL_IMPL_HPP
