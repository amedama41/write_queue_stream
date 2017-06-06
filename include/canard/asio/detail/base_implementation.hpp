#ifndef CANARD_ASIO_DETAIL_BASE_IMPLEMENTATION_HPP
#define CANARD_ASIO_DETAIL_BASE_IMPLEMENTATION_HPP

#ifdef CANARD_DEPENDS_ASIO_DETAIL_IMPL

#include <canard/asio/detail/asio_detail_impl.hpp>

#else // CANARD_DEPENDS_ASIO_DETAIL_IMPL

#include <climits>
#include <cstddef>
#include <atomic>
#include <memory>
#include <boost/asio/io_service.hpp>
#include <boost/asio/detail/fenced_block.hpp>
#include <boost/system/error_code.hpp>

namespace canard {
namespace detail {

  using io_service_impl = boost::asio::io_service;

  template <class Operation> class op_queue;

  class op_queue_access
  {
  public:
    template <class Operation>
    static auto next(Operation* const op) noexcept
      -> Operation*
    {
      return static_cast<Operation*>(op->next_);
    }

    template <class Operation1, class Operation2>
    static void next(Operation1* const op1, Operation2 const op2) noexcept
    {
      op1->next_ = op2;
    }

    template <class Operation>
    static auto front(op_queue<Operation>& queue) noexcept
      -> Operation*&
    {
      return queue.front_;
    }

    template <class Operation>
    static auto back(op_queue<Operation>& queue) noexcept
      -> Operation*&
    {
      return queue.back_;
    }
  };

  template <class Operation>
  class op_queue
  {
  public:
    op_queue(op_queue const&) = delete;
    op_queue(op_queue&&) = delete;
    auto operator=(op_queue const&) -> op_queue& = delete;
    auto operator=(op_queue&&) -> op_queue& = delete;

    op_queue() noexcept
      : front_(nullptr)
      , back_(nullptr)
    {
    }

    ~op_queue()
    {
      while (Operation* op = front_) {
        pop();
        op->destroy();
      }
    }

    auto front() noexcept
      -> Operation*
    {
      return front_;
    }

    void pop() noexcept
    {
      if (!empty()) {
        auto* tmp = front_;
        front_ = op_queue_access::next(front_);
        if (!front_) {
          back_ = nullptr;
        }
        op_queue_access::next(tmp, nullptr);
      }
    }

    void push(Operation* const op) noexcept
    {
      op_queue_access::next(op, nullptr);
      if (!empty()) {
        op_queue_access::next(back_, op);
        back_ = op;
      }
      else {
        front_ = back_ = op;
      }
    }

    template <class OtherOperation>
    void push(op_queue<OtherOperation>& queue) noexcept
    {
      if (auto const other_front = queue.front()) {
        if (empty()) {
          front_ = other_front;
        }
        else {
          op_queue_access::next(back_, other_front);
        }
        back_ = op_queue_access::back(queue);
        op_queue_access::front(queue) = op_queue_access::back(queue) = nullptr;
      }
    }

    auto empty() const noexcept
      -> bool
    {
      return front_ == nullptr;
    }

  private:
    friend op_queue_access;

    Operation* front_;
    Operation* back_;
  };

  class post_operation
  {
  public:
    void complete(
          io_service_impl& owner
        , boost::system::error_code const& ec, std::size_t bytes_transferred)
    {
      func_(std::addressof(owner), this, ec, bytes_transferred);
    }

    void post_to(boost::asio::io_service& io_service)
    {
      post_func_(io_service, this);
    }

    void destroy()
    {
      func_(nullptr, this, boost::system::error_code(), 0);
    }

  protected:
    using func_type = void(*)(
          io_service_impl*, post_operation*
        , boost::system::error_code const&, std::size_t);
    using post_func_type = void(*)(boost::asio::io_service&, post_operation*);

    explicit post_operation(func_type func, post_func_type post_func) noexcept
      : next_(nullptr)
      , func_(func)
      , post_func_(post_func)
    {
    }

    ~post_operation() = default;

  private:
    friend op_queue_access;

    post_operation* next_;
    func_type func_;
    post_func_type post_func_;
  };

  using operation = post_operation;

  class fenced_block
  {
  public:
    enum half_t { half };
    enum full_t { full };

    fenced_block(fenced_block const&) = delete;
    auto operator=(fenced_block const&) -> fenced_block& = delete;

    explicit fenced_block(half_t) noexcept
    {
    }

    explicit fenced_block(full_t) noexcept
    {
      std::atomic_thread_fence(std::memory_order_acquire);
    }

    ~fenced_block() noexcept
    {
      std::atomic_thread_fence(std::memory_order_release);
    }
  };

  inline auto get_io_service_impl(boost::asio::io_service& io_service) noexcept
    -> io_service_impl&
  {
    return io_service;
  }

  inline void post_to(io_service_impl& io_service_impl, operation* op)
  {
    op->post_to(io_service_impl);
  }

#if defined(IOV_MAX)
  constexpr std::size_t max_iov_len = IOV_MAX;
#else
  constexpr std::size_t max_iov_len = 16;
#endif

  constexpr std::size_t max_buffers = (64 < max_iov_len) ? 64 : max_iov_len;

} // namespace detail
} // namespace canard

#endif // CANARD_DEPENDS_ASIO_DETAIL_IMPL

#endif // CANARD_ASIO_DETAIL_BASE_IMPLEMENTATION_HPP
