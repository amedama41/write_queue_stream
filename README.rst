==========================
Write Queue Stream Library
==========================

Introduction
============

This library provide a Boost.Asio's AsyncWriteStream ``write_queue_stream`` for C++11 or later.

Users of the write_queue_stream can start multiple writing asynchronous operations
before other asynchronous writes are not completed yet.
All of writting data is queued in a write_queue_stream object, and written in order.

The write_queue_stream's ``async_write_some`` is a composed operation such as ``asio::async_write``.
So this function use inner handler. This handler will be called in the context which is passed 
to the write_queue_stream's constructor (or is default constructed).
The context type is specified write_queue_stream class's template parameter.

.. note:: Other composed operations use the context same as handler passed to the operations.

Other semantics, such as thread safty, object lifetime, or etc. are same as other Boost.Asio's IOObjects.

Install
=======

Through the path to the directory. ``/path/to/include/canard``

This library is a header only library.

Copyrights
==========

Copyright (c) 2016 amedama41.

Distributed under the Boost Software License, Version 1.0. (See accompanying
file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

