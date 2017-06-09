#define BOOST_TEST_DYN_LINK
#include <canard/asio/write_queue_stream.hpp>
#include <boost/test/unit_test.hpp>
#include <cstddef>
#include <algorithm>
#include <functional>
#include <future>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <boost/asio/io_service.hpp>
#include <boost/asio/strand.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/system/error_code.hpp>
#include <boost/thread/barrier.hpp>
#include "libs/stub_stream.hpp"

namespace asio = boost::asio;
namespace sys = boost::system;
using stub_stream = canard_test::stub_stream<char>;

template <class Context, std::size_t NumBuffers = 10>
struct buffer_fixture : Context
{
  buffer_fixture() : buffers(num_buffers)
  {
    result.reserve(num_buffers);
    constexpr auto buffer_size = std::size_t{512};
    for (auto i = 0; i < num_buffers; ++i) {
      buffers[i] = std::string(buffer_size, char('0' + i));
    }
  }

  auto joined_buffers() const -> std::vector<char>
  {
    return boost::copy_range<std::vector<char>>(
        boost::algorithm::join(buffers, ""));
  }

  auto joined_buffers(std::size_t const size) const -> std::vector<char>
  {
    auto tmp = joined_buffers();
    tmp.resize(size);
    return tmp;
  }

  struct callback_impl
  {
    void operator()(sys::error_code const& ec, std::size_t const size) {
      this_.result.emplace_back(ec, size);
    };
    buffer_fixture& this_;
  };

  auto callback() -> callback_impl
  {
    return callback_impl{*this};
  }

  template <class Stream>
  struct chained_callback_impl
  {
    void operator()(sys::error_code const& ec, std::size_t const size) {
      this_.result.emplace_back(ec, size);
      if (++counter != num_buffers) {
        stream.async_write_some(asio::buffer(this_.buffers[counter]), *this);
      }
    }
    buffer_fixture& this_;
    Stream& stream;
    std::size_t counter;
  };

  template <class Stream>
  auto chained_callback(Stream& stream)
    -> chained_callback_impl<Stream>
  {
    return chained_callback_impl<Stream>{*this, stream, 0};
  }

  enum { num_buffers = NumBuffers, buffer_size = 512 };
  std::vector<std::string> buffers;
  std::vector<std::tuple<sys::error_code, std::size_t>> result;
};

template <std::size_t MaxSize = 0>
struct default_ctx_fixture
{
  using stream_type = canard::write_queue_stream<stub_stream>;
  asio::io_service io_service{};
  std::size_t max_size = MaxSize;
};

template <std::size_t MaxSize = 0>
struct strand_ctx_fixture
{
  using stream_type
    = canard::write_queue_stream<stub_stream, asio::io_service::strand>;
  asio::io_service io_service{};
  asio::io_service::strand strand{io_service};
  std::size_t max_size = MaxSize;
};

struct original_ctx
{
  struct data {
    std::size_t alloc_counter = 0;
    std::size_t dealloc_counter = 0;
    std::size_t invoke_counter = 0;
    std::size_t continuation_counter = 0;
  };
  data* ptr;

  friend auto asio_handler_allocate(std::size_t size, original_ctx* ctx)
    -> void*
  {
    ++ctx->ptr->alloc_counter;
    return operator new(size);
  }

  friend void asio_handler_deallocate(void* p, std::size_t, original_ctx* ctx)
  {
    ++ctx->ptr->dealloc_counter;
    operator delete(p);
  }

  template <class Function>
  friend void asio_handler_invoke(Function&& function, original_ctx* ctx)
  {
    ++ctx->ptr->invoke_counter;
    function();
    ++ctx->ptr->invoke_counter;
  }

  friend bool asio_handler_is_continuation(original_ctx* ctx)
  {
    ++ctx->ptr->continuation_counter;
    return false;
  }
};

template <std::size_t MaxSize = 0>
struct original_ctx_fixture
{
  using stream_type = canard::write_queue_stream<stub_stream, original_ctx>;
  asio::io_service io_service{};
  std::size_t max_size = MaxSize;
};


BOOST_AUTO_TEST_SUITE(write_queue_stream)


  BOOST_AUTO_TEST_SUITE(default_context)

    BOOST_FIXTURE_TEST_CASE(construct_from_stream, default_ctx_fixture<5000>)
    {
      auto ec = make_error_code(sys::errc::bad_message);
      auto base_stream = stub_stream{io_service, max_size, ec};

      stream_type stream{std::move(base_stream)};

      BOOST_TEST(&stream.get_io_service() == &io_service);
      BOOST_TEST(stream.next_layer().max_writable_size_per_write() == max_size);
      BOOST_TEST(stream.next_layer().error_code() == ec);
      BOOST_TEST(stream.next_layer().written_data().size() == 0);
    }

    BOOST_FIXTURE_TEST_CASE(construct_from_arguments, default_ctx_fixture<3000>)
    {
      auto ec = make_error_code(sys::errc::no_stream_resources);

      stream_type stream{io_service, max_size, ec};

      BOOST_TEST(&stream.get_io_service() == &io_service);
      BOOST_TEST(stream.next_layer().max_writable_size_per_write() == max_size);
      BOOST_TEST(stream.next_layer().error_code() == ec);
      BOOST_TEST(stream.next_layer().written_data().size() == 0);
    }

    BOOST_FIXTURE_TEST_CASE(non_copy_constructible, default_ctx_fixture<5000>)
    {
      BOOST_TEST(not std::is_copy_constructible<stream_type>::value);
    }

    BOOST_FIXTURE_TEST_CASE(non_copy_assignable, default_ctx_fixture<5000>)
    {
      BOOST_TEST(not std::is_copy_assignable<stream_type>::value);
    }

    BOOST_FIXTURE_TEST_CASE(
        continuous_write, buffer_fixture<default_ctx_fixture<>>)
    {
      stream_type stream{io_service};

      for (auto&& buf : buffers) {
        stream.async_write_some(asio::buffer(buf), callback());
      }
      io_service.run();

      BOOST_TEST(result.size() == num_buffers);
      for (auto&& e : result) {
        BOOST_TEST(std::get<0>(e) == sys::error_code{});
        BOOST_TEST(std::get<1>(e) == buffer_size);
      }
      BOOST_TEST(stream.next_layer().written_data() == joined_buffers());
    }

    BOOST_FIXTURE_TEST_CASE(
        continuous_short_write, buffer_fixture<default_ctx_fixture<128>>)
    {
      stream_type stream{io_service, max_size};

      for (auto&& buf : buffers) {
        stream.async_write_some(asio::buffer(buf), callback());
      }
      io_service.run();

      BOOST_TEST(result.size() == num_buffers);
      for (auto&& e : result) {
        BOOST_TEST(std::get<0>(e) == sys::error_code{});
        BOOST_TEST(std::get<1>(e) == buffer_size);
      }
      BOOST_TEST(stream.next_layer().written_data() == joined_buffers());
    }

    BOOST_FIXTURE_TEST_CASE(
        error_in_continuous_write, buffer_fixture<default_ctx_fixture<128>>)
    {
      stream_type stream{io_service, max_size};
      auto const error = boost::asio::error::connection_refused;
      auto const writable_size = 1028;
      stream.next_layer().max_writable_size(writable_size, error);

      for (auto&& buf : buffers) {
        stream.async_write_some(asio::buffer(buf), callback());
      }
      io_service.run();

      auto expected = std::vector<std::tuple<sys::error_code, std::size_t>>(
            writable_size / buffer_size
          , std::make_tuple(sys::error_code{}, buffer_size));
      expected.emplace_back(error, writable_size % buffer_size);
      expected.resize(num_buffers, std::make_tuple(error, 0));
      BOOST_TEST_REQUIRE(result.size() == num_buffers);
      for (auto i = std::size_t{0}; i < num_buffers; ++i) {
        BOOST_TEST(std::get<0>(result[i]) == std::get<0>(expected[i]));
        BOOST_TEST(std::get<1>(result[i]) == std::get<1>(expected[i]));
      }
      BOOST_TEST(
          stream.next_layer().written_data() == joined_buffers(writable_size));
    }

    BOOST_FIXTURE_TEST_CASE(
        chained_write, buffer_fixture<default_ctx_fixture<128>>)
    {
      stream_type stream{io_service, max_size};

      stream.async_write_some(
          asio::buffer(buffers[0]), chained_callback(stream));
      io_service.run();

      BOOST_TEST(result.size() == num_buffers);
      for (auto&& e : result) {
        BOOST_TEST(std::get<0>(e) == sys::error_code{});
        BOOST_TEST(std::get<1>(e) == buffer_size);
      }
      BOOST_TEST(stream.next_layer().written_data() == joined_buffers());
    }

  BOOST_AUTO_TEST_SUITE_END() // default_context


  BOOST_AUTO_TEST_SUITE(io_service_context)

#if 0 // TODO: enhance write_queue_stream interface
    BOOST_AUTO_TEST_CASE(construct_from_stream)
    {
      asio::io_service io_service{};
      asio::io_service context{};
      auto max_size = std::size_t{5000};
      auto ec = make_error_code(sys::errc::bad_message);
      auto base_stream = stub_stream{io_service, max_size, ec};

      stream_type stream{std::move(base_stream), context};

      BOOST_TEST(&stream.get_io_service() == &io_service);
      BOOST_TEST(stream.next_layer().max_writable_size_per_write() == max_size);
      BOOST_TEST(stream.next_layer().error_code() == ec);
      BOOST_TEST(stream.next_layer().written_data().size() == 0);
    }

    BOOST_AUTO_TEST_CASE(construct_from_arguments)
    {
      asio::io_service io_service{};
      asio::io_service context{};
      auto max_size = std::size_t{3000};
      auto ec = make_error_code(sys::errc::no_stream_resources);

      stream_type stream{context, io_service, max_size, ec};

      BOOST_TEST(&stream.get_io_service() == &io_service);
      BOOST_TEST(stream.next_layer().max_writable_size_per_write() == max_size);
      BOOST_TEST(stream.next_layer().error_code() == ec);
      BOOST_TEST(stream.next_layer().written_data().size() == 0);
    }
#endif

  BOOST_AUTO_TEST_SUITE_END() // io_service_context


  BOOST_AUTO_TEST_SUITE(strand_context)

    BOOST_FIXTURE_TEST_CASE(construct_from_stream, strand_ctx_fixture<5000>)
    {
      auto ec = make_error_code(sys::errc::bad_message);
      auto base_stream = stub_stream{io_service, max_size, ec};

      stream_type stream{std::move(base_stream), strand};

      BOOST_TEST(&stream.get_io_service() == &io_service);
      BOOST_TEST(stream.next_layer().max_writable_size_per_write() == max_size);
      BOOST_TEST(stream.next_layer().error_code() == ec);
      BOOST_TEST(stream.next_layer().written_data().size() == 0);
    }

    BOOST_FIXTURE_TEST_CASE(construct_from_arguments, strand_ctx_fixture<3000>)
    {
      auto ec = make_error_code(sys::errc::no_stream_resources);

      stream_type stream{strand, io_service, max_size, ec};

      BOOST_TEST(&stream.get_io_service() == &io_service);
      BOOST_TEST(stream.next_layer().max_writable_size_per_write() == max_size);
      BOOST_TEST(stream.next_layer().error_code() == ec);
      BOOST_TEST(stream.next_layer().written_data().size() == 0);
    }

    BOOST_FIXTURE_TEST_CASE(non_copy_constructible, strand_ctx_fixture<5000>)
    {
      BOOST_TEST(not std::is_copy_constructible<stream_type>::value);
    }

    BOOST_FIXTURE_TEST_CASE(non_copy_assignable, strand_ctx_fixture<5000>)
    {
      BOOST_TEST(not std::is_copy_assignable<stream_type>::value);
    }

    BOOST_FIXTURE_TEST_CASE(
        continuous_write, buffer_fixture<strand_ctx_fixture<>>)
    {
      stream_type stream{strand, io_service};

      for (auto&& buf : buffers) {
        stream.async_write_some(asio::buffer(buf), callback());
      }
      io_service.run();

      BOOST_TEST(result.size() == num_buffers);
      for (auto&& e : result) {
        BOOST_TEST(std::get<0>(e) == sys::error_code{});
        BOOST_TEST(std::get<1>(e) == buffer_size);
      }
      BOOST_TEST(stream.next_layer().written_data() == joined_buffers());
    }

    BOOST_FIXTURE_TEST_CASE(
        continuous_short_write, buffer_fixture<strand_ctx_fixture<128>>)
    {
      stream_type stream{strand, io_service, max_size};

      for (auto&& buf : buffers) {
        stream.async_write_some(asio::buffer(buf), callback());
      }
      io_service.run();

      BOOST_TEST(result.size() == num_buffers);
      for (auto&& e : result) {
        BOOST_TEST(std::get<0>(e) == sys::error_code{});
        BOOST_TEST(std::get<1>(e) == buffer_size);
      }
      BOOST_TEST(stream.next_layer().written_data() == joined_buffers());
    }

    BOOST_FIXTURE_TEST_CASE(
        error_in_continuous_write, buffer_fixture<strand_ctx_fixture<128>>)
    {
      stream_type stream{strand, io_service, max_size};
      auto const error = boost::asio::error::connection_refused;
      auto const writable_size = 1028;
      stream.next_layer().max_writable_size(writable_size, error);

      for (auto&& buf : buffers) {
        stream.async_write_some(asio::buffer(buf), callback());
      }
      io_service.run();

      auto expected = std::vector<std::tuple<sys::error_code, std::size_t>>(
            writable_size / buffer_size
          , std::make_tuple(sys::error_code{}, buffer_size));
      expected.emplace_back(error, writable_size % buffer_size);
      expected.resize(num_buffers, std::make_tuple(error, 0));
      BOOST_TEST_REQUIRE(result.size() == num_buffers);
      for (auto i = std::size_t{0}; i < num_buffers; ++i) {
        BOOST_TEST(std::get<0>(result[i]) == std::get<0>(expected[i]));
        BOOST_TEST(std::get<1>(result[i]) == std::get<1>(expected[i]));
      }
      BOOST_TEST(
          stream.next_layer().written_data() == joined_buffers(writable_size));
    }

    BOOST_FIXTURE_TEST_CASE(
        chained_write, buffer_fixture<strand_ctx_fixture<128>>)
    {
      stream_type stream{strand, io_service, max_size};

      stream.async_write_some(
          asio::buffer(buffers[0]), chained_callback(stream));
      io_service.run();

      BOOST_TEST(result.size() == num_buffers);
      for (auto&& e : result) {
        BOOST_TEST(std::get<0>(e) == sys::error_code{});
        BOOST_TEST(std::get<1>(e) == buffer_size);
      }
      BOOST_TEST(stream.next_layer().written_data() == joined_buffers());
    }

  BOOST_AUTO_TEST_SUITE_END() // strand_context


  BOOST_AUTO_TEST_SUITE(original_context)

    BOOST_FIXTURE_TEST_CASE(construct_from_stream, original_ctx_fixture<5000>)
    {
      auto ec = make_error_code(sys::errc::bad_message);
      auto base_stream = stub_stream{io_service, max_size, ec};

      stream_type stream{std::move(base_stream)};

      BOOST_TEST(&stream.get_io_service() == &io_service);
      BOOST_TEST(stream.next_layer().max_writable_size_per_write() == max_size);
      BOOST_TEST(stream.next_layer().error_code() == ec);
      BOOST_TEST(stream.next_layer().written_data().size() == 0);
    }

    BOOST_FIXTURE_TEST_CASE(construct_from_arguments, original_ctx_fixture<3000>)
    {
      auto ec = make_error_code(sys::errc::no_stream_resources);

      stream_type stream{io_service, max_size, ec};

      BOOST_TEST(&stream.get_io_service() == &io_service);
      BOOST_TEST(stream.next_layer().max_writable_size_per_write() == max_size);
      BOOST_TEST(stream.next_layer().error_code() == ec);
      BOOST_TEST(stream.next_layer().written_data().size() == 0);
    }

    BOOST_FIXTURE_TEST_CASE(non_copy_constructible, original_ctx_fixture<5000>)
    {
      BOOST_TEST(not std::is_copy_constructible<stream_type>::value);
    }

    BOOST_FIXTURE_TEST_CASE(non_copy_assignable, original_ctx_fixture<5000>)
    {
      BOOST_TEST(not std::is_copy_assignable<stream_type>::value);
    }

    BOOST_FIXTURE_TEST_CASE(
        continuous_write, buffer_fixture<original_ctx_fixture<>>)
    {
      auto data = original_ctx::data{};
      stream_type stream{original_ctx{&data}, io_service};

      for (auto&& buf : buffers) {
        stream.async_write_some(asio::buffer(buf), callback());
      }
      io_service.run();

      auto const num_send = 2; // first send 512 byte and second send the reset.
      BOOST_TEST(data.alloc_counter == num_send);
      BOOST_TEST(data.dealloc_counter == num_send);
      BOOST_TEST(data.invoke_counter == num_send * 2);
      BOOST_TEST(data.continuation_counter == 0);
      BOOST_TEST(result.size() == num_buffers);
      for (auto&& e : result) {
        BOOST_TEST(std::get<0>(e) == sys::error_code{});
        BOOST_TEST(std::get<1>(e) == buffer_size);
      }
      BOOST_TEST(stream.next_layer().written_data() == joined_buffers());
    }

    BOOST_FIXTURE_TEST_CASE(
        continuous_short_write, buffer_fixture<original_ctx_fixture<128>>)
    {
      auto data = original_ctx::data{};
      stream_type stream{original_ctx{&data}, io_service, max_size};

      for (auto&& buf : buffers) {
        stream.async_write_some(asio::buffer(buf), callback());
      }
      io_service.run();

      auto const writable_size
        = stream.next_layer().max_writable_size_per_write();
      auto const total_buffer_size = std::size_t(num_buffers) * buffer_size;
      auto const num_send
        = (total_buffer_size + writable_size - 1) / writable_size;
      BOOST_TEST(data.alloc_counter == num_send);
      BOOST_TEST(data.dealloc_counter == num_send);
      BOOST_TEST(data.invoke_counter == num_send * 2);
      BOOST_TEST(data.continuation_counter == 0);
      BOOST_TEST(result.size() == num_buffers);
      for (auto&& e : result) {
        BOOST_TEST(std::get<0>(e) == sys::error_code{});
        BOOST_TEST(std::get<1>(e) == buffer_size);
      }
      BOOST_TEST(stream.next_layer().written_data() == joined_buffers());
    }

    BOOST_FIXTURE_TEST_CASE(
        error_in_continuous_write, buffer_fixture<original_ctx_fixture<128>>)
    {
      auto data = original_ctx::data{};
      stream_type stream{original_ctx{&data}, io_service, max_size};
      auto const error = boost::asio::error::connection_refused;
      auto const writable_size = 1028;
      stream.next_layer().max_writable_size(writable_size, error);

      for (auto&& buf : buffers) {
        stream.async_write_some(asio::buffer(buf), callback());
      }
      io_service.run();

      auto expected = std::vector<std::tuple<sys::error_code, std::size_t>>(
            writable_size / buffer_size
          , std::make_tuple(sys::error_code{}, buffer_size));
      expected.emplace_back(error, writable_size % buffer_size);
      expected.resize(num_buffers, std::make_tuple(error, 0));
      BOOST_TEST_REQUIRE(result.size() == num_buffers);
      for (auto i = std::size_t{0}; i < num_buffers; ++i) {
        BOOST_TEST(std::get<0>(result[i]) == std::get<0>(expected[i]));
        BOOST_TEST(std::get<1>(result[i]) == std::get<1>(expected[i]));
      }
      BOOST_TEST(
          stream.next_layer().written_data() == joined_buffers(writable_size));
      auto const one_writable_size
        = stream.next_layer().max_writable_size_per_write();
      auto const num_send
        = (writable_size + one_writable_size - 1) / one_writable_size;
      BOOST_TEST(data.alloc_counter == num_send);
      BOOST_TEST(data.dealloc_counter == num_send);
      BOOST_TEST(data.invoke_counter == num_send * 2);
      BOOST_TEST(data.continuation_counter == 0);
    }

  BOOST_AUTO_TEST_SUITE_END() // original_context


  BOOST_AUTO_TEST_SUITE(multi_thread)

    using fixture = buffer_fixture<strand_ctx_fixture<128>, 79>;
    constexpr auto num_threads = std::size_t{4};

    BOOST_FIXTURE_TEST_CASE(continuous_write, fixture)
    {
      stream_type stream{strand, io_service, max_size};
      auto result = std::vector<
        std::vector<std::tuple<sys::error_code, std::size_t>>
      >(num_threads);

      auto futures = std::vector<std::future<void>>{};
      boost::barrier barrier(num_threads);
      for (auto i = 0; i < num_threads; ++i) {
        futures.push_back(std::async(std::launch::async, [&, i]{
          io_service.post([&, i]{
            for (auto&& buf : buffers) {
              strand.dispatch([&, i]{
                stream.async_write_some(
                      asio::buffer(buf)
                    , [&, i](sys::error_code const& ec, std::size_t const size) {
                        result[i].emplace_back(ec, size);
                });
              });
            }
          });
          barrier.wait();
          io_service.run();
        }));
      }
      for (auto&& e : futures) e.get();

      for (auto i = 0; i < num_threads; ++i) {
        BOOST_TEST(result[i].size() == num_buffers);
        for (auto&& e : result[i]) {
          BOOST_TEST(std::get<0>(e) == sys::error_code{});
          BOOST_TEST(std::get<1>(e) == buffer_size);
        }
      }
      auto const& data = stream.next_layer().written_data();
      BOOST_TEST(data.size() % buffer_size == 0);
      for (auto i = std::size_t{}; i < data.size(); i += buffer_size) {
        BOOST_TEST(
            std::count(&data[i], &data[i] + buffer_size, data[i])
         == buffer_size);
      }
    }


    BOOST_FIXTURE_TEST_CASE(chained_write, fixture)
    {
      stream_type stream{strand, io_service, max_size};
      auto result = std::vector<
        std::vector<std::tuple<sys::error_code, std::size_t>>
      >(num_threads);

      auto counter = std::vector<std::size_t>(num_threads, 0);
      auto func = std::vector<
        std::function<void(sys::error_code const& ec, std::size_t const size)>
      >(num_threads);
      auto futures = std::vector<std::future<void>>{};
      boost::barrier barrier(num_threads);
      for (auto i = 0; i < num_threads; ++i) {
        func[i] = [&, i](sys::error_code const& ec, std::size_t const size) {
          result[i].emplace_back(ec, size);
          if (++counter[i] != num_buffers) {
            stream.async_write_some(
                asio::buffer(buffers[counter[i]]), strand.wrap(func[i]));
          }
        };
        futures.push_back(std::async(std::launch::async, [&, i]{
          io_service.post(strand.wrap([&, i]{
            stream.async_write_some(
                asio::buffer(buffers[0]), strand.wrap(func[i]));
          }));
          barrier.wait();
          io_service.run();
        }));
      }
      for (auto&& e : futures) e.get();

      for (auto i = 0; i < num_threads; ++i) {
        BOOST_TEST(result[i].size() == num_buffers);
        for (auto&& e : result[i]) {
          BOOST_TEST(std::get<0>(e) == sys::error_code{});
          BOOST_TEST(std::get<1>(e) == buffer_size);
        }
      }
      auto const& data = stream.next_layer().written_data();
      BOOST_TEST(data.size() % buffer_size == 0);
      for (auto i = std::size_t{}; i < data.size(); i += buffer_size) {
        BOOST_TEST(
            std::count(&data[i], &data[i] + buffer_size, data[i])
         == buffer_size);
      }
    }

  BOOST_AUTO_TEST_SUITE_END() // multi_thread


BOOST_AUTO_TEST_SUITE_END() // write_queue_stream

// vim: set et ts=2 sw=2 sts=0 :
