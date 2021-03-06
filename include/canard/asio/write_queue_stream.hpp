#ifndef CANARD_ASIO_WRITE_QUEUE_STREAM_HPP
#define CANARD_ASIO_WRITE_QUEUE_STREAM_HPP

#include <cstddef>
#include <array>
#include <iterator>
#include <type_traits>
#include <memory>
#include <new>
#include <system_error>
#include <utility>
#include <boost/asio/buffer.hpp>
#include <boost/asio/completion_condition.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <canard/asio/async_result_init.hpp>
#include <canard/asio/detail/base_implementation.hpp>
#include <canard/asio/detail/bind_handler.hpp>
#include <canard/asio/detail/consuming_buffers.hpp>
#include <canard/asio/detail/operation_holder.hpp>
#include <canard/exception.hpp>

namespace canard {
namespace detail {

  using buffers_type
    = std::array<boost::asio::const_buffer, detail::max_buffers>;
  using buffers_iterator = buffers_type::iterator;

  class write_op_base
    : public detail::post_operation
  {
  protected:
    using complete_func_type = detail::operation::func_type;
    using post_func_type = detail::post_operation::post_func_type;
    using consume_func_type
      = auto(*)(write_op_base*, std::size_t&) -> boost::asio::const_buffer;
    using buffers_func_type
      = auto(*)(write_op_base*, buffers_iterator, buffers_iterator)
        -> buffers_iterator;

    write_op_base(
          complete_func_type const complete_func
        , post_func_type const post_func
        , consume_func_type const consume_func
        , buffers_func_type const buffers_func)
      : detail::post_operation{complete_func, post_func}
      , consume_func_(consume_func)
      , buffers_func_(buffers_func)
      , ec_{}
      , bytes_transferred_{}
    {
    }

  public:
    auto consume(std::size_t& bytes_transferred)
      -> boost::asio::const_buffer
    {
      return consume_func_(this, bytes_transferred);
    }

    auto buffers(buffers_iterator it, buffers_iterator it_end)
      -> buffers_iterator
    {
      return buffers_func_(this, it, it_end);
    }

    auto error_code() const
      -> boost::system::error_code const&
    {
      return ec_;
    }

    void error_code(boost::system::error_code const ec)
    {
      ec_ = ec;
    }

    auto bytes_transferred() const noexcept
      -> std::size_t
    {
      return bytes_transferred_;
    }

    auto bytes_transferred_ref() noexcept
      -> std::size_t&
    {
      return bytes_transferred_;
    }

    void add_bytes_transferred(std::size_t const bytes) noexcept
    {
      bytes_transferred_ += bytes;
    }

  private:
    consume_func_type consume_func_;
    buffers_func_type buffers_func_;
    boost::system::error_code ec_;
    std::size_t bytes_transferred_;
  };

  template <class WriteHandler, class ConstBufferSequence>
  class waiting_op
    : public write_op_base
  {
  public:
    waiting_op(WriteHandler& handler, ConstBufferSequence&& buffers)
      : write_op_base{&do_complete, &do_post, &do_consume, &do_buffers}
      , handler_(handler)
      , buffers_(std::move(buffers))
    {
    }

    waiting_op(WriteHandler& handler, ConstBufferSequence const& buffers)
      : write_op_base{&do_complete, &do_post, &do_consume, &do_buffers}
      , handler_(handler)
      , buffers_(buffers)
    {
    }

  private:
    static void do_complete(
          detail::io_service_impl* owner
        , detail::operation* base
        , boost::system::error_code const& ec
        , std::size_t bytes_transferred)
    {
      auto const op = static_cast<waiting_op*>(base);

      detail::op_holder<WriteHandler, waiting_op> holder{ op->handler_, op };

      auto function
        = detail::bind(op->handler_, op->error_code(), op->bytes_transferred());

      holder.handler(function.handler());
      holder.reset();

      if (owner) {
        detail::fenced_block b{detail::fenced_block::half};

        using boost::asio::asio_handler_invoke;
        asio_handler_invoke(function, std::addressof(function.handler()));
      }
    }

    static void do_post(
        boost::asio::io_service& io_service, detail::post_operation* base)
    {
      auto const op = static_cast<waiting_op*>(base);

      detail::op_holder<WriteHandler, waiting_op> holder{ op->handler_, op };

      auto function
        = detail::bind(op->handler_, op->error_code(), op->bytes_transferred());

      holder.handler(function.handler());
      holder.reset();

      io_service.post(std::move(function));
    }

    static auto do_consume(write_op_base* base, std::size_t& bytes_transferred)
      -> boost::asio::const_buffer
    {
      auto const op = static_cast<waiting_op*>(base);
      return op->buffers_.consume(
          op->bytes_transferred_ref(), bytes_transferred);
    }

    static auto do_buffers(
        write_op_base* base, buffers_iterator it, buffers_iterator it_end)
      -> buffers_iterator
    {
      auto const op = static_cast<waiting_op*>(base);
      return op->buffers_.copy_buffers(it, it_end);
    }

  private:
    WriteHandler handler_;
    canard::detail::consuming_buffers<ConstBufferSequence> buffers_;
  };

  template <class Stream, class Context>
  class write_queue_handler;

  template <class Stream, class Context>
  struct write_queue_stream_impl
    : Context
  {
    template <class... Args>
    explicit write_queue_stream_impl(Context&& context, Args&&... args)
      : Context(std::move(context))
      , stream(std::forward<Args>(args)...)
    {
    }

    void start_write(std::shared_ptr<write_queue_stream_impl> ptr)
    {
      auto bytes_transferred = std::size_t{};
      head_buffer() = waiting_queue.front()->consume(bytes_transferred);

      auto& io_srv_impl = detail::get_io_service_impl(stream.get_io_service());

      stream.async_write_some(
            gather_buffers()
          , write_queue_handler<Stream, Context>{io_srv_impl, std::move(ptr)});
    }

    void continue_write(write_queue_handler<Stream, Context> const& handler)
    {
      stream.async_write_some(gather_buffers(), handler);
    }

    auto head_buffer() noexcept
      -> boost::asio::const_buffer&
    {
      return buffers_.front();
    }

    auto gather_buffers()
      -> boost::iterator_range<buffers_iterator>
    {
      auto it = std::next(buffers_.begin());
      auto const it_end = buffers_.end();
      using detail::op_queue_access;
      for (auto op = waiting_queue.front(); op; op = op_queue_access::next(op)) {
        it = op->buffers(it, it_end);
        if (it == it_end) {
          break;
        }
      }
      return boost::make_iterator_range(buffers_.begin(), it);
    }

    Stream stream;
    detail::op_queue<detail::write_op_base> waiting_queue;
    buffers_type buffers_;
  };

  void set_error_code(
        boost::system::error_code const& ec
      , detail::op_queue<detail::write_op_base>& queue)
  {
    using detail::op_queue_access;
    for (auto op = queue.front(); op; op = op_queue_access::next(op)) {
      op->error_code(ec);
    }
  }

  template <class Stream, class Context>
  class write_queue_handler
  {
  private:
    using impl_type = write_queue_stream_impl<Stream, Context>;
    using operation_queue = op_queue<detail::operation>;

    struct on_do_complete_exit
    {
      ~on_do_complete_exit()
      {
        if (need_write) {
          try {
            this_->impl_->continue_write(*this_);
          }
          catch (boost::system::system_error const& e) {
            set_error_to_ready_queue(e.code());
          }
          catch (std::system_error const& e) {
            set_error_to_ready_queue(
                boost::system::error_code{
                    e.code().value(), boost::system::system_category()
                });
          }
          catch (std::exception const&) {
            set_error_to_ready_queue(
                make_error_code(canard::has_any_exception));
          }
        }

        while (auto const op = ready_queue.front()) {
          ready_queue.pop();
          detail::post_to(this_->io_service_impl_, op);
        }
      }

      void set_error_to_ready_queue(boost::system::error_code const& ec)
      {
        set_error_code(ec, this_->impl_->waiting_queue);
        ready_queue.push(this_->impl_->waiting_queue);
      }

      write_queue_handler* this_;
      operation_queue& ready_queue;
      bool need_write;
    };

  public:
    write_queue_handler(
          detail::io_service_impl& io_service_impl
        , std::shared_ptr<impl_type>&& impl)
        : io_service_impl_(io_service_impl)
        , impl_(std::move(impl))
    {
    }

    void operator()(boost::system::error_code const ec
                  , std::size_t bytes_transferred)
    {
      operation_queue ready_queue{};

      auto const head_op = impl_->waiting_queue.front();
      auto const head_buffer_size
        = boost::asio::buffer_size(impl_->head_buffer());
      if (head_buffer_size <= bytes_transferred) {
        head_op->add_bytes_transferred(head_buffer_size);
        bytes_transferred -= head_buffer_size;
        while (auto const op = impl_->waiting_queue.front()) {
          auto const buffer = op->consume(bytes_transferred);
          if (boost::asio::buffer_size(buffer) != 0) {
            impl_->head_buffer() = buffer;
            break;
          }
          if (bytes_transferred == 0) {
            op->error_code(ec);
          }
          impl_->waiting_queue.pop();
          ready_queue.push(op);
        }
      }
      else {
        impl_->head_buffer() = impl_->head_buffer() + bytes_transferred;
        head_op->add_bytes_transferred(bytes_transferred);
      }

      if (ec) {
        set_error_code(ec, impl_->waiting_queue);
        ready_queue.push(impl_->waiting_queue);
      }

      on_do_complete_exit on_exit{
        this, ready_queue, !impl_->waiting_queue.empty()
      };

      while (auto const op = ready_queue.front()) {
        ready_queue.pop();
        op->complete(io_service_impl_, ec, 0);
      }
    }

    template <class Function>
    friend void asio_handler_invoke(
        Function&& function, write_queue_handler* const handler)
    {
      using boost::asio::asio_handler_invoke;
      asio_handler_invoke(
            std::forward<Function>(function)
          , static_cast<Context*>(handler->impl_.get()));
    }

    friend auto asio_handler_allocate(
        std::size_t const size, write_queue_handler* const handler)
      -> void*
    {
      using boost::asio::asio_handler_allocate;
      return asio_handler_allocate(
          size, static_cast<Context*>(handler->impl_.get()));
    }

    friend void asio_handler_deallocate(
          void* const pointer, std::size_t const size
        , write_queue_handler* const handler)
    {
      using boost::asio::asio_handler_deallocate;
      asio_handler_deallocate(
            pointer, size
          , static_cast<Context*>(handler->impl_.get()));
    }

    friend auto asio_handler_is_continuation(write_queue_handler*)
      -> bool
    {
      return true;
    }

  private:
    detail::io_service_impl& io_service_impl_;
    std::shared_ptr<impl_type> impl_;
  };

  struct null_context
  {
    void operator()() const {}
  };

  template <class Context>
  struct queue_stream_context
  {
    using type = Context;

    static auto convert(Context& context)
      -> Context&&
    {
      return std::move(context);
    }
  };

  template <>
  struct queue_stream_context<boost::asio::io_service::strand>
  {
    using original_context = boost::asio::io_service::strand;
    using type
      = decltype(std::declval<original_context>().wrap(null_context{}));

    static auto convert(original_context& strand)
      -> type
    {
      return strand.wrap(null_context{});
    }
  };

  template <>
  struct queue_stream_context<boost::asio::io_service>
  {
    using original_context = boost::asio::io_service;
    using type
      = decltype(std::declval<original_context>().wrap(null_context{}));

    static auto convert(original_context& io_service)
      -> type
    {
      return io_service.wrap(null_context{});
    }
  };

} // namespace detail

template <
    class Stream = boost::asio::ip::tcp::socket
  , class Context = detail::null_context
>
class write_queue_stream
{
private:
  using context_helper = detail::queue_stream_context<Context>;
  using impl_type
    = detail::write_queue_stream_impl<Stream, typename context_helper::type>;

  template <class ReadHandler>
  using read_result_init = async_result_init<
      typename std::decay<ReadHandler>::type
    , void(boost::system::error_code, std::size_t)
  >;

  template <class WriteHandler>
  using write_result_init = async_result_init<
      typename std::decay<WriteHandler>::type
    , void(boost::system::error_code, std::size_t)
  >;

  template <class CompletionHandler>
  using completion_result_init = async_result_init<
      typename std::decay<CompletionHandler>::type
    , void()
  >;

  struct queue_cleanup
  {
    ~queue_cleanup()
    {
      if (!commit) {
        queue->pop();
      }
    }
    detail::op_queue<detail::write_op_base>* queue;
    bool commit;
  };

public:
  using next_layer_type = typename std::remove_reference<Stream>::type;
  using lowest_layer_type = typename next_layer_type::lowest_layer_type;

  explicit write_queue_stream(Stream stream)
    : write_queue_stream{std::move(stream), Context{}}
  {
  }

  write_queue_stream(Stream stream, Context context)
    : impl_{
        std::make_shared<impl_type>(
          context_helper::convert(context), std::move(stream))
      }
  {
  }

  template <class... Args>
  explicit write_queue_stream(
      boost::asio::io_service& io_service, Args&&... args)
    : write_queue_stream{
        Context{}, io_service, std::forward<Args>(args)...
      }
  {
  }

  template <class... Args>
  write_queue_stream(Context context, Args&&... args)
    : impl_{
        std::make_shared<impl_type>(
          context_helper::convert(context), std::forward<Args>(args)...)
      }
  {
  }

  write_queue_stream(write_queue_stream const&) = delete;
  auto operator=(write_queue_stream const&) -> write_queue_stream& = delete;

  auto get_io_service()
    -> boost::asio::io_service&
  {
    return next_layer().get_io_service();
  }

  auto next_layer()
    -> next_layer_type&
  {
      return impl_->stream;
  }

  auto lowest_layer()
    -> lowest_layer_type&
  {
      return impl_->stream.lowest_layer();
  }

  template <class MutableBufferSequence, class ReadHandler>
  auto async_read_some(MutableBufferSequence&& buffers, ReadHandler&& handler)
    -> typename read_result_init<ReadHandler>::result_type
  {
    return next_layer().async_read_some(
          std::forward<MutableBufferSequence>(buffers)
        , std::forward<ReadHandler>(handler));
  }

  template <class ConstBufferSequence, class WriteHandler>
  auto async_write_some(ConstBufferSequence&& buffers, WriteHandler&& handler)
    -> typename write_result_init<WriteHandler>::result_type
  {
    write_result_init<WriteHandler> init{
      std::forward<WriteHandler>(handler)
    };

    using handler_type
      = typename write_result_init<WriteHandler>::handler_type;

    using operation_type = detail::waiting_op<
        handler_type
      , typename std::decay<ConstBufferSequence>::type
    >;

    detail::op_holder<handler_type, operation_type> holder{ init.handler() };
    auto const write_op = holder.construct(
        init.handler(), std::forward<ConstBufferSequence>(buffers));

    auto const enable_to_send = impl_->waiting_queue.empty();
    impl_->waiting_queue.push(write_op);
    if (enable_to_send) {
      queue_cleanup on_exit{ std::addressof(impl_->waiting_queue), false };
      impl_->start_write(impl_);
      on_exit.commit = true;
    }

    holder.release();

    return init.get();
  }

private:
  std::shared_ptr<impl_type> impl_;
};

} // namespace canard

#endif // CANARD_ASIO_WRITE_QUEUE_STREAM_HPP

